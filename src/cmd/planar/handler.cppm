/// @file handler.cppm
/// @brief `planar.cmd.planar.handler` — the one signature every verb
/// handler in this binary has (plan 996, task 6105).
///
/// Its own module, rather than a line in `dispatch.cppm`, purely so the
/// dependency runs one way: `dispatch` imports every handler module to
/// build its table, so a handler cannot import `dispatch` back. Both
/// import this.
///
/// The signature is `(context&, const cliapp::parsed_args&) ->
/// std::expected<void, domain_error>` — output goes to `ctx.out()`, failure
/// comes back as a value, and nothing calls `std::exit`. See
/// `planar.cmd.planar.exit`'s header for why the Zig original's `noreturn`
/// `die()` was not reproduced.
module;

export module planar.cmd.planar.handler;

import std;
import cli11;
import planar.cliapp.args;
import planar.cmd.planar.context;
import planar.cmd.planar.exit;

namespace planar::cmd {

/// @brief What a handler returns: nothing on success, a typed failure
/// otherwise.
export using handler_result = std::expected<void, domain_error>;

/// @brief The erased handler type the dispatch table stores.
export using handler_fn = std::function<handler_result(context&, const cliapp::parsed_args&)>;

} // namespace planar::cmd
