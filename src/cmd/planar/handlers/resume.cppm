/// @file resume.cppm
/// @brief `planar.cmd.planar.handlers.resume` — `planar resume validate`,
/// plus the placeholder that keeps the unported `planar resume` packet
/// LOUD (plan 996, task 6040).
///
/// Port target: zig/src/cmd/planar/handlers/resume/validate.zig.
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
/// ## `planar resume` — the 8-section packet — is NOT ported
///
/// It is registered as a handler that returns `not_implemented` (exit 64)
/// rather than left unregistered, and that is a deliberate choice between
/// two imperfect options. `resume` is a dual group-and-leaf node, so an
/// unregistered parent would fall to dispatch's help-page path and answer
/// EXIT 0 with a help page — a silent success where the oracle produces a
/// packet. Exit 64 is this binary's designated placeholder code and fails
/// loudly instead.
///
/// The divergence is real and is recorded rather than hidden:
///
///     oracle:  planar resume 2         -> exit 0, the 8-section packet
///     oracle:  planar resume           -> exit 1, "no active task in
///                                        cwd-derived scope; pass
///                                        <task-id> explicitly"
///     here:    planar resume [<id>]    -> exit 64, "not implemented yet"
///
/// The packet's oracle bytes, captured in a pinned arena so the port that
/// lands it has a target:
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
/// NOT PORTED; answers exit 64. See this module's header.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Always the `not_implemented` failure.
export auto resume_packet(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief `planar resume validate <task-id> [--json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success when resumable; the failure otherwise — after the
/// payload has already been written.
export auto resume_validate(context& ctx, const cliapp::parsed_args& args) -> handler_result;

} // namespace planar::cmd::handlers
