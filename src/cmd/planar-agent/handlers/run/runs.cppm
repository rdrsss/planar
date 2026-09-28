/// @file runs.cppm
/// @brief `planar-agent run start/end` handlers.
module;
export module planar.cmd.planar_agent.handlers.runs;
import planar.cliapp.args;
import planar.cmd.planar_agent.context;
import planar.cmd.planar_agent.handler;
namespace planar::cmd::agent::handlers {
/// @brief Provide the run start command operation.
/// @return Success or a command error.
export auto run_start(context&, const cliapp::parsed_args&) -> handler_result;
/// @brief Provide the run end command operation.
/// @return Success or a command error.
export auto run_end(context&, const cliapp::parsed_args&) -> handler_result;
/// @brief Provide the run heartbeat command operation.
/// @return Success or a command error.
export auto run_heartbeat(context&, const cliapp::parsed_args&) -> handler_result;
} // namespace planar::cmd::agent::handlers
