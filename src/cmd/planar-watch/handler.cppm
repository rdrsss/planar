/// @file handler.cppm
/// @brief `planar.cmd.planar_watch.handler` — the one signature every verb
/// handler in the `planar-watch` binary has (plan 996, task 6107).
module;

export module planar.cmd.planar_watch.handler;

import std;
import planar.cli;
import planar.cmd.planar_watch.context;
import planar.cmd.planar_watch.exit;

namespace planar::cmd::watch {

/// @brief What a handler returns: nothing on success, a typed failure
/// otherwise.
export using handler_result = std::expected<void, domain_error>;

/// @brief The erased handler type the dispatch table stores.
export using handler_fn = std::function<handler_result(context&, const cli::match_result&)>;

} // namespace planar::cmd::watch
