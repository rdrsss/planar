/// @file synthesize.cppm
/// @brief Layer-3 composition for `planar synthesize`.
module;

export module planar.cmd.planar.handlers.synthesize;

import planar.cliapp.args;
import planar.cmd.planar.context;
import planar.cmd.planar.handler;

namespace planar::cmd::handlers {
export auto synthesize(context& ctx, const cliapp::parsed_args& args) -> handler_result;
}
