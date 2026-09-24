/// @file routing.cpp
/// @brief Implementation of `planar.engine.workspace.routing`.

module planar.engine.workspace.routing;

import std;
import planar.json_dom;

namespace planar::engine::workspace::routing {

namespace {

/// @brief A string member, or nullopt when absent OR not a string.
///
/// Mirrors zig's `getString`: a wrong TYPE is indistinguishable from an
/// absent key, which is what makes a numeric `slug` decode to `""` rather
/// than failing.
auto get_string(const json_dom::json_value& obj, std::string_view key) -> std::optional<std::string> {
  const auto* found = obj.find(key);
  if (found == nullptr || found->kind != json_dom::json_kind::string) {
    return std::nullopt;
  }
  return found->string;
}

/// @brief An integer member, or nullopt when absent OR not an integer.
///
/// Deliberately does NOT accept `json_kind::floating`: zig's `getInteger`
/// tests `value != .integer`, so `"workspace_id": 1.0` is a missing field.
auto get_integer(const json_dom::json_value& obj, std::string_view key) -> std::optional<std::int64_t> {
  const auto* found = obj.find(key);
  if (found == nullptr || found->kind != json_dom::json_kind::integer) {
    return std::nullopt;
  }
  return found->integer;
}

/// @brief A string member with an empty-string default.
auto string_or_empty(const json_dom::json_value& obj, std::string_view key) -> std::string {
  return get_string(obj, key).value_or(std::string{});
}

/// @brief Decode an array of strings, SKIPPING every non-string element.
///
/// An absent key, or a value that is not an array, yields an empty vector —
/// not a failure.
auto parse_string_array(const json_dom::json_value* value) -> std::vector<std::string> {
  std::vector<std::string> out;
  if (value == nullptr || value->kind != json_dom::json_kind::array) {
    return out;
  }
  for (const auto& item : value->array) {
    if (item.kind == json_dom::json_kind::string) {
      out.push_back(item.string);
    }
  }
  return out;
}

/// @brief Decode an array of integers, SKIPPING every non-integer element.
auto parse_int_array(const json_dom::json_value* value) -> std::vector<std::int64_t> {
  std::vector<std::int64_t> out;
  if (value == nullptr || value->kind != json_dom::json_kind::array) {
    return out;
  }
  for (const auto& item : value->array) {
    if (item.kind == json_dom::json_kind::integer) {
      out.push_back(item.integer);
    }
  }
  return out;
}

/// @brief Decode `languages`, preserving the file's key order.
///
/// Accepts both `floating` and `integer` values (zig widens `.integer` to
/// `f64`); every other kind is skipped. `number_raw` is NOT accepted —
/// zig's arm tests the two live number kinds only, and a numeric token too
/// large for an i64 lands in neither.
auto parse_languages(const json_dom::json_value* value) -> std::vector<std::pair<std::string, double>> {
  std::vector<std::pair<std::string, double>> out;
  if (value == nullptr || value->kind != json_dom::json_kind::object) {
    return out;
  }
  for (const auto& [key, entry] : value->object) {
    if (entry.kind == json_dom::json_kind::floating) {
      out.emplace_back(key, entry.floating);
    } else if (entry.kind == json_dom::json_kind::integer) {
      out.emplace_back(key, static_cast<double>(entry.integer));
    }
  }
  return out;
}

/// @brief Decode a `planar_focus` object. REQUIRED by its caller.
auto parse_planar_focus(const json_dom::json_value* value) -> std::optional<planar_focus> {
  if (value == nullptr || value->kind != json_dom::json_kind::object) {
    return std::nullopt;
  }
  return planar_focus{
      .active_plans       = parse_int_array(value->find("active_plans")),
      .open_tasks         = get_integer(*value, "open_tasks").value_or(0),
      .open_questions     = get_integer(*value, "open_questions").value_or(0),
      .recent_session_ids = parse_int_array(value->find("recent_session_ids")),
  };
}

/// @brief Decode one `projects[]` element.
///
/// Every string field defaults to empty; only `planar_focus` can fail the
/// decode.
auto parse_project(const json_dom::json_value& value) -> std::optional<project_route> {
  if (value.kind != json_dom::json_kind::object) {
    return std::nullopt;
  }
  auto focus = parse_planar_focus(value.find("planar_focus"));
  if (!focus.has_value()) {
    return std::nullopt;
  }
  return project_route{
      .slug                = string_or_empty(value, "slug"),
      .root_path           = string_or_empty(value, "root_path"),
      .git_remote          = string_or_empty(value, "git_remote"),
      .summary             = string_or_empty(value, "summary"),
      .summary_source      = string_or_empty(value, "summary_source"),
      .capabilities        = parse_string_array(value.find("capabilities")),
      .capabilities_source = string_or_empty(value, "capabilities_source"),
      .depends_on          = parse_string_array(value.find("depends_on")),
      .depends_on_source   = string_or_empty(value, "depends_on_source"),
      .entry_points        = parse_string_array(value.find("entry_points")),
      .languages           = parse_languages(value.find("languages")),
      .focus               = *std::move(focus),
  };
}

/// @brief Decode the `cross_repo` object. `dependency_edges` is REQUIRED.
auto parse_cross_repo(const json_dom::json_value& value) -> std::optional<cross_repo> {
  if (value.kind != json_dom::json_kind::object) {
    return std::nullopt;
  }
  const auto* edges = value.find("dependency_edges");
  if (edges == nullptr || edges->kind != json_dom::json_kind::array) {
    return std::nullopt;
  }

  cross_repo out{
      .plans_scoped_to_org     = parse_int_array(value.find("plans_scoped_to_org")),
      .questions_scoped_to_org = parse_int_array(value.find("questions_scoped_to_org")),
      .dependency_edges        = {},
  };
  for (const auto& edge : edges->array) {
    // A non-object element is SKIPPED, not a failure — zig `continue`s.
    if (edge.kind != json_dom::json_kind::object) {
      continue;
    }
    out.dependency_edges.push_back(dependency_edge{
        .from   = string_or_empty(edge, "from"),
        .to     = string_or_empty(edge, "to"),
        .reason = string_or_empty(edge, "reason"),
    });
  }
  return out;
}

/// @brief Join `values` with `", "`.
auto join_commas(std::span<const std::string> values) -> std::string {
  std::string out;
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i > 0) {
      out += ", ";
    }
    out += values[i];
  }
  return out;
}

} // namespace

auto decode_error_name(decode_error value) -> std::string_view {
  switch (value) {
  case decode_error::syntax:
    return "SyntaxError";
  case decode_error::end_of_input:
    return "UnexpectedEndOfInput";
  case decode_error::duplicate_field:
    return "DuplicateField";
  case decode_error::invalid:
    return "InvalidInput";
  }
  return "InvalidInput";
}

auto from_json(const json_dom::json_value& value) -> std::optional<routing_table> {
  // BREAK-PROBE SURVIVOR, kept deliberately. Deleting this guard does not
  // fail any test, and that is correct rather than a test gap: `find()`
  // returns nullptr on a non-object, so a top-level `[]` / `42` / `"hi"` /
  // `null` / `true` falls through to the `projects` check below and yields
  // the SAME `invalid`. The guard is unobservable through this function's
  // return type by construction.
  //
  // Kept because it transcribes the oracle's own explicit
  // `if (parsed.value != .object) return error.InvalidInput;` (routing.zig
  // read()), and because a future field read that did NOT go through
  // `find()` would silently depend on it.
  if (value.kind != json_dom::json_kind::object) {
    return std::nullopt;
  }

  const auto* projects = value.find("projects");
  if (projects == nullptr || projects->kind != json_dom::json_kind::array) {
    return std::nullopt;
  }
  std::vector<project_route> decoded;
  decoded.reserve(projects->array.size());
  for (const auto& project : projects->array) {
    auto parsed = parse_project(project);
    if (!parsed.has_value()) {
      return std::nullopt;
    }
    decoded.push_back(*std::move(parsed));
  }

  const auto* cross_value = value.find("cross_repo");
  if (cross_value == nullptr) {
    return std::nullopt;
  }
  auto cross = parse_cross_repo(*cross_value);
  if (!cross.has_value()) {
    return std::nullopt;
  }

  // The five REQUIRED scalars. Each absent-or-wrong-typed one fails the
  // whole decode; `generator_version` below deliberately does not.
  auto schema = get_integer(value, "schema_version");
  auto id     = get_integer(value, "workspace_id");
  auto slug   = get_string(value, "workspace_slug");
  auto name   = get_string(value, "workspace_name");
  auto stamp  = get_string(value, "generated_at");
  if (!schema.has_value() || !id.has_value() || !slug.has_value() || !name.has_value() || !stamp.has_value()) {
    return std::nullopt;
  }

  return routing_table{
      .schema_version    = *schema,
      .workspace_id      = *id,
      .workspace_slug    = *std::move(slug),
      .workspace_name    = *std::move(name),
      .generated_at      = *std::move(stamp),
      .generator_version = get_string(value, "generator_version").value_or(std::string{generator_version_static}),
      .projects          = std::move(decoded),
      .cross             = *std::move(cross),
  };
}

auto decode(std::string_view raw) -> std::expected<routing_table, decode_error> {
  // `parse_json_reason`, not `parse_json`: the oracle interpolates zig's
  // error NAME, and three of its parse failures have three different names.
  auto parsed = json_dom::parse_json_reason(raw);
  if (!parsed.has_value()) {
    switch (parsed.error()) {
    case json_dom::json_parse_reason::end_of_input:
      return std::unexpected(decode_error::end_of_input);
    case json_dom::json_parse_reason::duplicate_field:
      return std::unexpected(decode_error::duplicate_field);
    case json_dom::json_parse_reason::syntax:
      return std::unexpected(decode_error::syntax);
    }
    return std::unexpected(decode_error::syntax);
  }
  auto table = from_json(*parsed);
  if (!table.has_value()) {
    return std::unexpected(decode_error::invalid);
  }
  return *std::move(table);
}

auto show_json(std::string_view raw) -> std::string {
  std::string out{raw};
  // The ONLY transformation this arm performs. An empty file yields a lone
  // newline; a file already ending in one is emitted unchanged.
  if (out.empty() || out.back() != '\n') {
    out += '\n';
  }
  return out;
}

auto render_text(const routing_table& table) -> std::string {
  std::string out;
  out += std::format("workspace: org:{} (id {})\n", table.workspace_slug, table.workspace_id);
  out += std::format("generated: {} ({})\n", table.generated_at, table.generator_version);
  // The blank line after the count belongs to the HEADER, so it prints even
  // when there are no projects to separate it from.
  out += std::format("projects:  {}\n\n", table.projects.size());

  for (const auto& project : table.projects) {
    out += std::format("- {}\n", project.slug);
    out += std::format("    path:         {}\n", project.root_path);
    out += std::format("    capabilities: {}\n",
                       project.capabilities.empty() ? std::string{"(none)"} : join_commas(project.capabilities));
    // Keyed on the SUMMARY, not on `summary_source`.
    out += std::format("    summary:      {}\n", project.summary.empty() ? "(no summary)" : project.summary);
    // The only per-project line that can be absent entirely.
    if (!project.depends_on.empty()) {
      out += std::format("    depends_on:   {}\n", join_commas(project.depends_on));
    }
    out += std::format("    open tasks:   {}\n", project.focus.open_tasks);
    out += std::format("    open Qs:      {}\n", project.focus.open_questions);
  }

  if (!table.cross.dependency_edges.empty()) {
    out += "\ncross-repo edges:\n";
    for (const auto& edge : table.cross.dependency_edges) {
      out += std::format("  {} -> {} ({})\n", edge.from, edge.to, edge.reason);
    }
  }
  return out;
}

auto missing_table_error(std::string_view path) -> std::string {
  return std::format("routing table not found at {}; run `planar workspace routing build` first", path);
}

} // namespace planar::engine::workspace::routing
