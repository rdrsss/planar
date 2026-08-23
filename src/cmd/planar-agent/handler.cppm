/// @file handler.cppm
/// @brief `planar.cmd.planar_agent.handler` — the one signature every verb
/// handler in the `planar-agent` binary has (plan 996, task 6107).
///
/// Its own module, rather than a line in `dispatch.cppm`, so the dependency
/// runs one way: `dispatch` imports every handler module to build its
/// table, so a handler cannot import `dispatch` back. Both import this.
module;

export module planar.cmd.planar_agent.handler;

import std;
import cli11;
import planar.cliapp.args;
import planar.cmd.planar_agent.context;
import planar.cmd.planar_agent.exit;

namespace planar::cmd::agent {

/// @brief What a handler returns: nothing on success, a typed failure
/// otherwise.
export using handler_result = std::expected<void, domain_error>;

/// @brief The erased handler type the dispatch table stores.
export using handler_fn = std::function<handler_result(context&, const cliapp::parsed_args&)>;

} // namespace planar::cmd::agent
