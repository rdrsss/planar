/// @file parse.cpp
/// @brief Implementation of `planar.engine.workbench.parse` (plan 996, task
/// 6037). See parse.cppm for the oracle-derived accept/reject boundary.

module planar.engine.workbench.parse;

import std;

namespace planar::engine::workbench::parse {

namespace {

constexpr std::string_view k_open_delimiter = "---\n";
constexpr std::string_view k_close_inline   = "\n---\n";
constexpr std::string_view k_close_at_eof   = "\n---";

/// @brief Trim `chars` from both ends.
auto trim(std::string_view text, std::string_view chars) -> std::string_view {
  auto const first = text.find_first_not_of(chars);
  if (first == std::string_view::npos) {
    return {};
  }
  auto const last = text.find_last_not_of(chars);
  return text.substr(first, last - first + 1);
}

/// @brief Trim `chars` from the right only.
auto trim_end(std::string_view text, std::string_view chars) -> std::string_view {
  auto const last = text.find_last_not_of(chars);
  if (last == std::string_view::npos) {
    return {};
  }
  return text.substr(0, last + 1);
}

/// @brief Split on '\n', KEEPING empty segments.
///
/// zig's `std.mem.splitScalar`, not `tokenizeScalar`: a blank line inside
/// the front matter must still advance the line counter, or every
/// diagnostic after it would name the wrong line.
auto split_lines(std::string_view text) -> std::vector<std::string_view> {
  std::vector<std::string_view> out;
  std::size_t                   start = 0;
  while (true) {
    auto const nl = text.find('\n', start);
    if (nl == std::string_view::npos) {
      out.push_back(text.substr(start));
      return out;
    }
    out.push_back(text.substr(start, nl - start));
    start = nl + 1;
  }
}

/// @brief Count a file's lines the way the missing-close diagnostic does.
auto count_lines(std::string_view content) -> std::size_t {
  if (content.empty()) {
    return 1;
  }
  auto const newlines = static_cast<std::size_t>(std::ranges::count(content, '\n'));
  return newlines + (content.back() != '\n' ? 1U : 0U);
}

/// @brief The YAML block and the body, or unset when no closing `---` exists.
///
/// `rest` is everything after the opening `---\n`. The closer is the FIRST
/// `\n---\n`; a nested front-matter block later in the body therefore does
/// NOT terminate the header (the double-wrap case, pinned in parse.t.cpp).
/// A file ending in exactly `\n---` also closes, with an empty body.
auto find_close(std::string_view rest) -> std::optional<std::pair<std::string_view, std::string_view>> {
  if (auto const end = rest.find(k_close_inline); end != std::string_view::npos) {
    auto const yaml = rest.substr(0, end);
    auto       body = rest.substr(end + k_close_inline.size());
    if (!body.empty() && body.front() == '\n') {
      body.remove_prefix(1);
    }
    return std::pair{yaml, body};
  }
  if (rest.size() >= k_close_at_eof.size() && rest.ends_with(k_close_at_eof)) {
    return std::pair{rest.substr(0, rest.size() - k_close_at_eof.size()), std::string_view{}};
  }
  return std::nullopt;
}

/// @brief True when `value` is a well-formed `<kind>:<positive-id>` ref.
auto valid_entity_ref(std::string_view value) -> bool {
  auto const colon = value.find(':');
  if (colon == std::string_view::npos) {
    return false;
  }
  auto const kind    = trim(value.substr(0, colon), " \t");
  auto const id_text = trim(value.substr(colon + 1), " \t");
  if (kind.empty() || id_text.empty()) {
    return false;
  }
  auto const id = parse_int64_zig(id_text);
  return id.has_value() && *id > 0;
}

/// @brief Which multi-line list a `- ` item belongs to, if any.
enum class list_field : std::uint8_t { none, touches, verifies, cites, derives_from };

/// @brief Map a front-matter key onto the list it introduces.
auto list_for_key(std::string_view key) -> list_field {
  if (key == "touches") {
    return list_field::touches;
  }
  if (key == "verifies") {
    return list_field::verifies;
  }
  if (key == "cites") {
    return list_field::cites;
  }
  // NOTE the DASH. `derives_from:` (underscore) is an unknown key, so the
  // `- plan:1` items under it become orphan scalars and the file is
  // REFUSED with `leading_dash_scalar`. Oracle-probed; a plausible typo
  // that fails loudly rather than silently dropping the refs.
  if (key == "derives-from") {
    return list_field::derives_from;
  }
  return list_field::none;
}

/// @brief True when `value` is in the comma-separated prose `list`.
///
/// The prose carries an Oxford `or ` on its last element (`"..., done, or
/// cancelled"`), which is stripped before comparison — the same string
/// serves as both the check and the operator-facing hint.
auto value_in_list(std::string_view value, std::string_view list) -> bool {
  std::size_t start = 0;
  while (start <= list.size()) {
    auto const comma     = list.find(',', start);
    auto const piece     = list.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start);
    auto       candidate = trim(piece, " ");
    if (candidate.starts_with("or ")) {
      candidate.remove_prefix(3);
    }
    if (candidate == value) {
      return true;
    }
    if (comma == std::string_view::npos) {
      return false;
    }
    start = comma + 1;
  }
  return false;
}

/// @brief Build a `missing_required_field` diagnostic with its SYNTHETIC line.
auto missing_field(std::string_view field, std::size_t line) -> diagnostic {
  return diagnostic{
      .err      = parse_error_kind::missing_required_field,
      .reason   = diagnostic_reason::missing_required_field,
      .line     = line,
      .field    = field,
      .expected = "a non-empty value",
  };
}

} // namespace

auto error_name(parse_error_kind kind) -> std::string_view {
  switch (kind) {
  case parse_error_kind::malformed_frontmatter:
    return "MalformedFrontmatter";
  case parse_error_kind::missing_required_field:
    return "MissingRequiredField";
  case parse_error_kind::invalid_entity_kind:
    return "InvalidEntityKind";
  case parse_error_kind::invalid_field_value:
    return "InvalidFieldValue";
  }
  return "MalformedFrontmatter";
}

auto parse_int64_zig(std::string_view raw) -> std::optional<std::int64_t> {
  std::string_view body     = raw;
  bool             negative = false;
  if (!body.empty() && (body.front() == '+' || body.front() == '-')) {
    negative = body.front() == '-';
    body.remove_prefix(1);
  }
  auto const is_digit = [](char c) { return c >= '0' && c <= '9'; };
  // zig refuses a leading or trailing digit separator outright, then skips
  // every other one. Requiring a digit at both ends is the same predicate
  // and also rejects `0x10`, `1 2` and a bare `+`. All four oracle-probed.
  if (body.empty() || !is_digit(body.front()) || !is_digit(body.back())) {
    return std::nullopt;
  }
  std::string digits;
  digits.reserve(body.size() + 1);
  if (negative) {
    digits.push_back('-');
  }
  for (char const c : body) {
    if (c == '_') {
      continue;
    }
    if (!is_digit(c)) {
      return std::nullopt;
    }
    digits.push_back(c);
  }
  std::int64_t value   = 0;
  auto const [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), value, 10);
  if (ec != std::errc{} || ptr != digits.data() + digits.size()) {
    return std::nullopt;
  }
  return value;
}

auto strip_yaml_quotes(std::string_view value) -> std::string_view {
  if (value.size() >= 2) {
    bool const single = value.front() == '\'' && value.back() == '\'';
    bool const dbl    = value.front() == '"' && value.back() == '"';
    if (single || dbl) {
      return value.substr(1, value.size() - 2);
    }
  }
  return value;
}

auto statuses_for_kind(std::string_view kind) -> std::string_view {
  if (kind == "plan") {
    return "draft, active, paused, done, or abandoned";
  }
  if (kind == "task") {
    return "todo, doing, blocked, done, or cancelled";
  }
  if (kind == "artifact") {
    return "draft, active, superseded, or retired";
  }
  if (kind == "scenario") {
    return "draft, ready, verified, failing, or retired";
  }
  if (kind == "decision") {
    return "proposed, accepted, superseded, or withdrawn";
  }
  return "open, answered, or wontfix";
}

auto artifact_kinds() -> std::string_view {
  return "tech_spec, adr, design_note, summary, readme, generated, other, product_spec, roadmap, research, "
         "getting_started, changelog_entry, glossary_term, or test_spec";
}

auto is_entity_kind(std::string_view kind) -> bool {
  return kind == "plan" || kind == "task" || kind == "artifact" || kind == "scenario" || kind == "decision" || kind == "question";
}

auto diagnose(std::string_view content) -> std::optional<diagnostic> {
  if (!content.starts_with(k_open_delimiter)) {
    return diagnostic{
        .err = parse_error_kind::malformed_frontmatter, .reason = diagnostic_reason::missing_open_delimiter, .line = 1};
  }

  auto const closed = find_close(content.substr(k_open_delimiter.size()));
  if (!closed) {
    return diagnostic{.err    = parse_error_kind::malformed_frontmatter,
                      .reason = diagnostic_reason::missing_close_delimiter,
                      .line   = count_lines(content)};
  }
  auto const yaml = closed->first;

  std::string_view kind_value;
  std::string_view status_value;
  std::string_view artifact_kind_value;
  std::int64_t     entity_id          = 0;
  bool             saw_entity_kind    = false;
  bool             saw_entity_id      = false;
  bool             saw_title          = false;
  bool             saw_status         = false;
  bool             saw_artifact_kind  = false;
  std::size_t      entity_kind_line   = 2;
  std::size_t      status_line        = 6;
  std::size_t      artifact_kind_line = 7;

  list_field  current = list_field::none;
  std::size_t line_no = 2;
  for (auto const& raw_line : split_lines(yaml)) {
    auto const advance = [&] { ++line_no; };
    // Indentation scan first: a TAB anywhere in the leading whitespace run
    // is refused before anything else looks at the line.
    for (char const c : raw_line) {
      if (c == '\t') {
        return diagnostic{
            .err = parse_error_kind::malformed_frontmatter, .reason = diagnostic_reason::tab_indentation, .line = line_no};
      }
      if (c != ' ') {
        break;
      }
    }
    auto const line = trim_end(raw_line, " \t\r");
    if (line.empty()) {
      advance();
      continue;
    }

    if (line.starts_with("- ")) {
      if (current == list_field::none) {
        return diagnostic{
            .err = parse_error_kind::malformed_frontmatter, .reason = diagnostic_reason::leading_dash_scalar, .line = line_no};
      }
      if (current != list_field::touches) {
        auto const item = trim(line.substr(2), " \t");
        if (!valid_entity_ref(item)) {
          return diagnostic{
              .err = parse_error_kind::malformed_frontmatter, .reason = diagnostic_reason::invalid_entity_ref, .line = line_no};
        }
      }
      advance();
      continue;
    }

    auto const colon = line.find(':');
    if (colon == std::string_view::npos) {
      return diagnostic{
          .err = parse_error_kind::malformed_frontmatter, .reason = diagnostic_reason::malformed_yaml, .line = line_no};
    }
    auto const key = trim(line.substr(0, colon), " \t");
    if (key.empty()) {
      return diagnostic{
          .err = parse_error_kind::malformed_frontmatter, .reason = diagnostic_reason::malformed_yaml, .line = line_no};
    }
    auto const raw_value = trim(line.substr(colon + 1), " \t");
    if (!raw_value.empty() && (raw_value.front() == '\'' || raw_value.front() == '"')) {
      if (raw_value.size() < 2 || raw_value.back() != raw_value.front()) {
        return diagnostic{
            .err = parse_error_kind::malformed_frontmatter, .reason = diagnostic_reason::malformed_yaml, .line = line_no};
      }
    } else {
      if (raw_value.find(": ") != std::string_view::npos) {
        return diagnostic{
            .err = parse_error_kind::malformed_frontmatter, .reason = diagnostic_reason::unquoted_colon, .line = line_no};
      }
      if (raw_value.starts_with("- ")) {
        return diagnostic{
            .err = parse_error_kind::malformed_frontmatter, .reason = diagnostic_reason::leading_dash_scalar, .line = line_no};
      }
    }
    auto const value = strip_yaml_quotes(raw_value);
    current          = list_for_key(key);

    if (key == "entity_kind") {
      kind_value       = value;
      saw_entity_kind  = !value.empty();
      entity_kind_line = line_no;
    } else if (key == "entity_id") {
      auto const parsed = parse_int64_zig(value);
      if (!parsed) {
        return diagnostic{.err    = parse_error_kind::malformed_frontmatter,
                          .reason = diagnostic_reason::invalid_integer,
                          .line   = line_no,
                          .field  = "entity_id"};
      }
      entity_id     = *parsed;
      saw_entity_id = true;
    } else if (key == "anchor_plan_id" || key == "priority") {
      if (!parse_int64_zig(value)) {
        return diagnostic{.err    = parse_error_kind::malformed_frontmatter,
                          .reason = diagnostic_reason::invalid_integer,
                          .line   = line_no,
                          .field  = key};
      }
    } else if (key == "title") {
      saw_title = !value.empty();
    } else if (key == "status") {
      status_value = value;
      saw_status   = !value.empty();
      status_line  = line_no;
    } else if (key == "artifact_kind") {
      artifact_kind_value = value;
      saw_artifact_kind   = !value.empty();
      artifact_kind_line  = line_no;
    }
    advance();
  }

  if (!saw_entity_kind) {
    return missing_field("entity_kind", 2);
  }
  if (!saw_entity_id || entity_id <= 0) {
    return missing_field("entity_id", 3);
  }
  if (!is_entity_kind(kind_value)) {
    return diagnostic{.err      = parse_error_kind::invalid_entity_kind,
                      .reason   = diagnostic_reason::invalid_entity_kind,
                      .line     = entity_kind_line,
                      .field    = "entity_kind",
                      .expected = "plan, task, artifact, scenario, decision, or question"};
  }
  if (!saw_title) {
    return missing_field("title", 5);
  }
  if (!saw_status) {
    return missing_field("status", 6);
  }
  auto const expected_status = statuses_for_kind(kind_value);
  if (!value_in_list(status_value, expected_status)) {
    return diagnostic{.err      = parse_error_kind::invalid_field_value,
                      .reason   = diagnostic_reason::invalid_field_value,
                      .line     = status_line,
                      .field    = "status",
                      .expected = expected_status};
  }
  if (kind_value == "artifact") {
    if (!saw_artifact_kind) {
      return missing_field("artifact_kind", 7);
    }
    if (!value_in_list(artifact_kind_value, artifact_kinds())) {
      return diagnostic{.err      = parse_error_kind::invalid_field_value,
                        .reason   = diagnostic_reason::invalid_field_value,
                        .line     = artifact_kind_line,
                        .field    = "artifact_kind",
                        .expected = artifact_kinds()};
    }
  }
  return std::nullopt;
}

auto parse(std::string_view content) -> std::expected<parse_result, parse_error_kind> {
  // `diagnose` IS the gate. Anything it accepts, the field walk below can
  // read without re-validating; anything it refuses never reaches here.
  if (auto const bad = diagnose(content)) {
    return std::unexpected(bad->err);
  }

  auto const closed = find_close(content.substr(k_open_delimiter.size()));
  if (!closed) {
    return std::unexpected(parse_error_kind::malformed_frontmatter);
  }

  parse_result result;
  result.body        = closed->second;
  list_field current = list_field::none;

  for (auto const& raw_line : split_lines(closed->first)) {
    auto const line = trim_end(raw_line, " \t\r");
    if (line.empty()) {
      continue;
    }
    if (line.starts_with("- ")) {
      auto const item = trim(line.substr(2), " \t");
      switch (current) {
      case list_field::touches:
        result.frontmatter.touches.emplace_back(item);
        break;
      case list_field::verifies:
      case list_field::cites:
      case list_field::derives_from: {
        auto const colon   = item.find(':');
        auto const kind    = trim(item.substr(0, colon), " \t");
        auto const id_text = trim(item.substr(colon + 1), " \t");
        entity_ref ref{.kind = std::string{kind}, .id = parse_int64_zig(id_text).value_or(0)};
        if (current == list_field::verifies) {
          result.frontmatter.verifies.push_back(std::move(ref));
        } else if (current == list_field::cites) {
          result.frontmatter.cites.push_back(std::move(ref));
        } else {
          result.frontmatter.derives_from.push_back(std::move(ref));
        }
        break;
      }
      case list_field::none:
        // Unreachable: `diagnose` refuses an orphan `- ` item.
        break;
      }
      continue;
    }

    auto const colon = line.find(':');
    if (colon == std::string_view::npos) {
      continue;
    }
    auto const key   = trim(line.substr(0, colon), " \t");
    auto const value = strip_yaml_quotes(trim(line.substr(colon + 1), " \t"));
    current          = list_for_key(key);

    if (key == "entity_kind") {
      result.frontmatter.entity_kind = std::string{value};
    } else if (key == "entity_id") {
      result.frontmatter.entity_id = parse_int64_zig(value).value_or(0);
    } else if (key == "anchor_plan_id") {
      result.frontmatter.anchor_plan_id = parse_int64_zig(value).value_or(0);
    } else if (key == "title") {
      result.frontmatter.title = std::string{value};
    } else if (key == "status") {
      result.frontmatter.status = std::string{value};
    } else if (key == "priority") {
      result.frontmatter.priority = parse_int64_zig(value).value_or(0);
    } else if (key == "scope") {
      result.frontmatter.scope = std::string{value};
    } else if (key == "artifact_kind") {
      result.frontmatter.artifact_kind = std::string{value};
    }
    // Unknown keys are silently ignored, mirroring Go. Oracle-probed.
  }

  return result;
}

} // namespace planar::engine::workbench::parse
