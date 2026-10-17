/// @file settings.cppm
/// @brief `planar.engine.hostqueue.settings` — the build queue's settings:
/// the field set, the defaults, the validation ranges and the loader that
/// reads them from the `queue_settings` table of the build queue database.
///
/// Keys, all optional, each stored as its text value in `queue_settings`:
///
///   - `slots`               integer, 1 to 1024 (default 1);
///   - `poll_interval`       duration, above zero (default `1s`);
///   - `stale_after`         duration, above zero and not below `poll_interval`
///                           (default `30s`);
///   - `grace`               duration, zero or more (default `10s`);
///   - `history_days`        integer, 1 to 36500 (default 30);
///   - `tagged_history_days` integer, 1 to 36500 (default 90).
///
/// A duration is an integer immediately followed by a unit: `ms`, `s`, `m` or
/// `h` (`250ms`, `30s`), at most 24 hours. A bare integer is refused rather
/// than guessed at, because the unit of a bare number is the classic silent
/// 1000x error. A value is stored as the operator typed it; the parsed value
/// is never written back.
///
/// Entry points: `load_settings` (the per-poll read, inside the caller's
/// transaction), `validate_setting` (one key/value pair as text, for
/// `config set`) and `parse_duration` / `parse_history_since` (the same
/// duration grammar for command-line flags). A missing key reads as its
/// default. Nothing is cached: a change written by another process applies at
/// the next call.
///
/// Error boundary: `std::expected`; nothing throws. Every SQLite failure is
/// wrapped into `settings_error` at this boundary.
module;

export module planar.engine.hostqueue.settings;

import std;
import planar.db;

namespace planar::engine::hostqueue {

/// @brief The build queue settings in force, in the units the engine consumes.
/// @invariant A value obtained from `load_settings` or `validate_setting`
/// satisfies the ranges in the file comment, including `stale_after_ms >=
/// poll_interval_ms`, so the engine never receives a zero or negative window.
export struct queue_settings {
  std::int64_t slots               = 1;     ///< How many commands may run at once; 1 to 1024.
  std::int64_t poll_interval_ms    = 1000;  ///< How often a submitter polls, ms; above zero.
  std::int64_t stale_after_ms      = 30000; ///< The staleness window, ms; above zero, not below `poll_interval_ms`.
  std::int64_t grace_ms            = 10000; ///< SIGTERM-to-SIGKILL grace, ms; zero or more.
  std::int64_t history_days        = 30;    ///< Days history rows and logs are kept; 1 to 36500.
  std::int64_t tagged_history_days = 90;    ///< Days tagged history rows are kept; 1 to 36500.

  /// @brief Field-wise equality.
  /// @return Whether every field is equal.
  auto operator==(const queue_settings&) const -> bool = default;
};

/// @brief Why a setting could not be read or written.
export struct settings_error {
  /// @brief The class of failure.
  enum class kind : std::uint8_t {
    unknown_key, ///< The key is not one of the queue settings.
    refused,     ///< The value, or the combination it forms with the other settings, is out of range or malformed.
    database,    ///< The `queue_settings` table could not be read.
  };
  kind        kind_ = kind::refused; ///< The class of failure.
  std::string key;                   ///< The setting the message is about; empty for `database`.
  std::string message;               ///< A one-line description that names the key.
};

/// @brief Parses a duration given on the command line (`queue run --timeout`,
/// `--wait-timeout`) with the grammar and the one-day cap the settings use, so
/// a flag and a setting spelled the same mean the same. Unlike a setting, a
/// flag has no meaningful zero: a run limit or a wait limit of zero would end
/// the entry before it began, so zero and negative values are refused.
/// @param text The value, an integer immediately followed by `ms`, `s`, `m` or `h`.
/// @return The milliseconds, between 1 and 24 hours inclusive, or a one-line
/// reason the text is refused.
export auto parse_duration(std::string_view text) -> std::expected<std::int64_t, std::string>;

/// @brief Parses `planar-watch queue history --since`: the grammar of
/// `parse_duration` plus the `d` (24 hour) unit, capped at the history
/// retention maximum (36500 days) rather than at one day.
/// @param text The value, an integer immediately followed by `ms`, `s`, `m`, `h` or `d`.
/// @return The milliseconds, between 1 and 36500 days inclusive, or a one-line
/// reason the text is refused.
export auto parse_history_since(std::string_view text) -> std::expected<std::int64_t, std::string>;

/// @brief The settings in force when a key is absent from `queue_settings`.
/// @return One slot, a 1s poll, a 30s staleness window, a 10s grace, 30 days of
/// history and 90 days of tagged history.
export auto default_queue_settings() -> queue_settings;

/// @brief Validates one key/value pair as the operator typed it, against the
/// settings currently in force, and returns the settings that would be in
/// force once the pair is written. Writes nothing.
/// @param key One of the six setting names.
/// @param value_text The value as typed: an integer, or a duration such as `30s`.
/// @param effective The settings in force now (from `load_settings`). The
/// cross-key rule, `stale_after` not below `poll_interval`, is judged against
/// these values with the new one substituted.
/// @return The candidate settings, or a `refused` or `unknown_key` error whose
/// message names the key that was refused.
export auto validate_setting(std::string_view key, std::string_view value_text, const queue_settings& effective)
    -> std::expected<queue_settings, settings_error>;

/// @brief Reads every setting from `queue_settings` inside the caller's
/// transaction, filling a missing key with its default.
/// @param conn The build queue connection; the caller's transaction, if any, is
/// the one the read runs in. Nothing is opened or cached.
/// @return The settings, or a `refused` error naming the stored key whose value
/// is invalid (a hand-edited row is never silently defaulted), or a `database`
/// error when the table cannot be read.
export auto load_settings(db::connection& conn) -> std::expected<queue_settings, settings_error>;

} // namespace planar::engine::hostqueue
