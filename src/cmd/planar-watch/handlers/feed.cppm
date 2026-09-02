/// @file feed.cppm
/// @brief Bounded snapshot implementation of the read-only activity feed.
module;
export module planar.cmd.planar_watch.handlers.feed;
import cli11;
import planar.cliapp.args;
import planar.cmd.planar_watch.context;
import planar.cmd.planar_watch.handler;
namespace planar::cmd::watch::handlers {
/// @brief Render the initial activity-feed snapshot; streaming is refused.
export auto feed(context& ctx, const cliapp::parsed_args& args) -> handler_result;
} // namespace planar::cmd::watch::handlers
