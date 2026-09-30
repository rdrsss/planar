/// @file queue.cppm
/// @brief `planar.engine.config.queue` — the `[queue]` configuration table of
/// the host-wide build and test queue (plan 1080, task hq-config; tech spec
/// 647 § Components and § How many slots, and how it is configured).
///
/// Keys, all optional, in `~/.planar/config.toml`:
///
///   - `slots`         integer, at least 1 (default 1);
///   - `poll_interval` duration, above zero (default `1s`);
///   - `stale_after`   duration, above zero and not below `poll_interval`
///                     (default `30s`);
///   - `grace`         duration, zero or more (default `10s`);
///   - `history_days`  integer, at least 1 (default 30).
///
/// A duration is an integer immediately followed by a unit: `ms`, `s`, `m` or
/// `h` (`250ms`, `30s`). A bare integer is refused rather than guessed at,
/// because the unit of a bare number is the classic silent 1000x error.
/// Every value has an upper bound so its millisecond form cannot overflow.
///
/// Entry points: `queue_from_map` (validate and convert a parsed document),
/// `validate_queue` (every finding, for `planar config validate`) and
/// `load_queue_settings` (the per-poll loader a `planar-agent` handler calls).
/// `queue_settings` carries exactly the parameters the `hostqueue` bucket
/// consumes (`slots`, `stale_after_ms`, `grace_ms`, `history_days`) plus the
/// poll interval the caller sleeps for, in milliseconds.
///
/// Invariant: a `queue_settings` obtained from this module always satisfies
/// the ranges above, so the engine never receives a negative or zero
/// staleness window (task hq-negative-window).
///
/// Independence: the loader reads ONLY the configuration file. It never opens
/// `planar.db` or any database, so the queue keeps working when the main
/// database is schema-locked (product spec, "Planar's main database is
/// unavailable"). Error boundary: `std::expected`; nothing throws.
module;

export module planar.engine.config.queue;

import std;
import planar.engine.config.toml;

namespace planar::engine::config {

/// @brief The `[queue]` settings in force, in the units the engine consumes.
export struct queue_settings {
  std::int64_t slots            = 1;     ///< How many commands may run at once; at least 1.
  std::int64_t poll_interval_ms = 1000;  ///< How often a submitter polls, ms; above zero.
  std::int64_t stale_after_ms   = 30000; ///< The staleness window, ms; above zero, not below `poll_interval_ms`.
  std::int64_t grace_ms         = 10000; ///< SIGTERM-to-SIGKILL grace, ms; zero or more.
  std::int64_t history_days     = 30;    ///< How many days history rows and logs are kept; at least 1.

  /// @brief Field-wise equality.
  /// @return Whether every field is equal.
  auto operator==(const queue_settings&) const -> bool = default;
};

/// @brief One refused `[queue]` value.
export struct queue_finding {
  std::string key;     ///< The dotted key, for example `queue.stale_after`.
  std::string message; ///< What is wrong and what would be accepted.
};

/// @brief Why `load_queue_settings` could not produce settings.
export struct queue_load_error {
  /// @brief The class of failure.
  enum class kind : std::uint8_t {
    unreadable,   ///< The file exists but could not be read.
    parse_failed, ///< The file is not valid TOML.
    invalid,      ///< The `[queue]` table has refused values; see `findings`.
  };
  kind                       kind_ = kind::unreadable; ///< The class of failure.
  std::string                message;                  ///< A one-line description.
  std::vector<queue_finding> findings;                 ///< The refused values, for `kind::invalid`.
};

/// @brief Parses a duration given on the command line (`queue run --timeout`,
/// `--wait-timeout`) with the grammar and the one-day cap `[queue]` durations
/// use, so a flag and a configuration key spelled the same mean the same.
/// Unlike a configuration key, a flag has no meaningful zero: a run limit or a
/// wait limit of zero would end the entry before it began, so zero and
/// negative values are refused.
/// @param text The value, an integer immediately followed by `ms`, `s`, `m` or `h`.
/// @return The milliseconds, between 1 and 24 hours inclusive, or a one-line
/// reason the text is refused.
export auto parse_duration_flag(std::string_view text) -> std::expected<std::int64_t, std::string>;

/// @brief The settings used when no `[queue]` table is present.
/// @return One slot, a 1s poll, a 30s staleness window, a 10s grace and 30 days of history.
export auto default_queue_settings() -> queue_settings;

/// @brief Check every `queue.*` key of a parsed document.
/// @param doc The flattened document from `parse_toml`.
/// @return Every refused value, in key order; empty when the table is valid or absent.
export auto validate_queue(const toml_map& doc) -> std::vector<queue_finding>;

/// @brief Convert a parsed document to settings, refusing an invalid table.
/// @param doc The flattened document from `parse_toml`.
/// @return The settings (defaults for absent keys), or every finding.
export auto queue_from_map(const toml_map& doc) -> std::expected<queue_settings, std::vector<queue_finding>>;

/// @brief Read the queue settings from the configuration file, as a handler does at each poll.
/// @param config_path The configuration file; a missing file yields the defaults.
/// @return The settings, or why they could not be produced. Opens no database.
export auto load_queue_settings(const std::filesystem::path& config_path) -> std::expected<queue_settings, queue_load_error>;

} // namespace planar::engine::config
