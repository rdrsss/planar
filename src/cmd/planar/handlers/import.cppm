/// @file import.cppm
/// @brief `planar import`: layer-3 composition of staging and plan creation.
module;

export module planar.cmd.planar.handlers.importer;

import planar.cliapp.args;
import planar.cmd.planar.context;
import planar.cmd.planar.handler;

namespace planar::cmd::handlers {
/// @brief Handle `planar import <repo-root>` and its staging/apply flags.
export auto import_repo(context& ctx, const cliapp::parsed_args& args) -> handler_result;
}
