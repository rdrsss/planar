/// @file wait.cppm
/// @brief Read-only, finite logical-ticket observation for `planar-agent queue wait`.
/// The handler owns its signal relay and store connection, returning recorded
/// outcomes separately from observer reasons and never changing queue state.
module;
export module planar.cmd.planar_agent.handlers.queue.wait;
import std;
import planar.cliapp.args;
import planar.cmd.planar_agent.context;
import planar.cmd.planar_agent.handler;

namespace planar::cmd::agent::handlers {
/// @brief Observe a logical queue ticket within the requested finite budget.
/// @param ctx Invocation streams and environment.
/// @param args Parsed sequence, optional timeout and JSON flag.
/// @return The recorded command exit or a distinct observer stop status.
export auto queue_wait(context& ctx, const cliapp::parsed_args& args) -> handler_outcome;
} // namespace planar::cmd::agent::handlers
