/// @file scope.cppm
/// @brief `planar.cmd.planar.scope` — the handler-facing scope-resolution
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
/// OMISSION, named rather than silently dropped: the worktree gate.
/// zig/src/cmd/planar/scope.zig probes for a git worktree on every resolve
/// so zig/src/cmd/planar/worktree_gate.zig can refuse planning verbs run
/// from inside one — and `--scope` deliberately does NOT override that
/// refusal. `planar.engine.identity.scope`'s port carries no
/// worktree-detection fields at all (its own header says so), so there is
/// nothing here to forward; the gate needs a git-subprocess seam that does
/// not exist anywhere in this tree yet, the same dependency that deferred
/// `bench harvest`, `capture commits` and `workflow run`. Deferred WITH its
/// dependency, not quietly skipped: a verb ported later that relies on the
/// gate must not assume this module already enforces it.
module;

export module planar.cmd.planar.scope;

import std;
import planar.engine.identity;
import planar.cmd.planar.context;
import planar.cmd.planar.exit;

namespace planar::cmd {

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

} // namespace planar::cmd
