/// @file dispatch.cppm
/// @brief Layer-3 routing dispatch preview and confirm handlers.
module;
export module planar.cmd.planar_agent.handlers.dispatch;
import planar.cliapp.args;
import planar.cmd.planar_agent.context;
import planar.cmd.planar_agent.handler;
namespace planar::cmd::agent::handlers {
/// @brief Provide the dispatch preview command operation.
/// @return Success or a command error.
export auto dispatch_preview(context&, const cliapp::parsed_args&) -> handler_result;
/// @brief Provide the dispatch confirm command operation.
/// @return Success or a command error.
export auto dispatch_confirm(context&, const cliapp::parsed_args&) -> handler_result;
} // namespace planar::cmd::agent::handlers
