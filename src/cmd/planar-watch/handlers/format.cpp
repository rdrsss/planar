/// @file format.cpp
/// @brief Implementation of `planar.cmd.planar_watch.handlers.format`.

module planar.cmd.planar_watch.handlers.format;

import std;

namespace planar::cmd::watch::handlers::format {

namespace {

/// @brief U+2026 HORIZONTAL ELLIPSIS, three bytes in UTF-8.
constexpr std::string_view k_ellipsis = "\xE2\x80\xA6";

/// @brief Parse exactly `count` ASCII digits.
/// @param text The digits.
/// @return The value, or unset when any character is not a digit.
auto parse_digits(std::string_view text) -> std::optional<std::int64_t> {
  std::int64_t value = 0;
  for (char const c : text) {
    if (c < '0' || c > '9') {
      return std::nullopt;
    }
    value = (value * 10) + (c - '0');
  }
  return value;
}

} // namespace

auto now_ms() -> std::int64_t {
  auto const now = std::chrono::system_clock::now();
  return std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
}

auto now_iso() -> std::string {
  auto const now = std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now());
  // `sys_time` formats in UTC by definition, so no zone lookup is needed —
  // and must not be introduced, because the row timestamps this is compared
  // against are UTC.
  return std::format("{:%Y-%m-%dT%H:%M:%S}Z", now);
}

auto iso_to_ms(std::string_view iso) -> std::optional<std::int64_t> {
  // "YYYY-MM-DDTHH:MM:SSZ" is the shortest accepted form.
  if (iso.size() < 20 || iso.back() != 'Z') {
    return std::nullopt;
  }
  if (iso[4] != '-' || iso[7] != '-' || iso[10] != 'T' || iso[13] != ':' || iso[16] != ':') {
    return std::nullopt;
  }
  auto const year   = parse_digits(iso.substr(0, 4));
  auto const month  = parse_digits(iso.substr(5, 2));
  auto const day    = parse_digits(iso.substr(8, 2));
  auto const hour   = parse_digits(iso.substr(11, 2));
  auto const minute = parse_digits(iso.substr(14, 2));
  auto const second = parse_digits(iso.substr(17, 2));
  if (!year || !month || !day || !hour || !minute || !second) {
    return std::nullopt;
  }

  std::int64_t millis = 0;
  if (iso.size() > 20) {
    // ".mmm" then 'Z'. Anything else in that slot is a parse failure rather
    // than a silently-dropped fraction.
    if (iso.size() != 24 || iso[19] != '.') {
      return std::nullopt;
    }
    auto const parsed = parse_digits(iso.substr(20, 3));
    if (!parsed) {
      return std::nullopt;
    }
    millis = *parsed;
  }

  std::chrono::year_month_day const date{std::chrono::year{static_cast<int>(*year)},
                                         std::chrono::month{static_cast<unsigned>(*month)},
                                         std::chrono::day{static_cast<unsigned>(*day)}};
  if (!date.ok()) {
    return std::nullopt;
  }
  auto const days = std::chrono::sys_days{date}.time_since_epoch().count();
  return (((days * 24 + *hour) * 60 + *minute) * 60 + *second) * 1000 + millis;
}

auto relative_time(std::int64_t now_ms_value, std::string_view then_iso) -> std::optional<std::string> {
  auto const then_ms = iso_to_ms(then_iso);
  if (!then_ms) {
    return std::nullopt;
  }
  auto const delta_ms = now_ms_value - *then_ms;
  // Signed on purpose: a FUTURE timestamp (clock skew between two machines
  // sharing a database) is negative and lands in this arm rather than
  // rendering a nonsense negative age.
  if (delta_ms < 5000) {
    return std::string{"just now"};
  }
  auto const delta_s = delta_ms / 1000;
  if (delta_s < 60) {
    return std::format("{}s ago", delta_s);
  }
  auto const delta_m = delta_s / 60;
  if (delta_m < 60) {
    return std::format("{}m ago", delta_m);
  }
  auto const delta_h = delta_m / 60;
  if (delta_h < 24) {
    return std::format("{}h ago", delta_h);
  }
  return std::format("{}d ago", delta_h / 24);
}

auto render_activity_summary(const std::optional<std::string>& summary) -> std::string {
  std::string_view const text = summary.has_value() ? std::string_view{*summary} : std::string_view{};
  if (text.empty()) {
    return "\"\"";
  }

  constexpr std::size_t k_limit = 80;
  if (text.size() <= k_limit) {
    return std::format("\"{}\"", text);
  }

  std::size_t cut = k_limit - k_ellipsis.size(); // 77
  // Walk back off a UTF-8 continuation byte (0b10xxxxxx) so the cut lands
  // on a character boundary. Without this the column can emit an invalid
  // byte sequence for any summary with a multi-byte character near 77.
  while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0U) == 0x80U) {
    --cut;
  }
  return std::format("\"{}{}\"", text.substr(0, cut), k_ellipsis);
}

auto render_worktree_column(const std::optional<std::string>& worktree_path) -> std::string {
  if (!worktree_path.has_value() || worktree_path->empty()) {
    return "\"\"";
  }
  std::string_view const path     = *worktree_path;
  auto const             slash    = path.find_last_of('/');
  std::string_view const basename = slash == std::string_view::npos ? path : path.substr(slash + 1);
  if (path.size() <= 40) {
    return std::string{basename};
  }
  return std::format("{}{}", k_ellipsis, basename);
}

auto render_relative_heartbeat(std::int64_t now_ms_value, std::string_view last_heartbeat_at) -> std::string {
  if (last_heartbeat_at.empty()) {
    return {};
  }
  auto const rendered = relative_time(now_ms_value, last_heartbeat_at);
  if (!rendered) {
    return {};
  }
  constexpr std::string_view k_suffix = " ago";
  if (rendered->ends_with(k_suffix)) {
    return rendered->substr(0, rendered->size() - k_suffix.size());
  }
  return *rendered;
}

} // namespace planar::cmd::watch::handlers::format
