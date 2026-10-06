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

/// @brief Test seam for a failure after the first signal disposition is installed.
/// The callback runs synchronously while the relay owns its pipe and handler.
/// @param ctx Invocation streams and environment.
/// @param args Parsed wait arguments.
/// @param after_first_install Return false to simulate partial setup failure.
/// @return The handler result after scoped resources have unwound.
export auto queue_wait_with_signal_setup_hook(context& ctx, const cliapp::parsed_args& args,
                                              const std::function<bool()>& after_first_install) -> handler_outcome;
} // namespace planar::cmd::agent::handlers
