/// @file settings.cpp
/// @brief Implementation of `planar.engine.hostqueue.settings`. See settings.cppm
/// for the contract.

module planar.engine.hostqueue.settings;

import std;
import planar.db;

namespace planar::engine::hostqueue {

namespace {

constexpr std::int64_t k_max_slots        = 1024;
constexpr std::int64_t k_max_history_days = 36500;
constexpr std::int64_t k_ms_per_day       = 24LL * 60 * 60 * 1000;
constexpr std::int64_t k_max_duration_ms  = k_ms_per_day; ///< One day; keeps every ms form far from overflow.

/// @brief A duration parsed from text, before its range is judged.
struct parsed_duration {
  std::int64_t ms       = 0;     ///< The value in milliseconds, saturated at the int64 limit on overflow.
  bool         negative = false; ///< The text began with `-`.
};

/// @brief Parse `<integer><unit>` with unit `ms`, `s`, `m` or `h` (and `d` when `allow_days`).
/// @param allow_days Accept the `d` (24h) unit; only `parse_history_since` does.
/// @return The parsed value, or nullopt when the text is not of that shape.
auto parse_duration_text(std::string_view text, bool allow_days) -> std::optional<parsed_duration> {
  parsed_duration out;
  if (text.starts_with('-')) {
    out.negative = true;
    text.remove_prefix(1);
  }
  std::size_t digits = 0;
  while (digits < text.size() && text[digits] >= '0' && text[digits] <= '9') {
    ++digits;
  }
  if (digits == 0) {
    return std::nullopt;
  }
  std::int64_t count   = 0;
  auto const [ptr, ec] = std::from_chars(text.data(), text.data() + digits, count);
  static_cast<void>(ptr);
  bool overflow = (ec == std::errc::result_out_of_range);
  if (ec != std::errc{} && !overflow) {
    return std::nullopt;
  }
  auto const   unit   = text.substr(digits);
  std::int64_t factor = 0;
  if (unit == "ms") {
    factor = 1;
  } else if (unit == "s") {
    factor = 1000;
  } else if (unit == "m") {
    factor = 60 * 1000;
  } else if (unit == "h") {
    factor = 60 * 60 * 1000;
  } else if (allow_days && unit == "d") {
    factor = 24 * 60 * 60 * 1000;
  } else {
    return std::nullopt;
  }
  if (overflow || count > std::numeric_limits<std::int64_t>::max() / factor) {
    out.ms = std::numeric_limits<std::int64_t>::max();
  } else {
    out.ms = count * factor;
  }
  return out;
}

/// @brief Parses a whole string as a base-ten integer; nullopt for anything else.
auto parse_integer(std::string_view text) -> std::optional<std::int64_t> {
  if (text.empty()) {
    return std::nullopt;
  }
  std::int64_t value   = 0;
  auto const [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (ec != std::errc{} || ptr != text.data() + text.size()) {
    return std::nullopt;
  }
  return value;
}

/// @brief A refusal naming `key`.
auto refuse(std::string_view key, std::string message) -> std::unexpected<settings_error> {
  return std::unexpected(
      settings_error{.kind = settings_error_kind::refused, .key = std::string{key}, .message = std::move(message)});
}

/// @brief Applies an integer setting, refusing a value outside `[lo, hi]`.
auto apply_integer(queue_settings& settings, std::string_view key, std::string_view text, std::int64_t lo, std::int64_t hi,
                   std::int64_t queue_settings::* field) -> std::expected<void, settings_error> {
  auto const value = parse_integer(text);
  if (!value.has_value() || *value < lo || *value > hi) {
    return refuse(key, std::format("{} must be an integer between {} and {}, got \"{}\"", key, lo, hi, text));
  }
  settings.*field = *value;
  return {};
}

/// @brief Applies a duration setting, refusing a bare integer, a negative value, a value below `min_ms` or above one day.
/// @param min_ms The smallest accepted value: 1 for a positive interval, 0 where zero is meaningful.
auto apply_duration(queue_settings& settings, std::string_view key, std::string_view text, std::int64_t min_ms,
                    std::int64_t queue_settings::* field) -> std::expected<void, settings_error> {
  auto const parsed = parse_duration_text(text, /*allow_days=*/false);
  if (!parsed.has_value()) {
    return refuse(key,
                  std::format("{} must be an integer followed by a unit (ms, s, m, h) such as \"30s\", got \"{}\"", key, text));
  }
  if (parsed->negative && parsed->ms != 0) {
    return refuse(key, std::format("{} must not be negative, got \"{}\"", key, text));
  }
  if (parsed->ms < min_ms) {
    return refuse(key, std::format("{} must be greater than zero, got \"{}\"", key, text));
  }
  if (parsed->ms > k_max_duration_ms) {
    return refuse(key, std::format("{} must be at most 24h, got \"{}\"", key, text));
  }
  settings.*field = parsed->ms;
  return {};
}

/// @brief Applies one stored or typed key/value pair, judging the value alone.
/// The cross-key rule is judged separately by `check_window`.
auto apply_value(queue_settings& settings, std::string_view key, std::string_view text) -> std::expected<void, settings_error> {
  if (key == "slots") {
    return apply_integer(settings, key, text, 1, k_max_slots, &queue_settings::slots);
  }
  if (key == "poll_interval") {
    return apply_duration(settings, key, text, 1, &queue_settings::poll_interval_ms);
  }
  if (key == "stale_after") {
    return apply_duration(settings, key, text, 1, &queue_settings::stale_after_ms);
  }
  if (key == "grace") {
    return apply_duration(settings, key, text, 0, &queue_settings::grace_ms);
  }
  if (key == "history_days") {
    return apply_integer(settings, key, text, 1, k_max_history_days, &queue_settings::history_days);
  }
  if (key == "tagged_history_days") {
    return apply_integer(settings, key, text, 1, k_max_history_days, &queue_settings::tagged_history_days);
  }
  return std::unexpected(settings_error{
      .kind    = settings_error_kind::unknown_key,
      .key     = std::string{key},
      .message = std::format("{} is not a queue setting (expected slots, poll_interval, stale_after, grace, history_days, "
                             "tagged_history_days)",
                             key)});
}

/// @brief The window must span at least one poll, or every waiting entry would
/// look stale between its own refreshes.
/// @param key The setting the refusal is reported against.
auto check_window(const queue_settings& settings, std::string_view key) -> std::expected<void, settings_error> {
  if (settings.stale_after_ms < settings.poll_interval_ms) {
    return refuse(key, std::format("stale_after ({}ms) must not be shorter than poll_interval ({}ms)", settings.stale_after_ms,
                                   settings.poll_interval_ms));
  }
  return {};
}

} // namespace

auto parse_duration(std::string_view text) -> std::expected<std::int64_t, std::string> {
  auto const parsed = parse_duration_text(text, /*allow_days=*/false);
  if (!parsed.has_value()) {
    return std::unexpected(
        std::format("'{}' is not a duration: use an integer followed by a unit (ms, s, m, h), such as 30s or 5m", text));
  }
  if (parsed->negative || parsed->ms < 1) {
    return std::unexpected(std::format("'{}' must be greater than zero", text));
  }
  if (parsed->ms > k_max_duration_ms) {
    return std::unexpected(std::format("'{}' must be at most 24h", text));
  }
  return parsed->ms;
}

auto parse_history_since(std::string_view text) -> std::expected<std::int64_t, std::string> {
  auto const parsed = parse_duration_text(text, /*allow_days=*/true);
  if (!parsed.has_value()) {
    return std::unexpected(
        std::format("'{}' is not a duration: use an integer followed by a unit (ms, s, m, h, d), such as 30m or 7d", text));
  }
  if (parsed->negative || parsed->ms < 1) {
    return std::unexpected(std::format("'{}' must be greater than zero", text));
  }
  if (parsed->ms > k_max_history_days * k_ms_per_day) {
    return std::unexpected(std::format("'{}' must be at most {}d (the history retention maximum)", text, k_max_history_days));
  }
  return parsed->ms;
}

auto default_queue_settings() -> queue_settings {
  return {};
}

auto validate_setting(std::string_view key, std::string_view value_text, const queue_settings& effective)
    -> std::expected<queue_settings, settings_error> {
  queue_settings candidate = effective;
  if (auto applied = apply_value(candidate, key, value_text); !applied.has_value()) {
    return std::unexpected(applied.error());
  }
  if (key == "poll_interval" || key == "stale_after") {
    if (auto window = check_window(candidate, key); !window.has_value()) {
      return std::unexpected(window.error());
    }
  }
  return candidate;
}

auto load_settings(db::connection& conn) -> std::expected<queue_settings, settings_error> {
  auto stmt = conn.prepare("select key, value from queue_settings");
  if (!stmt.has_value()) {
    return std::unexpected(settings_error{.kind    = settings_error_kind::database,
                                          .key     = {},
                                          .message = std::format("cannot read queue_settings: {}", stmt.error().message_)});
  }
  queue_settings settings = default_queue_settings();
  while (true) {
    auto stepped = stmt->step();
    if (!stepped.has_value()) {
      return std::unexpected(settings_error{.kind    = settings_error_kind::database,
                                            .key     = {},
                                            .message = std::format("cannot read queue_settings: {}", stepped.error().message_)});
    }
    if (*stepped == db::step_result::done) {
      break;
    }
    auto const key   = stmt->column_text(0);
    auto const value = stmt->column_text(1);
    if (auto applied = apply_value(settings, key, value); !applied.has_value()) {
      return std::unexpected(applied.error());
    }
  }
  if (auto window = check_window(settings, "stale_after"); !window.has_value()) {
    return std::unexpected(window.error());
  }
  return settings;
}

} // namespace planar::engine::hostqueue
