/// @file queue.cpp
/// @brief Implementation of `planar.engine.hostqueue.queue` (plan 1080, task
/// hq-enqueue). See queue.cppm for the contract.

module planar.engine.hostqueue.queue;

import std;
import planar.db;

namespace planar::engine::hostqueue {

namespace {

/// @brief The one failure the skeleton reports: nothing is implemented yet.
auto not_implemented(std::string_view what) -> std::unexpected<queue_error> {
  return std::unexpected(queue_error{
      .kind        = queue_error_kind::query_failed,
      .sqlite_code = 0,
      .message     = std::format("hostqueue: {} is not implemented", what),
  });
}

} // namespace

auto to_string(entry_state state) -> std::string_view {
  return state == entry_state::running ? "running" : "waiting";
}

auto encode_argv(std::span<std::string const> argv) -> std::string {
  std::string out;
  for (auto const& arg : argv) {
    if (!out.empty()) {
      out += ' ';
    }
    out += arg;
  }
  return out;
}

auto decode_argv(std::string_view text) -> std::expected<std::vector<std::string>, queue_error> {
  return std::vector<std::string>{std::string(text)};
}

auto enqueue(db::connection& /*conn*/, const enqueue_request& /*request*/) -> std::expected<std::int64_t, queue_error> {
  return not_implemented("enqueue");
}

auto find(db::connection& /*conn*/, std::int64_t /*seq*/) -> std::expected<std::optional<entry>, queue_error> {
  return not_implemented("find");
}

auto list(db::connection& /*conn*/) -> std::expected<std::vector<entry>, queue_error> {
  return not_implemented("list");
}

} // namespace planar::engine::hostqueue
