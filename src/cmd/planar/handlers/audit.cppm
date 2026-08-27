/// @file audit.cppm
/// @brief `planar.cmd.planar.handlers.audit` — the `planar audit session`
/// leaf (plan 996, task 6090).
///
/// Port of zig/src/cmd/planar/handlers/audit/session.zig.
///
/// ## Two of the family's five leaves, and the other three are BLOCKED
///
/// `audit session` landed at task 6090; `audit trail` at task 6262. Named
/// rather than left to be inferred from the dispatch table:
///
///   - `audit commits` rests on the GIT-WALK half of
///     `engine.runtime.sessioncommits` — revision walks, ref resolution,
///     per-commit metadata, all through subprocess calls. No process-spawn
///     abstraction exists in this tree, and `capture commits` and `bench
///     harvest` are already deferred on the same dependency. Task 6262
///     landed that module's READ half (`list_for_sessions`, pure SQL) for
///     `audit trail`'s commits fold-in; that does NOT unblock this leaf,
///     which is about walking git, not reading rows.
///   - `audit publish-decision` posts a decision body to the operational
///     plane, so it needs an adapter INSTANCE — the auth-resolving adapter
///     factory that `ext create` / `ext propagate` / `ext test` and the
///     three `sync` leaves are all still waiting on.
///   - `audit handoff-readiness` is the one that is merely LARGE rather
///     than blocked.
///
/// ## `audit trail` IS TWO VERBS SHARING A NAME, AND `--link` WINS
///
/// The entity form reads `audit_log`; the link form reads `external_links`
/// + `sync_events` and never touches `audit_log` at all. They share only
/// the commits and agent-activity fold-ins.
///
/// `--link` is checked FIRST and returns unconditionally, so
/// `audit trail 1 --link 1` runs the LINK form and the positional is
/// silently ignored — it does not refuse, and it does not merge. Captured
/// from the oracle; asserted, because the natural port checks the
/// positional first and would answer the other verb.
///
/// ## THREE BAD-INPUT SHAPES, THREE ANSWERS, AND ONE OF THEM IS SUCCESS
///
///   - `audit trail` with neither         -> exit 2, `audit trail requires
///                                          <entity-id> or --link <id>`
///   - `audit trail abc`                  -> exit 2, `entity id must be an
///                                          integer, got 'abc'`
///   - `audit trail --link abc`           -> exit 2, `invalid --link value
///                                          'abc'`  (its OWN wording, not
///                                          the `entity_id_arg` one)
///   - `audit trail --link 99`            -> exit 1, `external link 99 not
///                                          found`
///   - `audit trail 99` (no such task)    -> EXIT 0, `(0 entries)`.
///
/// That last one is the trap. The SIBLING leaf in this same file answers
/// exit 1 for `audit session 99`. Same family, same shape of bad id, one
/// refuses and one succeeds — because an entity with no history is an
/// answer here and a missing session is not. Both are pinned in one test
/// case so a port cannot "harmonise" them.
///
/// ## `--kind` IS NOT VALIDATED, AND `--grep` CHANGES WHICH QUERY RUNS
///
/// `--kind bogus` is exit 0 with zero entries (contrast `search --kind
/// bogus`, which refuses at exit 2). `--kind ""` renders a header reading
/// `audit trail for :1`.
///
/// `--grep` does NOT filter the default result set — it selects a
/// DIFFERENT engine function. Without it the leaf calls
/// `for_entity_with_links`, which widens by one `entity_links` hop; with it
/// the leaf calls `for_entity_grep`, which does NOT widen. So adding
/// `--grep` can drop rows that no grep pattern excluded, purely because the
/// linked entity's history is no longer in scope. Captured on a fixture
/// where a linked decision's row vanishes under `--grep ''`.
///
/// ## ONE JSON PAYLOAD, TWO NULL CONVENTIONS
///
/// In the SAME document: `entries[]` and `agent_activity` OMIT unset
/// optionals, and `commits[]` emits them as explicit `null`. That is not a
/// transcription slip — the commits rows come from a different renderer in
/// the oracle. Asserted as exact payloads.
///
/// And `sync_events[].evidence` is spliced in RAW: the `context_json`
/// column's text becomes a JSON *value*, so `{"token":"abc"}` renders as an
/// object, not as an escaped string. A port that ran it through the string
/// escaper produces valid JSON with the wrong shape, which no
/// "parses as JSON" assertion would catch.
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

/// @brief Handle `planar audit trail [<entity-id>] [--kind K] [--grep P]
/// [--link N] [--json]`.
///
/// Two forms selected by `--link`; see this file's header for which wins
/// and what each reads.
/// @param ctx The process context.
/// @param args The parsed command line.
/// @return Success after writing the trail, or the refusal.
export auto audit_trail(context& ctx, const cliapp::parsed_args& args) -> handler_result;

} // namespace planar::cmd::handlers
