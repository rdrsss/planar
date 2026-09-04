/// @file scope.cppm
/// @brief `planar.cmd.planar_ext.scope` — the handler-facing scope-resolution
/// entry point (plan 996, task 6105).
///
/// Port target: zig/src/cmd/planar/scope.zig. That file exists because the
/// engine owns the RULE (which project matches this cwd, what the guard
/// refuses) while the cmd layer owns the CLI concerns wrapped around it —
/// `--scope` precedence, where the cwd comes from, and forwarding to the
/// lazy DB. The same split holds here, with one simplification and one
/// deliberate omission.
///
/// SIMPLIFICATION: `--scope` precedence is already inside
/// `planar.engine.identity.scope::resolve_for_write`, which takes the flag
/// value as a parameter (it threads an explicit flag through verbatim, no
/// DB lookup — see that function's doc comment). So this module does not
/// re-implement the precedence; it supplies the two things the engine
/// cannot get for itself, `context::ensure_db()` and `context::cwd()`, and
/// maps the engine's `scope_error` onto this binary's exit-code buckets.
///
/// FORMER OMISSION, now CLOSED (task 6137): the worktree gate. This header
/// used to record it as deferred WITH its git-subprocess dependency, and
/// task 6135 turned that deferral from theoretical into observable —
/// planning verbs succeeded from inside a worktree with a real row written,
/// where the oracle refuses. The seam landed as `planar.git` and the gate
/// as `planar.cmd.planar.worktree_gate`, wired into `dispatch::run` ahead
/// of the parser. It does NOT run through this module: the gate consults
/// cwd-derived state ONLY and `--scope` deliberately does not override it,
/// so routing it through a function whose whole job is `--scope`
/// precedence would have invited exactly the override the rule forbids.
module;

export module planar.cmd.planar_ext.scope;

import std;
import planar.db;
import planar.engine.identity;
import planar.cmd.planar_ext.context;
import planar.cmd.planar_ext.exit;

namespace planar::cmd::ext {

/// @brief The membership-aware cross-scope guard.
///
/// Delegates to `engine::identity::check_scope_guard` first. On a refusal it
/// applies exactly one widening: an entity at `repo:<project>` is allowed
/// when the operator's write scope is an ASSOCIATION that the project is a
/// member of. Every other refusal stands, and the reverse direction (a repo
/// write scope reaching an association entity) is never widened.
///
/// Lived in `planar.cmd.planar_ext.handlers.sync` until task 6303, which gave it
/// its second caller family (`feedback triage set`). It sits here rather than
/// in either handler because a handler importing another handler is the
/// `cmd_* -> cmd_*` edge D18 prohibits, and because every caller invokes
/// `resolve_write_scope` immediately before it — the guard's second argument
/// is that call's result.
///
/// NOTE the refusal MESSAGE is deliberately not here. `sync` says "target is
/// outside the operator write scope", `feedback triage set` says "Refusing
/// cross-scope write; pass --scope ...", and `task` says a third thing; all
/// three were captured from the oracle and are not one string.
/// @param conn An open, migrated database connection.
/// @param entity_scope The entity's scope label (unset = global).
/// @param write_scope The operator's resolved write scope (unset = global).
/// @return True when the write is allowed, false when refused.
export auto guard_with_membership(db::connection& conn, std::optional<std::string_view> entity_scope,
                                  std::optional<std::string_view> write_scope) -> bool;

/// @brief Resolve the write scope for a mutating verb: the `--scope` value
/// when the operator passed one, otherwise the cwd-derived scope.
///
/// Opens the database as a side effect (via `context::ensure_db`), because
/// deriving a scope from a working directory is a `projects` /
/// `associations` query. A verb that does not need a scope must not call
/// this, or it forfeits the lazy-DB rule for no reason.
/// @param ctx The invocation context.
/// @param scope_flag The raw `--scope` value, when passed.
/// @param verb The verb name to lead a failure message with, e.g.
/// `"annotate add"` — the Zig handler interpolates its own verb name into
/// that message, so it cannot be a constant here.
/// @return The resolved write scope, or the failure as a `domain_error`
/// (`scope_mismatch` maps to exit 5, an invalid cwd to exit 2, a SQL
/// failure to exit 1).
export auto resolve_write_scope(context& ctx, std::optional<std::string_view> scope_flag, std::string_view verb)
    -> std::expected<engine::identity::write_scope_resolution, domain_error>;

/// @brief Map an engine `scope_error` onto this binary's exit-code bucket
/// and message. Exposed because more than one verb family needs the same
/// mapping and a second copy would be the drift D19 exists to prevent.
/// @param err The engine error.
/// @param verb The verb name to lead the message with, e.g. `"annotate add"`.
/// @return The mapped failure.
export auto map_scope_error(engine::identity::scope_error err, std::string_view verb) -> domain_error;

/// @brief Map a `resolve_for_write` failure, which may carry the
/// meta-workspace ambiguity detail, onto this binary's exit-code bucket.
///
/// The meta-workspace refusal (task 6134) does NOT use `map_scope_error`'s
/// generic "resolving scope failed: \<Name\>" shape: its message names the
/// two `--scope` values the operator may choose between, and the oracle
/// renders it verbatim. Everything else delegates.
/// @param failure The engine failure.
/// @param cwd The cwd to interpolate into the ambiguity message.
/// @param verb The verb name to lead a generic message with.
/// @return The mapped failure.
export auto map_scope_failure(const engine::identity::write_scope_failure& failure, std::string_view cwd, std::string_view verb)
    -> domain_error;

/// @brief Resolve the cwd-derived READ scope set for a listing verb, as the
/// slug labels an engine list filter takes (task 6141).
///
/// Call this ONLY on the no-`--scope` path. When the operator passed
/// `--scope`, the oracle's listing handlers bypass the read set entirely and
/// hand the raw flag value to the engine, which resolves it and reports its
/// own `SlugNotFound` — a different message and a different code path from
/// this one. Routing an explicit flag through here would change the error
/// text on every unknown `--scope`.
///
/// An EMPTY read set is turned into the oracle's refusal here rather than
/// being returned to the caller, because every caller must refuse and a
/// caller that forgot would list every scope in the database. That is the
/// "optional argument silently taking its default" shape in read-verb form:
/// exit 0, plausible rows, wrong rows.
/// @param ctx The invocation context.
/// @return The filter slugs (never empty on success), or the refusal as a
/// `domain_error` (exit 1 for both the empty set and a SQL failure).
export auto resolve_read_scope_slugs(context& ctx) -> std::expected<std::vector<std::string>, domain_error>;

} // namespace planar::cmd
