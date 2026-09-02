module;
export module planar.cmd.planar_agent.handlers.context;
import planar.cliapp.args;
import planar.cmd.planar_agent.context;
import planar.cmd.planar_agent.handler;
namespace planar::cmd::agent::handlers {
export auto context_add(context&, const cliapp::parsed_args&) -> handler_result;
export auto context_capsule(context&, const cliapp::parsed_args&) -> handler_result;
export auto context_list(context&, const cliapp::parsed_args&) -> handler_result;
export auto context_resolve(context&, const cliapp::parsed_args&) -> handler_result;
/// @brief Translate one Claude or Copilot hook event into session/action rows.
export auto ingest(context&, const cliapp::parsed_args&) -> handler_result;
} // namespace planar::cmd::agent::handlers
