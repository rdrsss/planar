/// @file lint.cpp
/// @brief Implementation of `planar.engine.workbench.lint` (plan 996, task
/// 6037). Every message and hint string below is oracle-captured from
/// `workbench lint --path <file> --json`.

module planar.engine.workbench.lint;

import std;
import planar.db;
import planar.engine.workbench.fsutil;
import planar.engine.workbench.parse;

namespace planar::engine::workbench::lint {

namespace {

auto code_for(parse::parse_error_kind kind) -> std::string_view {
  switch (kind) {
  case parse::parse_error_kind::malformed_frontmatter:
    return "malformed_frontmatter";
  case parse::parse_error_kind::missing_required_field:
    return "missing_required_field";
  case parse::parse_error_kind::invalid_entity_kind:
    return "invalid_entity_kind";
  case parse::parse_error_kind::invalid_field_value:
    return "invalid_field_value";
  }
  return "malformed_frontmatter";
}

/// @brief The `front matter is malformed: <detail>` tail.
auto malformed_detail(parse::diagnostic_reason reason) -> std::string_view {
  switch (reason) {
  case parse::diagnostic_reason::missing_open_delimiter:
    return "opening delimiter is missing";
  case parse::diagnostic_reason::missing_close_delimiter:
    return "closing delimiter is missing";
  case parse::diagnostic_reason::tab_indentation:
    return "tab indentation is not valid YAML";
  case parse::diagnostic_reason::unquoted_colon:
    return "an unquoted scalar contains ': '";
  case parse::diagnostic_reason::leading_dash_scalar:
    return "an unquoted scalar begins with '- '";
  case parse::diagnostic_reason::invalid_integer:
    return "an integer field is invalid";
  case parse::diagnostic_reason::invalid_entity_ref:
    return "an entity reference is invalid";
  default:
    return "YAML syntax is invalid";
  }
}

auto message_for(const parse::diagnostic& diagnostic) -> std::string {
  switch (diagnostic.err) {
  case parse::parse_error_kind::malformed_frontmatter:
    return std::format("front matter is malformed: {}", malformed_detail(diagnostic.reason));
  case parse::parse_error_kind::missing_required_field:
    return std::format("front matter is missing required field '{}'", diagnostic.field);
  case parse::parse_error_kind::invalid_entity_kind:
    return "front matter field 'entity_kind' is not supported";
  case parse::parse_error_kind::invalid_field_value:
    return std::format("front matter field '{}' has an unsupported value", diagnostic.field);
  }
  return {};
}

auto hint_for(const parse::diagnostic& diagnostic) -> std::string {
  switch (diagnostic.reason) {
  case parse::diagnostic_reason::missing_open_delimiter:
    return "start the file with a '---' front matter delimiter";
  case parse::diagnostic_reason::missing_close_delimiter:
    return "add a closing '---' delimiter on its own line";
  case parse::diagnostic_reason::tab_indentation:
    return "replace tab indentation with spaces";
  case parse::diagnostic_reason::unquoted_colon:
    return "quote the scalar with single or double quotes because it contains ': '";
  case parse::diagnostic_reason::leading_dash_scalar:
    return "quote the scalar because values beginning with '- ' are YAML sequence indicators";
  case parse::diagnostic_reason::invalid_integer:
    return std::format("set {} to a base-10 integer", diagnostic.field);
  case parse::diagnostic_reason::invalid_entity_ref:
    return "use '<entity-kind>:<positive-id>' for each reference";
  case parse::diagnostic_reason::missing_required_field:
    return std::format("add required front matter field '{}'", diagnostic.field);
  case parse::diagnostic_reason::invalid_entity_kind:
  case parse::diagnostic_reason::invalid_field_value:
    return std::format("use one of: {}", diagnostic.expected);
  case parse::diagnostic_reason::malformed_yaml:
    return "check YAML key/value syntax in the front matter block";
  }
  return {};
}

/// @brief The 1-based line on which `key:` appears, or 1 when it does not.
///
/// Used only for the `anchor_plan_not_found` warning, whose line is REAL
/// (unlike the parse diagnostics' synthetic ones) when the key is present —
/// and falls back to 1 when the key is absent entirely, which is the
/// oracle-observed line for a file with no `anchor_plan_id` at all.
auto line_for_key(std::string_view content, std::string_view key) -> std::size_t {
  std::size_t line_no = 1;
  std::size_t pos     = 0;
  while (pos <= content.size()) {
    auto const nl    = content.find('\n', pos);
    auto       line  = content.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
    auto const first = line.find_first_not_of(" \t");
    if (first != std::string_view::npos) {
      line.remove_prefix(first);
      if (line.starts_with(key) && line.size() > key.size() && line[key.size()] == ':') {
        return line_no;
      }
    }
    if (nl == std::string_view::npos) {
      break;
    }
    pos = nl + 1;
    ++line_no;
  }
  return 1;
}

auto plan_exists(db::connection& conn, std::int64_t plan_id) -> std::expected<bool, lint_error> {
  auto stmt = conn.prepare("select 1 from plans where id = ? limit 1");
  if (!stmt || !stmt->bind_int64(1, plan_id)) {
    return std::unexpected(lint_error::query_failed);
  }
  auto stepped = stmt->step();
  if (!stepped) {
    return std::unexpected(lint_error::query_failed);
  }
  return *stepped == db::step_result::row;
}

} // namespace

auto severity_name(severity value) -> std::string_view {
  return value == severity::warning ? "warning" : "error";
}

auto run(db::connection& conn, const std::filesystem::path& target) -> std::expected<result, lint_error> {
  std::error_code ec;
  auto const      status = std::filesystem::status(target, ec);
  if (ec || !std::filesystem::exists(status)) {
    return std::unexpected(lint_error::not_found);
  }

  std::vector<std::string> files;
  if (std::filesystem::is_regular_file(status)) {
    if (!target.string().ends_with(".md")) {
      return std::unexpected(lint_error::invalid_input);
    }
    files.push_back(target.string());
  } else if (std::filesystem::is_directory(status)) {
    files = fsutil::collect_markdown(target);
  } else {
    return std::unexpected(lint_error::invalid_input);
  }
  std::ranges::sort(files);

  result out;
  out.files_scanned = files.size();
  for (auto const& path : files) {
    auto const content = fsutil::read_file(path);
    if (!content) {
      continue;
    }
    if (auto const diagnostic = parse::diagnose(*content)) {
      out.issues.push_back(issue{.path    = path,
                                 .line    = diagnostic->line,
                                 .level   = severity::error,
                                 .code    = std::string{code_for(diagnostic->err)},
                                 .message = message_for(*diagnostic),
                                 .hint    = hint_for(*diagnostic)});
      ++out.errors;
      continue;
    }
    auto const parsed = parse::parse(*content);
    if (!parsed) {
      continue; // Unreachable: `diagnose` accepted the file.
    }
    auto const anchor_plan_id = parsed->frontmatter.anchor_plan_id;
    bool       missing        = anchor_plan_id <= 0;
    if (!missing) {
      auto const exists = plan_exists(conn, anchor_plan_id);
      if (!exists) {
        return std::unexpected(exists.error());
      }
      missing = !*exists;
    }
    if (missing) {
      out.issues.push_back(issue{
          .path  = path,
          .line  = line_for_key(*content, "anchor_plan_id"),
          .level = severity::warning,
          .code  = "anchor_plan_not_found",
          // Two distinct messages: absent/zero vs. present-but-dangling.
          .message = anchor_plan_id <= 0 ? std::string{"front matter field 'anchor_plan_id' must reference an existing plan"}
                                         : std::format("anchor_plan_id {} does not reference an existing plan", anchor_plan_id),
          .hint    = "set anchor_plan_id to the owning top-level plan ID"});
      ++out.warnings;
    }
  }
  return out;
}

} // namespace planar::engine::workbench::lint
