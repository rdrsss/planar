/// @file questions.cpp
/// @brief Implementation of `planar.engine.workbench.questions`.

module planar.engine.workbench.questions;

import std;
import planar.json_text;

namespace planar::engine::workbench::questions {

namespace {

/// @brief One body line plus its 1-based number.
struct numbered_line {
  std::string_view text;        ///< The line, newline excluded.
  std::size_t      line_no = 0; ///< 1-based within the body.
};

/// @brief The characters the oracle's `std.mem.trim(u8, x, " \t\r")` strips.
constexpr std::string_view k_trim_set = " \t\r";

/// @brief Trim `k_trim_set` from both ends.
auto trim(std::string_view text) -> std::string_view {
  auto const first = text.find_first_not_of(k_trim_set);
  if (first == std::string_view::npos) {
    return {};
  }
  auto const last = text.find_last_not_of(k_trim_set);
  return text.substr(first, last - first + 1);
}

/// @brief Drop leading whitespace, matching `std.ascii.isWhitespace`.
///
/// Wider than `k_trim_set`: it also eats `\n`, `\v`, `\f`. That only shows
/// up on the bullet test, where the line never contains a newline anyway,
/// but it is reproduced rather than narrowed.
auto trim_left_whitespace(std::string_view text) -> std::string_view {
  std::size_t idx = 0;
  while (idx < text.size() && (std::isspace(static_cast<unsigned char>(text[idx])) != 0)) {
    ++idx;
  }
  return text.substr(idx);
}

/// @brief Split a body into numbered lines.
///
/// The oracle appends a FINAL line after the last `\n` unconditionally, so
/// a body ending in a newline yields a trailing empty line. That empty line
/// is never a heading or a bullet, so it changes no output — but it does
/// shift nothing and is reproduced so the two line walks stay identical.
auto split_lines(std::string_view body) -> std::vector<numbered_line> {
  std::vector<numbered_line> out;
  std::size_t                line_no = 1;
  std::size_t                start   = 0;
  for (std::size_t i = 0; i < body.size(); ++i) {
    if (body[i] == '\n') {
      out.push_back({.text = body.substr(start, i - start), .line_no = line_no});
      ++line_no;
      start = i + 1;
    }
  }
  out.push_back({.text = body.substr(start), .line_no = line_no});
  return out;
}

/// @brief Case-insensitive ASCII equality, matching `std.ascii.eqlIgnoreCase`.
auto equals_ignore_case(std::string_view lhs, std::string_view rhs) -> bool {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  for (std::size_t i = 0; i < lhs.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(lhs[i])) != std::tolower(static_cast<unsigned char>(rhs[i]))) {
      return false;
    }
  }
  return true;
}

/// @brief Whether the line opens the `## Open Questions` section.
auto is_open_questions_h2(std::string_view line) -> bool {
  if (!line.starts_with("## ")) {
    return false;
  }
  return equals_ignore_case(trim(line.substr(3)), "open questions");
}

/// @brief Whether the line terminates the section.
auto is_h1_or_h2(std::string_view line) -> bool {
  return line.starts_with("# ") || line.starts_with("## ") || line == "#" || line == "##";
}

/// @brief Whether the line is an H3 heading.
auto is_h3(std::string_view line) -> bool {
  return line.starts_with("### ");
}

/// @brief Whether the line is a bullet at any indent.
auto is_bullet(std::string_view line) -> bool {
  auto const trimmed = trim_left_whitespace(line);
  return trimmed.starts_with("- ") || trimmed.starts_with("* ");
}

/// @brief Join a run of lines into a body, dropping blank lines at both ends.
auto join_body_lines(std::span<const numbered_line> lines) -> std::string {
  std::size_t start = 0;
  while (start < lines.size() && trim(lines[start].text).empty()) {
    ++start;
  }
  std::size_t end = lines.size();
  while (end > start && trim(lines[end - 1].text).empty()) {
    --end;
  }
  if (start >= end) {
    return {};
  }
  std::string out;
  for (std::size_t i = start; i < end; ++i) {
    out.append(lines[i].text);
    if (i + 1 < end) {
      out.push_back('\n');
    }
  }
  return out;
}

/// @brief Split a bullet at the first sentence terminator.
///
/// The terminators are `". "`, `"? "` and `"! "` — each is punctuation plus
/// a SPACE, so a bullet ending in `?` with nothing after it does not split
/// and becomes an all-title question. The title keeps the punctuation; the
/// body starts after the space.
auto split_first_sentence(std::string_view text) -> std::pair<std::string_view, std::string_view> {
  constexpr std::array<std::string_view, 3> k_terms{". ", "? ", "! "};
  std::optional<std::size_t>                earliest;
  for (auto const term : k_terms) {
    auto const at = text.find(term);
    if (at == std::string_view::npos) {
      continue;
    }
    auto const punct = at + 1;
    if (!earliest || punct < *earliest) {
      earliest = punct;
    }
  }
  if (!earliest) {
    return {trim(text), std::string_view{}};
  }
  auto const title = trim(text.substr(0, *earliest));
  auto const body  = *earliest + 1 <= text.size() ? trim(text.substr(*earliest + 1)) : std::string_view{};
  return {title, body};
}

/// @brief The H3 branch: each `### ` heading opens a question.
auto extract_h3_questions(std::span<const numbered_line> section) -> std::vector<question> {
  std::vector<question>           out;
  std::optional<std::string_view> current_title;
  std::size_t                     current_line = 0;
  std::size_t                     body_start   = 0;

  for (std::size_t idx = 0; idx < section.size(); ++idx) {
    if (!is_h3(section[idx].text)) {
      continue;
    }
    if (current_title) {
      out.push_back({.title       = std::string{*current_title},
                     .body        = join_body_lines(section.subspan(body_start, idx - body_start)),
                     .source_line = current_line});
    }
    current_title = trim(section[idx].text.substr(4));
    current_line  = section[idx].line_no;
    body_start    = idx + 1;
  }
  if (current_title) {
    out.push_back({.title       = std::string{*current_title},
                   .body        = join_body_lines(section.subspan(body_start)),
                   .source_line = current_line});
  }
  return out;
}

/// @brief The bullet branch: every bullet line is its own question.
auto extract_bullet_questions(std::span<const numbered_line> section) -> std::vector<question> {
  std::vector<question> out;
  for (auto const& line : section) {
    if (!is_bullet(line.text)) {
      continue;
    }
    // Both `- ` and `* ` drop exactly two characters; the oracle spells the
    // two arms separately but they are identical.
    auto const trimmed       = trim_left_whitespace(line.text);
    auto const raw           = trim(trimmed.substr(2));
    auto const [title, body] = split_first_sentence(raw);
    out.push_back({.title = std::string{title}, .body = std::string{body}, .source_line = line.line_no});
  }
  return out;
}

} // namespace

auto collect_top_level_specs(const std::filesystem::path& dir) -> std::vector<std::string> {
  std::vector<std::string> out;
  std::error_code          ec;
  if (!std::filesystem::is_directory(dir, ec)) {
    return out;
  }
  std::filesystem::directory_iterator it(dir, std::filesystem::directory_options::skip_permission_denied, ec);
  if (ec) {
    return out;
  }
  for (auto const& entry : it) {
    auto const name = entry.path().filename().string();
    // README.md is the feature's index page, never a spec. See the module
    // header: this skip plus the non-recursive walk is the vacuous-fixture
    // trap.
    if (name == "README.md" || !name.ends_with(".md")) {
      continue;
    }
    std::error_code entry_ec;
    if (!entry.is_regular_file(entry_ec)) {
      continue;
    }
    out.push_back(name);
  }
  std::ranges::sort(out);
  return out;
}

auto extract_questions(std::string_view body) -> std::vector<question> {
  auto const lines = split_lines(body);

  std::optional<std::size_t> section_start;
  for (std::size_t idx = 0; idx < lines.size(); ++idx) {
    if (is_open_questions_h2(lines[idx].text)) {
      section_start = idx;
      break;
    }
  }
  if (!section_start) {
    return {};
  }

  auto const start = *section_start + 1;
  auto       end   = lines.size();
  for (std::size_t i = start; i < lines.size(); ++i) {
    if (is_h1_or_h2(lines[i].text)) {
      end = i;
      break;
    }
  }
  std::span<const numbered_line> const section{lines.data() + start, end - start};

  // ONE branch for the whole section, never both: a section that mixes an
  // H3 with bullets yields only the H3 questions.
  auto const has_h3 = std::ranges::any_of(section, [](const numbered_line& line) { return is_h3(line.text); });
  return has_h3 ? extract_h3_questions(section) : extract_bullet_questions(section);
}

auto render_text(std::span<const file_questions> results) -> std::string {
  std::string out;
  for (auto const& result : results) {
    out += std::format("{} (artifact {}): {} question(s)\n", result.file, result.artifact_id, result.questions.size());
    for (auto const& item : result.questions) {
      if (item.body.empty()) {
        out += std::format("  [line {}] {}\n", item.source_line, item.title);
      } else if (item.body.size() > 60) {
        // A BYTEWISE cut, matching `q.body[0..60]`. It can split a UTF-8
        // sequence; the oracle does too, and this is the observable form.
        out += std::format("  [line {}] {} — {}…\n", item.source_line, item.title, std::string_view{item.body}.substr(0, 60));
      } else {
        out += std::format("  [line {}] {} — {}\n", item.source_line, item.title, item.body);
      }
    }
  }
  return out;
}

auto render_json(std::span<const file_questions> results) -> std::string {
  std::string out          = "[";
  bool        first_result = true;
  for (auto const& result : results) {
    if (!first_result) {
      out += ',';
    }
    first_result = false;
    out += std::format(R"({{"artifact_id":{},"file":)", result.artifact_id);
    json_text::append_json_string(out, result.file);
    out += R"(,"questions":[)";
    bool first_question = true;
    for (auto const& item : result.questions) {
      if (!first_question) {
        out += ',';
      }
      first_question = false;
      out += R"({"title":)";
      json_text::append_json_string(out, item.title);
      out += R"(,"body":)";
      json_text::append_json_string(out, item.body);
      out += std::format(R"(,"source_line":{}}})", item.source_line);
    }
    out += "]}";
  }
  out += ']';
  return out;
}

} // namespace planar::engine::workbench::questions
