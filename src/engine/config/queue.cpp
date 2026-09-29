/// @file queue.cpp
/// @brief Implementation of `planar.engine.config.queue` (see queue.cppm).
module planar.engine.config.queue;

import std;
import planar.engine.config.toml;

namespace planar::engine::config {

namespace {

constexpr std::string_view k_prefix = "queue.";

constexpr std::int64_t k_max_slots        = 1024;
constexpr std::int64_t k_max_history_days = 36500;
constexpr std::int64_t k_max_duration_ms  = 24LL * 60 * 60 * 1000; ///< One day; keeps every ms form far from overflow.

/// @brief A duration parsed from text, before its range is judged.
struct parsed_duration {
  std::int64_t ms       = 0;     ///< The value in milliseconds, saturated at the int64 limit on overflow.
  bool         negative = false; ///< The text began with `-`.
};

/// @brief Parse `<integer><unit>` with unit `ms`, `s`, `m` or `h`.
/// @return The parsed value, or nullopt when the text is not of that shape.
auto parse_duration(std::string_view text) -> std::optional<parsed_duration> {
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

/// @brief What one duration key resolved to.
struct duration_result {
  std::optional<std::int64_t> value; ///< The milliseconds when the key is present and valid.
  bool                        present = false;
};

/// @brief Resolve a duration key, appending a finding when it is refused.
/// @param min_ms The smallest accepted value (0 when zero is meaningful, 1 otherwise).
auto read_duration(const toml_map& doc, std::string_view name, std::int64_t min_ms, std::vector<queue_finding>& findings)
    -> duration_result {
  auto const key = std::format("{}{}", k_prefix, name);
  auto const it  = doc.find(key);
  if (it == doc.end()) {
    return {};
  }
  duration_result out;
  out.present    = true;
  auto const bad = [&](std::string message) { findings.push_back(queue_finding{.key = key, .message = std::move(message)}); };
  if (it->second.kind_ != toml_value::kind::string) {
    bad(std::format("{} must be a duration string such as \"30s\" (units ms, s, m, h)", name));
    return out;
  }
  auto const parsed = parse_duration(it->second.string_);
  if (!parsed.has_value()) {
    bad(std::format("{} must be an integer followed by a unit (ms, s, m, h) such as \"30s\", got \"{}\"", name,
                    it->second.string_));
    return out;
  }
  if (parsed->negative && parsed->ms != 0) {
    bad(std::format("{} must not be negative, got \"{}\"", name, it->second.string_));
    return out;
  }
  if (parsed->ms < min_ms) {
    bad(std::format("{} must be greater than zero, got \"{}\"", name, it->second.string_));
    return out;
  }
  if (parsed->ms > k_max_duration_ms) {
    bad(std::format("{} must be at most 24h, got \"{}\"", name, it->second.string_));
    return out;
  }
  out.value = parsed->ms;
  return out;
}

/// @brief Resolve an integer key with an inclusive range, appending a finding when it is refused.
auto read_integer(const toml_map& doc, std::string_view name, std::int64_t lo, std::int64_t hi,
                  std::vector<queue_finding>& findings) -> std::optional<std::int64_t> {
  auto const key = std::format("{}{}", k_prefix, name);
  auto const it  = doc.find(key);
  if (it == doc.end()) {
    return std::nullopt;
  }
  auto const bad = [&](std::string message) { findings.push_back(queue_finding{.key = key, .message = std::move(message)}); };
  if (it->second.kind_ != toml_value::kind::integer) {
    bad(std::format("{} must be an integer between {} and {}", name, lo, hi));
    return std::nullopt;
  }
  auto const value = it->second.int_;
  if (value < lo || value > hi) {
    bad(std::format("{} must be between {} and {}, got {}", name, lo, hi, value));
    return std::nullopt;
  }
  return value;
}

/// @brief The result of judging a whole document.
struct judged {
  queue_settings             settings;
  std::vector<queue_finding> findings;
};

auto judge(const toml_map& doc) -> judged {
  judged out;
  auto&  findings = out.findings;

  for (auto const& [key, value] : doc) {
    static_cast<void>(value);
    if (!key.starts_with(k_prefix)) {
      continue;
    }
    auto const                                name  = std::string_view{key}.substr(k_prefix.size());
    constexpr std::array<std::string_view, 5> known = {"slots", "poll_interval", "stale_after", "grace", "history_days"};
    if (!std::ranges::contains(known, name)) {
      findings.push_back(queue_finding{
          .key = key,
          .message =
              std::format("{} is not a [queue] key (expected slots, poll_interval, stale_after, grace, history_days)", name)});
    }
  }

  if (auto const slots = read_integer(doc, "slots", 1, k_max_slots, findings)) {
    out.settings.slots = *slots;
  }
  if (auto const days = read_integer(doc, "history_days", 1, k_max_history_days, findings)) {
    out.settings.history_days = *days;
  }
  auto const poll  = read_duration(doc, "poll_interval", 1, findings);
  auto const stale = read_duration(doc, "stale_after", 1, findings);
  if (auto const grace = read_duration(doc, "grace", 0, findings); grace.value.has_value()) {
    out.settings.grace_ms = *grace.value;
  }
  if (poll.value.has_value()) {
    out.settings.poll_interval_ms = *poll.value;
  }
  if (stale.value.has_value()) {
    out.settings.stale_after_ms = *stale.value;
  }

  // The window must span at least one poll, or every waiting entry would look
  // stale between its own refreshes. Judged only when both sides are usable;
  // an absent side is the default in force.
  bool const poll_ok  = poll.value.has_value() || !poll.present;
  bool const stale_ok = stale.value.has_value() || !stale.present;
  if (poll_ok && stale_ok && (poll.present || stale.present) && out.settings.stale_after_ms < out.settings.poll_interval_ms) {
    findings.push_back(queue_finding{.key     = std::format("{}stale_after", k_prefix),
                                     .message = std::format("stale_after ({}ms) must not be shorter than poll_interval ({}ms)",
                                                            out.settings.stale_after_ms, out.settings.poll_interval_ms)});
  }

  std::ranges::sort(findings, {}, &queue_finding::key);
  return out;
}

} // namespace

auto default_queue_settings() -> queue_settings {
  return {};
}

auto validate_queue(const toml_map& doc) -> std::vector<queue_finding> {
  return judge(doc).findings;
}

auto queue_from_map(const toml_map& doc) -> std::expected<queue_settings, std::vector<queue_finding>> {
  auto result = judge(doc);
  if (!result.findings.empty()) {
    return std::unexpected(std::move(result.findings));
  }
  return result.settings;
}

auto load_queue_settings(const std::filesystem::path& config_path) -> std::expected<queue_settings, queue_load_error> {
  std::error_code ec;
  if (!std::filesystem::exists(config_path, ec)) {
    return default_queue_settings();
  }
  if (!std::filesystem::is_regular_file(config_path, ec)) {
    return std::unexpected(
        queue_load_error{.kind_   = queue_load_error::kind::unreadable,
                         .message = std::format("config path is not a regular file: {}", config_path.string())});
  }
  std::ifstream in{config_path, std::ios::binary};
  if (!in) {
    return std::unexpected(queue_load_error{.kind_   = queue_load_error::kind::unreadable,
                                            .message = std::format("cannot read config file: {}", config_path.string())});
  }
  std::string content{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
  auto const  parsed = parse_toml(content);
  if (!parsed.has_value()) {
    return std::unexpected(queue_load_error{.kind_   = queue_load_error::kind::parse_failed,
                                            .message = std::format("line {}: col {}: TOML parse error: {}", parsed.error().line,
                                                                   parsed.error().column, parsed.error().message)});
  }
  auto settings = queue_from_map(*parsed);
  if (!settings.has_value()) {
    return std::unexpected(
        queue_load_error{.kind_    = queue_load_error::kind::invalid,
                         .message  = std::format("invalid [queue] configuration: {}", settings.error().front().key),
                         .findings = std::move(settings.error())});
  }
  return *settings;
}

} // namespace planar::engine::config
