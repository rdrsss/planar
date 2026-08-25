/// @file promotion.cppm
/// @brief `planar.engine.promotion` — scope promotion and demotion for
/// promotable planning entities, plus the `promote` / `demote` verbs'
/// operator-facing output envelopes (plan 996 task 6094).
///
/// Behavior-preserving port (D2) of zig/src/engine/promotion.zig together
/// with the output half of zig/src/cmd/planar/handlers/promote.zig and
/// handlers/demote.zig. Every expectation in promotion.t.cpp was derived
/// by RUNNING `zig/zig-out/bin/planar promote|demote` against a scratch
/// database (`PLANAR_DB=/tmp/... PLANAR_CONFIG_PATH=/tmp/...`), never by
/// reading `--help` text.
///
/// Why the render functions live here rather than in a `cmd_*` module:
/// there is no layer-3 `cmd_*` module in the C++ tree yet (M4 ports engine
/// buckets; the binary lands later). The alternative to colocating them
/// was to drop the byte-level JSON/text parity assertions entirely until a
/// `cmd/` module exists, which would leave the two leaves this module
/// covers with no oracle-pinned output contract at all. They are pure
/// string builders over this module's own `scope_info` type, take no
/// database handle, and are trivially liftable into a handler later.
///
/// ## Deliberate omissions, with rationale
///
/// - **`policy.audit.record`.** The Zig original writes a BEST-EFFORT
///   `audit_log` row (verb `status_change`) after each successful scope
///   UPDATE — `catch {}`, deliberately, so a failed audit row cannot roll
///   back an already-committed scope change. The layer-1 `planar.policy`
///   module exists as of task 6100 and `engine_planning`/
///   `engine_identity` write through it, but those sites all use the
///   `try`-propagate shape. This bucket's two sites are the tree's only
///   best-effort ones, and wiring them by reflex alongside the rest would
///   have quietly made a failed audit write fail `promote`. Left for its
///   own cycle; see CMakeLists.txt.
/// - **The cross-scope guard.** VERIFIED EMPIRICALLY against the oracle,
///   not assumed: with the cwd project joined to association `alpha` and
///   a plan living in association `beta`,
///   `planar demote plan:1 --json` and `planar promote plan:1 --to beta
///   --json` both SUCCEED (exit 0). The Zig `promote`/`demote` path
///   consults no guard at any layer. Whether it should is Planar task
///   6075, an OPEN operator decision; this port pins the observed
///   behavior and adds no guard coverage of its own.
module;

export module planar.engine.promotion;

import std;
import planar.db;

namespace planar::engine::promotion {

/// @brief Error surface for this module's fallible operations. Mirrors
/// zig's `promotion.Error` (minus its allocator/audit arms, which have no
/// C++ analogue).
export enum class promote_error : std::uint8_t {
  not_found,         ///< The entity id does not exist in its table.
  invalid_scope,     ///< The entity kind is not promotable.
  scope_unchanged,   ///< The entity is already at the requested scope.
  unsupported_scope, ///< A `repo:<slug>` target was requested (reserved).
  slug_not_found,    ///< The target association slug does not exist.
  query_failed,      ///< An underlying SQL statement failed.
};

/// @brief A promotable entity's stored scope, as the `promote`/`demote`
/// handlers read it before and after the mutation. Mirrors zig's
/// `promotion.ScopeInfo`.
export struct scope_info {
  std::string                 scope_kind; ///< The stored `scope_kind` column text.
  std::optional<std::int64_t> scope_id;   ///< The stored `scope_id` column, unset when SQL NULL.
};

/// @brief Arguments to `promote`. Mirrors zig's `promotion.PromoteArgs`.
export struct promote_args {
  std::string_view kind;     ///< Entity kind: plan, task, question, test_scenario, artifact, decision.
  std::int64_t     id;       ///< The entity's row id.
  std::string_view to_scope; ///< An association slug, or the literal `"global"` to demote.
};

/// @brief Promote or demote an entity to `args.to_scope`.
///
/// Order of operations is load-bearing and matches the Zig original
/// exactly (confirmed against the oracle: `promote plan:1 --to repo:x`
/// reports `repo: scopes are not supported` even when `x` is not a real
/// project, i.e. the prefix check precedes any lookup):
///   1. Unknown entity kind -> `invalid_scope`.
///   2. `to_scope` starting with `"repo:"` -> `unsupported_scope`.
///   3. `to_scope == "global"` -> delegate to the demote path.
///   4. Resolve the association slug -> `slug_not_found` when absent.
///   5. Read the entity's current scope -> `not_found` when absent.
///   6. Already at the target association -> `scope_unchanged`.
///   7. UPDATE `scope_kind`/`scope_id`/`updated_at`.
///
/// @param conn An open, migrated database connection.
/// @param args The entity ref and target scope.
/// @return Success, or the first failing condition above.
export auto promote(db::connection& conn, const promote_args& args) -> std::expected<void, promote_error>;

/// @brief Demote an entity to global scope (`scope_kind='global'`,
/// `scope_id=NULL`). Mirrors zig's `promotion.demote`.
///
/// @param conn An open, migrated database connection.
/// @param kind Entity kind (see `promote_args::kind`).
/// @param id The entity's row id.
/// @return Success, `promote_error::invalid_scope` for an unknown kind,
/// `promote_error::not_found` when the id is absent, or
/// `promote_error::scope_unchanged` when the entity is already global.
export auto demote(db::connection& conn, std::string_view kind, std::int64_t id) -> std::expected<void, promote_error>;

/// @brief Read `(scope_kind, scope_id)` for a promotable entity. Mirrors
/// zig's `promotion.readEntityScope` — the shared reader both handlers
/// call before AND after the mutation so the output envelope can name the
/// previous scope.
///
/// This pre-read is why the oracle's `promote plan:999 --to alpha` reports
/// `reading entity scope: NotFound` rather than the engine's own
/// `no plan with id 999`: the handler's pre-read fails first, so the
/// engine's message for that case is unreachable through the CLI (pinned
/// in promotion.t.cpp).
///
/// @param conn An open, migrated database connection.
/// @param kind Entity kind (see `promote_args::kind`).
/// @param id The entity's row id.
/// @return The stored scope, `promote_error::invalid_scope` for an
/// unknown kind, or `promote_error::not_found` when the id is absent.
export auto read_entity_scope(db::connection& conn, std::string_view kind, std::int64_t id)
    -> std::expected<scope_info, promote_error>;

/// @brief Render the `--json` envelope both `promote` and `demote` emit on
/// success. Byte-identical to the oracle's, including key order and the
/// absence of any interior whitespace, and INCLUDING the trailing newline the
/// oracle writes (handlers/promote.zig:117 prints `}}\n` from the same call
/// that printed the body). The caller appends nothing.
///
/// Oracle-captured shape:
/// `{"ok":true,"kind":"plan","id":1,"scope_kind":"association","scope_id":1,`
/// `"previous_scope_kind":"global","previous_scope_id":null}`
///
/// @param kind The entity kind text.
/// @param id The entity's row id.
/// @param current The scope read AFTER the mutation.
/// @param previous The scope read BEFORE the mutation.
/// @return The complete stdout payload: the single-line JSON object WITH
/// its trailing newline.
export auto render_scope_change_json(std::string_view kind, std::int64_t id, const scope_info& current,
                                     const scope_info& previous) -> std::string;

/// @brief Render `promote`'s non-`--json` success line. Byte-identical to
/// the oracle's, including the DOUBLE space before `(was:` and the trailing
/// newline. The caller appends nothing.
///
/// Oracle-captured shape:
/// `plan:1 promoted to association beta  (was: association:1)`
///
/// @param kind The entity kind text.
/// @param id The entity's row id.
/// @param to_scope The `--to` value as the operator typed it (NOT the
/// resolved association id — the oracle echoes the raw flag).
/// @param previous The scope read BEFORE the mutation.
/// @return The complete stdout payload: the success line WITH its newline.
export auto render_promote_text(std::string_view kind, std::int64_t id, std::string_view to_scope, const scope_info& previous)
    -> std::string;

/// @brief Render `demote`'s non-`--json` success line. Byte-identical to
/// the oracle's, including the DOUBLE space before `(was:` and the trailing
/// newline. The caller appends nothing.
///
/// Oracle-captured shape: `plan:1 demoted to global  (was: association:1)`
///
/// @param kind The entity kind text.
/// @param id The entity's row id.
/// @param previous The scope read BEFORE the mutation.
/// @return The complete stdout payload: the success line WITH its newline.
export auto render_demote_text(std::string_view kind, std::int64_t id, const scope_info& previous) -> std::string;

} // namespace planar::engine::promotion
