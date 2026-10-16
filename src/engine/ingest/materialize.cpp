/// @file materialize.cpp
/// @brief Implementation of `planar.engine.ingest.materialize` (see materialize.cppm).

module planar.engine.ingest.materialize;

import std;
import planar.sha256;

namespace planar::engine::ingest::materialize {
namespace {

constexpr std::string_view whitespace_inline = " \t";
constexpr std::string_view whitespace_block  = " \t\r\n";

[[nodiscard]] auto trim(std::string_view s, std::string_view chars) -> std::string_view {
  const auto first = s.find_first_not_of(chars);
  if (first == std::string_view::npos) {
    return {};
  }
  return s.substr(first, s.find_last_not_of(chars) - first + 1);
}

[[nodiscard]] auto trim_end(std::string_view s, std::string_view chars) -> std::string_view {
  const auto last = s.find_last_not_of(chars);
  return last == std::string_view::npos ? std::string_view{} : s.substr(0, last + 1);
}

/// @brief ASCII case-insensitive equality.
[[nodiscard]] auto equals_fold(std::string_view a, std::string_view b) -> bool {
  if (a.size() != b.size()) {
    return false;
  }
  const auto lower = [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c; };
  return std::ranges::equal(a, b, [&](char x, char y) { return lower(x) == lower(y); });
}

/// @brief One ATX heading line.
struct heading {
  std::size_t      level_ = 0;
  std::string_view title_;
};

/// @brief Recognises an ATX heading (`#` … `######`), stripping closing hashes.
///
/// Up to three leading spaces are allowed (four makes it indented code).
[[nodiscard]] auto atx_heading(std::string_view line) -> std::optional<heading> {
  const auto  without_cr = trim_end(line, "\r");
  std::size_t indent     = 0;
  while (indent < without_cr.size() && without_cr[indent] == ' ' && indent < 4) {
    ++indent;
  }
  if (indent > 3 || indent >= without_cr.size() || without_cr[indent] != '#') {
    return std::nullopt;
  }
  std::size_t level = 0;
  while (indent + level < without_cr.size() && without_cr[indent + level] == '#' && level < 7) {
    ++level;
  }
  if (level == 0 || level > 6) {
    return std::nullopt;
  }
  const auto after = indent + level;
  if (after < without_cr.size() && without_cr[after] != ' ' && without_cr[after] != '\t') {
    return std::nullopt;
  }
  auto title = trim(without_cr.substr(after), whitespace_inline);
  while (!title.empty() && title.back() == '#') {
    title = trim_end(title.substr(0, title.size() - 1), whitespace_inline);
  }
  return heading{.level_ = level, .title_ = title};
}

/// @brief One fenced-code-block delimiter line.
struct fence {
  char        char_       = '\0';
  std::size_t len_        = 0;
  bool        is_closing_ = false; ///< Nothing but whitespace follows the run.
};

[[nodiscard]] auto fence_delimiter(std::string_view line) -> std::optional<fence> {
  const auto  without_cr = trim_end(line, "\r");
  std::size_t indent     = 0;
  while (indent < without_cr.size() && without_cr[indent] == ' ' && indent < 4) {
    ++indent;
  }
  if (indent > 3 || indent >= without_cr.size()) {
    return std::nullopt;
  }
  const char c = without_cr[indent];
  if (c != '`' && c != '~') {
    return std::nullopt;
  }
  std::size_t count = 0;
  while (indent + count < without_cr.size() && without_cr[indent + count] == c) {
    ++count;
  }
  if (count < 3) {
    return std::nullopt;
  }
  return fence{.char_ = c, .len_ = count, .is_closing_ = trim(without_cr.substr(indent + count), whitespace_inline).empty()};
}

[[nodiscard]] auto is_indented_code(std::string_view line) -> bool {
  if (line.empty()) {
    return false;
  }
  if (line.front() == '\t') {
    return true;
  }
  std::size_t spaces = 0;
  while (spaces < line.size() && line[spaces] == ' ') {
    ++spaces;
  }
  return spaces >= 4;
}

} // namespace

auto sha256_hex(std::string_view input) -> std::string {
  // One implementation, in layer 1 (task 6759). This module carried its own
  // copy of FIPS 180-4; `source_digest` is a STORED contract, so the copies
  // were verified to agree as ORDERED constant sequences -- not as a sorted
  // set, which cannot see a misordering, the exact shape of the bug fixed at
  // task 6106 -- before any of them was removed.
  return planar::sha256::hex(input);
}

auto source_digest(std::string_view source_kind, std::int64_t source_id, std::string_view locator,
                   std::string_view semantic_source) -> std::string {
  std::string canonical;
  canonical.append(source_kind);
  canonical.push_back('\0');
  canonical.append(std::format("{}", source_id));
  canonical.push_back('\0');
  canonical.append(locator);
  canonical.push_back('\0');
  canonical.append(semantic_source);
  return sha256_hex(canonical);
}

auto artifact_section(std::string_view body, std::string_view locator) -> std::optional<std::string_view> {
  const auto hash = locator.find('#');
  if (hash == std::string_view::npos) {
    return std::nullopt;
  }
  const auto section_name = trim(locator.substr(hash + 1), whitespace_inline);
  if (section_name.empty()) {
    return std::nullopt;
  }

  std::optional<std::size_t> selected_level;
  std::size_t                section_start = 0;
  std::optional<char>        fence_char;
  std::size_t                fence_len = 0;
  std::size_t                offset    = 0;

  while (offset <= body.size()) {
    const auto line_end    = body.find('\n', offset);
    const auto real_end    = line_end == std::string_view::npos ? body.size() : line_end;
    const auto line        = body.substr(offset, real_end - offset);
    const auto next_offset = line_end == std::string_view::npos ? body.size() + 1 : line_end + 1;

    if (const auto delim = fence_delimiter(line); delim.has_value()) {
      if (!fence_char.has_value()) {
        fence_char = delim->char_;
        fence_len  = delim->len_;
      } else if (delim->char_ == *fence_char && delim->len_ >= fence_len && delim->is_closing_) {
        fence_char.reset();
        fence_len = 0;
      }
      offset = next_offset;
      continue;
    }

    if (!fence_char.has_value() && !is_indented_code(line)) {
      if (const auto head = atx_heading(line); head.has_value()) {
        if (selected_level.has_value()) {
          // A heading at the same or a shallower level closes the section; a
          // deeper one is nested content and stays inside it.
          if (head->level_ <= *selected_level) {
            return trim(body.substr(section_start, offset - section_start), whitespace_block);
          }
        } else if (equals_fold(head->title_, section_name)) {
          selected_level = head->level_;
          section_start  = std::min(next_offset, body.size());
        }
      }
    }

    if (real_end == body.size()) {
      break;
    }
    offset = next_offset;
  }

  if (!selected_level.has_value()) {
    return std::nullopt;
  }
  return trim(body.substr(section_start), whitespace_block);
}

} // namespace planar::engine::ingest::materialize
