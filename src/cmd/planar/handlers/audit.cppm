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
///   - `audit commits` landed at task 6277. THE ENTRY THAT USED TO SIT
///     HERE WAS WRONG and is worth recording rather than deleting: it
///     said this leaf "rests on the GIT-WALK half of
///     `engine.runtime.sessioncommits` … it is about walking git, not
///     reading rows", and grouped it with `capture commits` and `bench
///     harvest` as blocked on a process-spawn seam.
///
///     The oracle handler is 78 lines and spawns nothing. It calls
///     `session.getById`, `task.show`, `listFiltered` and `writeJsonList`
///     — all pure SQL over rows an EARLIER `capture commits` run wrote.
///     Walking git is what FILLS `session_commits`; this leaf only
///     queries it. `capture commits` and `bench harvest` remain genuinely
///     blocked. The mistake was inferring a leaf's dependencies from its
///     MODULE's rather than from its own handler, and it survived three
///     files and two tasks before task 6272 caught it.
///   - `audit publish-decision` posts a decision body to the operational
///     plane, so it needs an adapter INSTANCE — the auth-resolving adapter
///     factory that `ext create` / `ext propagate` / `ext test` and the
///     three `sync` leaves are all still waiting on.
///   - `audit handoff-readiness` landed at task 6329, and THE ENTRY THAT
///     USED TO SIT HERE — "the one that is merely LARGE rather than
///     blocked" — was wrong in the same direction as the `audit commits`
///     entry above it. The oracle handler is 101 lines. It is the
///     SMALLEST leaf in this family, not the largest, and its one
///     dependency (`engine.runtime.resumecheck`) was already in the tree
///     when the note was written. Recorded rather than deleted because
///     this is now the second time this file has over-stated a leaf's
///     cost from something other than the leaf's own handler.
///
///     `audit publish-decision` is the family's only remaining leaf.
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

/// @brief Handle `planar audit commits [--session N] [--task N]
/// [--json|--shas]`.
///
/// Port target: zig/src/cmd/planar/handlers/audit/commits.zig (plan 996,
/// task 6277). See this file's header for why the "blocked on the git
/// walk" note that used to stand here was wrong.
///
/// ## THREE OUTPUT SHAPES, AND `--json` + `--shas` IS A REFUSAL
///
/// `--json` and `--shas` are mutually exclusive and combining them exits 2
/// with `cannot combine --json with --shas`. That check runs FIRST — before
/// `--session` / `--task` are resolved — so `audit commits --json --shas
/// --session 99` reports the combination, not the missing session
/// (oracle-captured; the natural port validates ids first and answers the
/// other message).
///
/// The three shapes disagree about the empty case and all three are
/// pinned:
///   - text  -> the HEADER ROW ALONE. Not `(no commits)`, not zero bytes.
///   - json  -> `[]`.
///   - shas  -> ZERO BYTES. The only one of the three that prints nothing.
///
/// ## `--session` AND `--task` ARE VALIDATED FOR EXISTENCE, IN THAT ORDER
///
/// Each is looked up before the listing runs, and a well-formed id naming
/// nothing REFUSES at exit 1 (`session 99 not found` / `task 99 not
/// found`) rather than listing empty. Session is checked first:
/// `--session 99 --task 99` reports the session. Contrast the sibling
/// `audit trail 99`, which answers exit 0 with an empty trail for a
/// nonexistent entity — same family, opposite posture, both captured.
///
/// A task that EXISTS but carries no claim is not an error: exit 0 and the
/// empty shape for whichever output mode is active.
///
/// ## `--task` DROPS ROWS THAT `--session` KEEPS
///
/// `session_commits` has no `task_id`; the task predicate joins
/// `agent_work_claims` through `claim_id`. The join is INNER, so filtering
/// by task silently excludes every commit recorded outside a claim window
/// (`claim_id is null`) — rows the same session lists happily without the
/// flag. That is the oracle's behaviour, captured against a fixture whose
/// three rows are split exactly on this, and it is not a bug this port may
/// round off.
/// @param ctx The process context.
/// @param args The parsed command line.
/// @return Success after writing, or `invalid_input` (exit 2) for the flag
/// combination, `not_found` (exit 1) for an unknown session or task, or
/// `generic_failure` (exit 1) on an engine failure.
export auto audit_commits(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar audit trail [<entity-id>] [--kind K] [--grep P]
/// [--link N] [--json]`.
///
/// Two forms selected by `--link`; see this file's header for which wins
/// and what each reads.
/// @param ctx The process context.
/// @param args The parsed command line.
/// @return Success after writing the trail, or the refusal.
export auto audit_trail(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar audit handoff-readiness [--threshold N] [--json]`.
///
/// Port target: zig/src/cmd/planar/handlers/audit/handoff_readiness.zig
/// (plan 996, task 6329).
///
/// Scans every task whose status is `todo`, `doing` or `blocked`, runs
/// `resumecheck::validate` over each, and reports the pass rate.
///
/// Three behaviours that a natural port gets wrong, all oracle-captured:
///
///   - **The scan is GLOBAL.** No `--scope` flag, no scope predicate. It
///     counts in-flight tasks across every association in the database.
///   - **The threshold gate TRUNCATES while the display ROUNDS.** `ok` is
///     `int64(pct) >= threshold`; both rendered forms round. At 2 of 3
///     tasks passing the text arm prints `FAIL: threshold not met (67% <
///     67%)`, which is self-contradictory on its face and is nonetheless
///     the contract. Reproduced exactly.
///   - **The FAIL list is not gated on the verdict.** A passing run at a
///     low threshold still lists every failing task, then prints `OK:`.
///
///   - An EMPTY database is `ok:true` at any threshold, including 101 —
///     `total == 0` short-circuits ahead of the comparison, so the
///     `percentage:0.00` it reports alongside is not what was tested.
///
/// The refusal is emitted AFTER the payload, like `resume validate`: the
/// JSON or text body is written unconditionally and the exit-1 refusal
/// (`handoff readiness below threshold`) follows on stderr.
/// @param ctx The process context.
/// @param args The parsed command line.
/// @return Success when the threshold is met, or the exit-1 refusal.
export auto audit_handoff_readiness(context& ctx, const cliapp::parsed_args& args) -> handler_result;

} // namespace planar::cmd::handlers
