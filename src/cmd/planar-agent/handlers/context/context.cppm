/// @file context.cppm
/// @brief Handler declarations for `planar-agent context` and `ingest`.
module;
export module planar.cmd.planar_agent.handlers.context;
import cli11;
import planar.cliapp.args;
import planar.cmd.planar_agent.context;
import planar.cmd.planar_agent.handler;
namespace planar::cmd::agent::handlers {
export auto context_add(context&, const cliapp::parsed_args&) -> handler_result;
export auto context_capsule(context&, const cliapp::parsed_args&) -> handler_result;
export auto context_list(context&, const cliapp::parsed_args&) -> handler_result;
export auto context_resolve(context&, const cliapp::parsed_args&) -> handler_result;

/// @brief Handle `planar-agent ingest`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure to report.
export auto ingest(context& ctx, const cliapp::parsed_args& args) -> handler_result;
} // namespace planar::cmd::agent::handlers
