/// @file history.cpp
/// @brief Red stub of `planar.engine.hostqueue.history` (plan 1080, task
/// hq-history): every operation does nothing. See history.cppm for the
/// contract.

module planar.engine.hostqueue.history;

import std;
import planar.db;
import planar.engine.hostqueue.queue;

namespace planar::engine::hostqueue {

auto to_string(history_outcome /*outcome*/) -> std::string_view {
  return "";
}

auto parse_history_outcome(std::string_view /*text*/) -> std::optional<history_outcome> {
  return std::nullopt;
}

auto encode_canceller(const canceller& /*who*/) -> std::string {
  return {};
}

auto decode_canceller(std::string_view /*text*/) -> std::expected<canceller, queue_error> {
  return canceller{};
}

auto end_entry(db::connection& /*conn*/, std::int64_t /*seq*/, const end_request& /*request*/)
    -> std::expected<end_result, queue_error> {
  return end_result::already_gone;
}

auto record_successor(db::connection& /*conn*/, std::int64_t /*seq*/, std::int64_t /*successor_seq*/)
    -> std::expected<successor_result, queue_error> {
  return successor_result::no_such_entry;
}

auto find_history(db::connection& /*conn*/, std::int64_t /*seq*/) -> std::expected<std::optional<history_row>, queue_error> {
  return std::optional<history_row>{};
}

auto list_history(db::connection& /*conn*/, std::optional<std::int64_t> /*ended_since*/)
    -> std::expected<std::vector<history_row>, queue_error> {
  return std::vector<history_row>{};
}

auto delete_expired_history(db::connection& /*conn*/, std::int64_t /*retention_days*/, std::int64_t /*now*/)
    -> std::expected<expired_history, queue_error> {
  return expired_history{};
}

auto remove_log_files(std::span<std::string const> /*paths*/) -> std::vector<log_removal_failure> {
  return {};
}

} // namespace planar::engine::hostqueue
