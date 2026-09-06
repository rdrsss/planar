/// @file resume.cppm
/// @brief `planar.cmd.planar.handlers.resume` — `planar resume validate`,
/// the no-id cwd-scope derivation ahead of the 8-section packet, and the
/// placeholder that keeps the still-unported packet body LOUD (plan 996,
/// tasks 6040/6452).
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
/// What remains NOT ported is the 8-section packet BODY: once a task id is
/// in hand — whether typed explicitly or derived from cwd above — building
/// and rendering its packet still answers `not_implemented` (exit 64). It
/// is registered as a handler that returns that failure rather than left
/// unregistered, and that is a deliberate choice between two imperfect
/// options: `resume` is a dual group-and-leaf node, so an unregistered
/// parent would fall to dispatch's help-page path and answer EXIT 0 with a
/// help page — a silent success where the oracle produces a packet. Exit
/// 64 is this binary's designated placeholder code and fails loudly
/// instead.
///
/// The remaining divergence is real and is recorded rather than hidden:
///
///     oracle:  planar resume 2         -> exit 0, the 8-section packet
///     here:    planar resume [<id>]    -> exit 64, "not implemented yet"
///             (once a task id is resolved, by either path above)
///
/// The packet's oracle bytes, captured in a pinned arena so the port that
/// lands it has a target — RE-VERIFIED live against the oracle at task
/// 6452 and still an accurate shape (field names/nesting unchanged; the
/// concrete values below are from an EARLIER fixture, not the live run):
///
///     {"identity":{"task_id":2,"plan_id":1,"title":"...","status":"done",
///      "scope_kind":"association","scope_id":1},
///      "state":{"status":"done","next_action":"do the thing",
///               "last_action_at":"...","last_action_body":"checkpoint body"},
///      "plan":{"plan_id":1,"plan_title":"Demo plan","completed":[],
///              "current":[],"remaining":[]},
///      "operational_plane":{"links":[],"refresh_note":""},
///      "recent_activity":[],"decisions":[],"questions":[],"artifacts":[],
///      "audit":null,"active_claim":null,"from_handoff":null}
///
/// and the text form is eight `## N. <Title>` sections under a
/// `=== Resume Packet: task N ===` banner, with `(none)` / `(no external
/// links)` / `(no prior session)` placeholders for the empty ones.
///
/// What blocks it is four unported LAYER-2 surfaces, not this layer; the
/// inventory is in `src/lib/engine/runtime/CMakeLists.txt`.
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
/// most-recent-active-task walk) IS ported and can fail with its own two
/// refusals; the packet BODY built from a resolved task id is NOT PORTED
/// and always answers the `not_implemented` failure. See this module's
/// header.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return The no-id derivation's refusal when cwd resolution or
/// most-recent-active selection fails; the bad-id `invalid_input` failure
/// for a malformed explicit id; otherwise `not_implemented` once a task id
/// is in hand.
export auto resume_packet(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief `planar resume validate <task-id> [--json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success when resumable; the failure otherwise — after the
/// payload has already been written.
export auto resume_validate(context& ctx, const cliapp::parsed_args& args) -> handler_result;

} // namespace planar::cmd::handlers
