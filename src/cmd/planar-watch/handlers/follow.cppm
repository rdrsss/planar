/// @file follow.cppm
/// @brief Shared interruptible polling support for `planar-watch --follow`.
module;
export module planar.cmd.planar_watch.handlers.follow;
import std;
import planar.cmd.planar_watch.context;
import planar.cmd.planar_watch.handler;
namespace planar::cmd::watch::handlers::follow {
/// @brief Run one snapshot immediately, then re-run it on each poll until SIGINT.
/// @param ctx The read-only watch context.
/// @param interval_text Optional duration (`ns`, `us`, `ms`, `s`, `m`, `h`).
/// @param snapshot Rendering operation.
/// @return A snapshot error, if one occurs.
export auto snapshots(context& ctx, const std::optional<std::string>& interval_text,
                      const std::function<handler_result()>& snapshot) -> handler_result;
/// @brief True while `snapshots` invokes its callback, preventing recursive setup.
export auto active() -> bool;
} // namespace planar::cmd::watch::handlers::follow
