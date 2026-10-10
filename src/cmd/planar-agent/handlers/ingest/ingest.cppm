/// @file ingest.cppm
/// @brief `planar.cmd.planar_agent.handlers.ingest` — the handler declaration
/// for `planar-agent ingest`, the vendor-hook normalizer (plan 996, task
/// 6038).
///
/// The implementation lives in `ingest.cpp`. It was declared in the
/// `handlers.context` module until task 7458 removed `planar-agent context`;
/// it now stands alone, so the handler outlives the verbs it shared a file
/// with.
module;

export module planar.cmd.planar_agent.handlers.ingest;

import cli11;
import planar.cliapp.args;
import planar.cmd.planar_agent.context;
import planar.cmd.planar_agent.handler;

namespace planar::cmd::agent::handlers {

/// @brief Handle `planar-agent ingest`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure to report.
export auto ingest(context& ctx, const cliapp::parsed_args& args) -> handler_result;

} // namespace planar::cmd::agent::handlers
