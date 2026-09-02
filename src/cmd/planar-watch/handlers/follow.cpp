/// @file follow.cpp
/// @brief Signal-safe, bounded-latency polling implementation for watch views.
module;
#include <csignal>
module planar.cmd.planar_watch.handlers.follow;
import std;
import planar.cliapp.args;
import planar.cmd.planar_watch.context;
import planar.cmd.planar_watch.exit;
import planar.cmd.planar_watch.handler;
namespace planar::cmd::watch::handlers::follow {
namespace {
volatile std::sig_atomic_t interrupted = 0;
thread_local bool          in_follow   = false;
auto                       on_sigint(int) -> void {
  interrupted = 1;
}
auto parse_interval(const std::optional<std::string>& text) -> std::expected<std::chrono::nanoseconds, domain_error> {
  if (!text)
    return std::chrono::seconds{1};
  auto const& value = *text;
  std::size_t p     = 0;
  while (p < value.size() && std::isdigit(static_cast<unsigned char>(value[p])))
    ++p;
  if (p == 0)
    return std::unexpected(error_from_body(domain_error_kind::invalid_input, "InvalidValue"));
  auto const n = cliapp::parse_int64_zig(std::string_view{value}.substr(0, p));
  if (!n || *n <= 0)
    return std::unexpected(error_from_body(domain_error_kind::invalid_input, "InvalidValue"));
  auto const   unit  = std::string_view{value}.substr(p);
  std::int64_t scale = 1'000'000'000;
  if (unit == "ns")
    scale = 1;
  else if (unit == "us")
    scale = 1'000;
  else if (unit == "ms")
    scale = 1'000'000;
  else if (unit == "s" || unit.empty())
    scale = 1'000'000'000;
  else if (unit == "m")
    scale = 60'000'000'000;
  else if (unit == "h")
    scale = 3'600'000'000'000;
  else
    return std::unexpected(error_from_body(domain_error_kind::invalid_input, "InvalidValue"));
  if (*n > std::numeric_limits<std::int64_t>::max() / scale)
    return std::unexpected(error_from_body(domain_error_kind::invalid_input, "InvalidValue"));
  return std::chrono::nanoseconds{*n * scale};
}
} // namespace
auto active() -> bool {
  return in_follow;
}
auto snapshots(context& ctx, const std::optional<std::string>& interval_text, const std::function<handler_result()>& snapshot)
    -> handler_result {
  auto interval = parse_interval(interval_text);
  if (!interval)
    return std::unexpected(interval.error());
  interrupted   = 0;
  auto previous = std::signal(SIGINT, on_sigint);
  in_follow     = true;
  while (!interrupted) {
    if (auto result = snapshot(); !result)
      return result;
    auto remaining = *interval;
    while (!interrupted && remaining.count() > 0) {
      auto const chunk =
          std::min(remaining, std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::milliseconds{100}));
      std::this_thread::sleep_for(chunk);
      remaining -= chunk;
    }
    if (!interrupted)
      ctx.refresh_db();
  }
  std::signal(SIGINT, previous);
  in_follow = false;
  return {};
}
} // namespace planar::cmd::watch::handlers::follow
