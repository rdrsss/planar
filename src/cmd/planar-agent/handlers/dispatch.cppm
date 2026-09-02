/// @file dispatch.cppm
/// @brief Layer-3 routing dispatch preview and confirm handlers.
module;
export module planar.cmd.planar_agent.handlers.dispatch;
import planar.cliapp.args;
import planar.cmd.planar_agent.context;
import planar.cmd.planar_agent.handler;
namespace planar::cmd::agent::handlers {
export auto dispatch_preview(context&, const cliapp::parsed_args&) -> handler_result;
export auto dispatch_confirm(context&, const cliapp::parsed_args&) -> handler_result;
} // namespace planar::cmd::agent::handlers
