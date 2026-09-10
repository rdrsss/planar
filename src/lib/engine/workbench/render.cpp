/// @file render.cpp
/// @brief Implementation of `planar.engine.workbench.render` (plan 996, task
/// 6037). See render.cppm for the oracle-captured byte shape.

module planar.engine.workbench.render;

import std;
import planar.engine.workbench.parse;

namespace planar::engine::workbench::render {

namespace {

auto append_string_field(std::string& out, std::string_view key, std::string_view value) -> void {
  if (needs_yaml_quote(value)) {
    std::format_to(std::back_inserter(out), "{}: '{}'\n", key, value);
    return;
  }
  std::format_to(std::back_inserter(out), "{}: {}\n", key, value);
}

auto append_int_field(std::string& out, std::string_view key, std::int64_t value) -> void {
  std::format_to(std::back_inserter(out), "{}: {}\n", key, value);
}

auto append_string_list(std::string& out, std::string_view key, std::span<const std::string> items) -> void {
  std::format_to(std::back_inserter(out), "{}:\n", key);
  for (auto const& item : items) {
    std::format_to(std::back_inserter(out), "- {}\n", item);
  }
}

auto append_ref_list(std::string& out, std::string_view key, std::span<const parse::entity_ref> refs) -> void {
  std::format_to(std::back_inserter(out), "{}:\n", key);
  for (auto const& ref : refs) {
    std::format_to(std::back_inserter(out), "- {}:{}\n", ref.kind, ref.id);
  }
}

} // namespace

auto needs_yaml_quote(std::string_view value) -> bool {
  if (value.empty()) {
    return true;
  }
  switch (value.front()) {
  case '{':
  case '}':
  case '[':
  case ']':
  case ',':
  case '#':
  case '&':
  case '*':
  case '!':
  case '|':
  case '>':
  case '\'':
  case '"':
  case '%':
  case '@':
  case '`':
    return true;
  case '-':
    if (value.size() > 1 && value[1] == ' ') {
      return true;
    }
    break;
  default:
    break;
  }
  if (value.find(": ") != std::string_view::npos) {
    return true;
  }
  return value.back() == ':';
}

auto render(const parse::front_matter& fm, std::string_view body) -> std::string {
  std::string out;
  out += "---\n";

  // The three identity fields are UNCONDITIONAL, including a zero
  // `anchor_plan_id` — `entity_kind: plan\nentity_id: 1\nanchor_plan_id: 1`
  // is the invariant prefix of every file the oracle writes.
  append_string_field(out, "entity_kind", fm.entity_kind);
  append_int_field(out, "entity_id", fm.entity_id);
  append_int_field(out, "anchor_plan_id", fm.anchor_plan_id);

  if (!fm.title.empty()) {
    append_string_field(out, "title", fm.title);
  }
  if (!fm.status.empty()) {
    append_string_field(out, "status", fm.status);
  }
  if (fm.priority != 0) {
    append_int_field(out, "priority", fm.priority);
  }
  if (!fm.scope.empty()) {
    append_string_field(out, "scope", fm.scope);
  }
  if (!fm.artifact_kind.empty()) {
    append_string_field(out, "artifact_kind", fm.artifact_kind);
  }
  if (!fm.touches.empty()) {
    append_string_list(out, "touches", fm.touches);
  }
  if (!fm.verifies.empty()) {
    append_ref_list(out, "verifies", fm.verifies);
  }
  if (!fm.cites.empty()) {
    append_ref_list(out, "cites", fm.cites);
  }
  if (!fm.derives_from.empty()) {
    append_ref_list(out, "derives-from", fm.derives_from);
  }

  out += "---\n";
  if (!body.empty()) {
    out += '\n';
    out += body;
  }
  return out;
}

} // namespace planar::engine::workbench::render
