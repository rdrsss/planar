/// @file resume.cppm
/// @brief `planar.cmd.planar.handlers.resume` — `planar resume validate`,
/// the no-id cwd-scope derivation, and the 8-section packet body (plan 996,
/// tasks 6040/6452/6455).
///
/// Port target: zig/src/cmd/planar/handlers/resume/validate.zig and the
/// no-id branch of zig/src/cmd/planar/handlers/resume/cmd.zig's `handle`.
///
/// ## `resume validate` is the gate the durable-interruption contract rests on
///
/// The orchestrator asks `planar resume validate <task-id> --json` whether
/// a task can be picked up from zero conversational context. A packet that
/// validates when it should NOT is worse than one that refuses, because
/// the caller acts on it. The two rules, and the four things that are
/// deliberately NOT rules, are derived by RUNNING the oracle against
/// deliberately broken states and are recorded in
/// `planar.engine.runtime.resumecheck`'s header rather than repeated here.
///
/// The observable contract this layer owns:
///
///   resumable      exit 0, stdout `{"task_id":N,"resumable":true,"failures":null}`
///   not resumable  exit 1, the SAME JSON on stdout (with a populated
///                  `failures` array) AND `error: task N is not resumable`
///                  on stderr
///   absent task    exit 1, ZERO bytes on stdout, `error: task N not found`
///   bad id         exit 2, `error: task id must be an integer, got 'x'`
///
/// The middle case is the one worth stating twice: a non-resumable task
/// still WRITES ITS PAYLOAD to stdout and then fails. A caller that reads
/// stdout only on exit 0 loses the failure list that tells it what to fix.
/// The handler therefore writes to `ctx.out()` and only then returns the
/// error.
///
/// ## `planar resume`'s no-id cwd-scope derivation IS ported (task 6452)
///
/// Both the no-id and the with-id forms used to answer `not_implemented`
/// (exit 64) unconditionally. Task 6452 ported the no-id branch's TWO
/// pieces — cwd read-set derivation (`engine::identity::resolve_read_scope_set`,
/// no `--scope` override since this verb has no such flag) and the
/// newest-first `sessions` walk that picks the first still-active task
/// (`todo`/`doing`/`blocked`) whose stored scope lands in that read set
/// (`matches_read_scope`, mirroring `handlers/resume/cmd.zig`'s
/// `matchesReadScope`) — matching the oracle byte-for-byte on both of its
/// refusals:
///
///     oracle & here:  planar resume     (no match)  -> exit 1, "no active
///                     task in cwd-derived scope; pass <task-id> explicitly"
///     oracle & here:  planar resume     (cwd unscoped) -> exit 1, "cwd is
///                     not inside any registered Planar scope; cd into a
///                     registered scope or pass <task-id> explicitly"
///
/// ## The 8-section packet BODY IS ported (task 6455)
///
/// Once a task id is in hand — whether typed explicitly or derived from
/// cwd above — this handler now calls
/// `planar.engine.runtime.resumecheck::build_packet` and renders its
/// result. `resume` stays registered as a handler regardless (the
/// dual-group-and-leaf argument above still holds: an unregistered parent
/// would fall to dispatch's help-page path and answer EXIT 0 with a help
/// page — a silent success where the oracle produces a packet), but the
/// not-found and query-failure arms are the only ones left, plus the
/// bad-id `invalid_input` refusal.
///
/// Task 6452's blocker inventory named FOUR layer-2 surfaces as missing.
/// Re-verified by RUNNING the oracle at task 6455: only session's
/// per-task readers (`recent_entries_for_task` / `recent_sessions_for_task`,
/// landed on `session.cppm` this task) were genuinely absent. The other
/// three had already landed as unrelated work progressed
/// (`external.link::links_for_entity`, all three of
/// `engine.planning.{decision,question,artifact}`) or were never actually
/// missing (`plan_step::list_steps` already existed; only `plan.cppm`
/// itself lacked a step read path, and the note conflated the two
/// modules). None of that changes the LAYER shape though — `build_packet`
/// reproduces the oracle's own raw SQL for plan position, operational-plane
/// links, decisions, questions and artifacts directly rather than importing
/// those sibling layer-2 buckets; see `resumecheck.cppm`'s header for the
/// full account and for why `decisions_for_task` specifically is NOT reused
/// here.
///
/// Byte-for-byte confirmed against the live oracle at task 6455 (values
/// from a fresh fixture, not the stale one this header used to carry):
///
///     {"identity":{"task_id":1,"plan_id":1,"title":"RESUME_TARGET",
///      "status":"todo","scope_kind":"global","scope_id":null},
///      "state":{"status":"todo","next_action":"verify",
///               "last_action_at":"...","last_action_body":"checkpoint"},
///      "plan":{"plan_id":1,"plan_title":"Demo plan",
///              "completed":[{"ordinal":1,"body":"step one","status":"done"}],
///              "current":[],
///              "remaining":[{"ordinal":2,"body":"step two","status":"pending"},
///                           {"ordinal":3,"body":"step three","status":"pending"}]},
///      "operational_plane":{"links":[{"link_id":1,"external_id":"42",
///        "external_url":"","remote_status":"","remote_assignee":"",
///        "last_synced_at":"","sync_status":"never","conflict":false,
///        "refresh_error":""}],"refresh_note":""},
///      "recent_activity":[...],
///      "decisions":[{"id":1,"title":"Use X","status":"proposed"}],
///      "questions":[{"id":1,"title":"Is X ok","status":"open","answer_body":""}],
///      "artifacts":[{"artifact_id":1,"title":"Design doc","kind":"design_note",
///                    "relationship":"verifies"}],
///      "audit":{"session_id":1,"vendor":"cli","started_at":"..."},
///      "active_claim":null,"from_handoff":{"handoff_id":1,
///        "worktree_path":"/wt2","repo_root":"","branch":""}}
///
/// (`active_claim`, when one is held, carries `claim_id`/`claim_token`/
/// `vendor`/`worktree_path`/`repo_root`/`branch` — probed separately since
/// a task cannot hold both an active claim AND the handoff fallback in the
/// same fixture; `from_handoff` is populated only when no active claim
/// carries a nonempty `worktree_path`.)
///
/// The text form is the eight `## N. <Title>` sections under a
/// `=== Resume Packet: task N ===` banner, oracle-captured byte-for-byte
/// from `handlers/resume/cmd.zig`'s `renderText` — see
/// `resumecheck.cpp`'s `render_packet_text` for the exact strings,
/// including the `(none)` / `(no external links)` / `(no prior session)`
/// placeholders and the em-dash (U+2014) in the recent-activity line.
module;

export module planar.cmd.planar.handlers.resume;

import std;
import cli11;
import planar.cliapp.args;
import planar.cmd.planar.context;
import planar.cmd.planar.handler;

namespace planar::cmd::handlers {

/// @brief `planar resume [<task-id>] [--json]` — the 8-section packet.
///
/// The no-id cwd-scope derivation (read-set resolution plus the
/// most-recent-active-task walk) and the packet body built from a
/// resolved task id are BOTH ported. See this module's header.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return The no-id derivation's refusal when cwd resolution or
/// most-recent-active selection fails; the bad-id `invalid_input` failure
/// for a malformed explicit id; `not_found` when the resolved task id does
/// not exist; otherwise success with the rendered packet written to
/// `ctx.out()`.
export auto resume_packet(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief `planar resume validate <task-id> [--json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success when resumable; the failure otherwise — after the
/// payload has already been written.
export auto resume_validate(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Declare the `resume` group. Folded out of the generated
/// `surface.cpp` at M11.3f (task 6636, decision 1068).
/// @param root The root app to attach it to.
export auto declare_resume(CLI::App& root) -> void;

} // namespace planar::cmd::handlers
