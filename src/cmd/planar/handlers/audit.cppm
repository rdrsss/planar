/// @file audit.cppm
/// @brief `planar.cmd.planar.handlers.audit` — the `planar audit session`
/// leaf (plan 996, task 6090).
///
/// Port of zig/src/cmd/planar/handlers/audit/session.zig.
///
/// ## One of the family's five leaves, and the other four are BLOCKED
///
/// Named rather than left to be inferred from the dispatch table:
///
///   - `audit commits` rests on `engine.runtime.sessioncommits`
///     (1205 Zig lines whose whole job is shelling `git` through
///     subprocess calls — revision walks, ref resolution, per-commit
///     metadata). No process-spawn abstraction exists in this tree, and
///     `capture commits` and `bench harvest` are already deferred on the
///     same dependency.
///   - `audit publish-decision` posts a decision body to the operational
///     plane, so it needs an adapter INSTANCE — the auth-resolving adapter
///     factory that `ext create` / `ext propagate` / `ext test` and the
///     three `sync` leaves are all still waiting on.
///   - `audit handoff-readiness` and `audit trail` are the two that are
///     merely LARGE rather than blocked. `trail` in particular is a
///     563-line handler that additionally needs the `external_links` /
///     `sync_events` read path for its link-id arm, which no module in
///     this tree has; its ENTITY arm is served by
///     `planar.engine.runtime.audit_trail`'s `for_entity` /
///     `for_entity_with_links` / `for_entity_grep`, which DID land in this
///     cycle and are sitting unwired for it.
///
/// ## Two different exit codes for two different bad ids
///
/// Both captured from live runs, and the pair is the reason this note
/// exists — the naive reading gives both the same code:
///
///   - a NON-INTEGER id refuses at exit 2:
///     `session id must be an integer, got 'abc'`
///   - an integer id no session has refuses at exit 1:
///     `session 99 not found`
///
/// `--json` does NOT change either refusal: both still go to stderr as
/// plain `error: <body>` lines with no JSON envelope.
///
/// ## `--json` drops null optionals; the sibling `health hygiene` does not
///
/// This verb stringifies with `emit_null_optional_fields = false`, so an
/// unbound session omits `task_id` entirely and an active one omits
/// `ended_at` — they are absent keys, not `null` values. `health hygiene`
/// uses the default options and emits `"parent_plan_id":null`. Two verbs,
/// two conventions; both were captured, neither was inferred from the
/// other.
module;

export module planar.cmd.planar.handlers.audit;

import std;
import planar.cliapp.args;
import planar.cmd.planar.context;
import planar.cmd.planar.handler;

namespace planar::cmd::handlers {

/// @brief Handle `planar audit session <session-id> [--json]`.
/// @param ctx The process context.
/// @param args The parsed command line.
/// @return Success after writing the timeline, or the refusal.
export auto audit_session(context& ctx, const cliapp::parsed_args& args) -> handler_result;

} // namespace planar::cmd::handlers
