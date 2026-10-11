/// @file brief.cppm
/// @brief `planar.cmd.planar_agent.handlers.brief` — `planar-agent brief <plan-id>`, the single read an
/// orchestrator makes before it dispatches work (plan 1142, task mo-brief-verb).
///
/// The verb composes `planar.cmd.planar_agent.brief` (`gather_brief`, `render_brief_text`,
/// `render_brief_json`) with two inputs of its own: a STRICTLY READ-ONLY database connection
/// (`database_policy::open_read_only`, so no statement the engine runs can write) and the user's
/// config file, resolved from `PLANAR_CONFIG_PATH` or `$HOME/.planar/config.toml` exactly as
/// `planar config` resolves it. A missing config file means the shipped defaults; a config file that
/// cannot be read or parsed does not fail the verb: the brief renders with `policy.valid` false and
/// one finding for the file. The `[policy]` values have no environment override.
module;

export module planar.cmd.planar_agent.handlers.brief;

import cli11;
import planar.cliapp.args;
import planar.cmd.planar_agent.context;
import planar.cmd.planar_agent.handler;

namespace planar::cmd::agent::handlers {

/// @brief Handle `planar-agent brief <plan-id> [--json]`.
///
/// Exit 0: the brief was written. Exit 1: no plan has the id (the message names it), or the database
/// could not be opened or read. Exit 2: the id is not positive. Exit 7: the schema is behind or ahead.
/// Writes nothing to the database, and nothing outside standard output.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure to report.
export auto brief(context& ctx, const cliapp::parsed_args& args) -> handler_result;

} // namespace planar::cmd::agent::handlers
