/// @file render.cpp
/// @brief Implementation of `planar.engine.workflows.render` (plan 996, task
/// 6096). See render.cppm for scope and the oracle-derived output shapes.

module planar.engine.workflows.render;

import std;
import planar.engine.workflows.catalog;

namespace planar::engine::workflows::render {

namespace {

/// @brief Append `text` to `out` as a quoted JSON string.
///
/// Same escaping table as engine/models/render.cpp's: the two mandatory
/// escapes, the five short forms, LOWERCASE `\u00xx` for the remaining C0
/// bytes, and no escaping of `/` or of non-ASCII. Duplicated rather than
/// shared because the two buckets are layer-2 peers and a sideways
/// `engine_*` -> `engine_*` dependency is forbidden (D15/D18); extracting it
/// to layer 1 under D19 would be the alternative, and is worth doing once a
/// THIRD bucket needs it rather than on the second.
///
/// This matters here specifically because a workflow's `path` and
/// `description` are arbitrary operator-authored strings: a Windows-style
/// path would carry backslashes, and a description can hold quotes.
auto append_json_string(std::string& out, std::string_view text) -> void {
  out.push_back('"');
  for (const char raw : text) {
    const auto c = static_cast<unsigned char>(raw);
    switch (c) {
    case '\\':
      out.append("\\\\");
      break;
    case '"':
      out.append("\\\"");
      break;
    case 0x08:
      out.append("\\b");
      break;
    case 0x0C:
      out.append("\\f");
      break;
    case '\n':
      out.append("\\n");
      break;
    case '\r':
      out.append("\\r");
      break;
    case '\t':
      out.append("\\t");
      break;
    default:
      if (c < 0x20) {
        out.append(std::format("\\u{:04x}", static_cast<unsigned>(c)));
      } else {
        out.push_back(raw);
      }
      break;
    }
  }
  out.push_back('"');
}

auto append_field(std::string& out, std::string_view key, std::string_view value) -> void {
  append_json_string(out, key);
  out.push_back(':');
  append_json_string(out, value);
}

/// @brief Left-align in `width` columns WITHOUT truncating an over-long value.
///
/// Matching zig's `{s:<N}`: a 48-character workflow name pushes the rest of
/// the row right rather than being cut, in both implementations.
auto pad(std::string_view text, std::size_t width) -> std::string {
  std::string out(text);
  while (out.size() < width) {
    out.push_back(' ');
  }
  return out;
}

auto kind_text(const catalog::entry& value) -> std::string_view {
  return value.is_local ? std::string_view{"local"} : std::string_view{"shipped"};
}

} // namespace

auto entry_json(const catalog::entry& value) -> std::string {
  std::string out = "{";
  append_field(out, "name", catalog::effective_name(value));
  out.push_back(',');
  append_field(out, "kind", kind_text(value));
  out.push_back(',');
  append_field(out, "path", value.path);
  out.push_back(',');
  append_field(out, "filename", value.filename);
  out.append(std::format(",\"meta_found\":{},", value.meta_found ? "true" : "false"));
  append_field(out, "description", value.meta.description);
  out.push_back(',');
  append_field(out, "phases", value.meta.phases);
  out.push_back(',');
  append_field(out, "seam", value.meta.seam);
  out.append("}\n");
  return out;
}

auto list_json(std::span<const catalog::entry> entries) -> std::string {
  // NDJSON: no enclosing brackets, no separating commas. An EMPTY catalog
  // therefore renders as the empty string, not `[]` -- oracle-confirmed as
  // zero bytes on stdout.
  std::string out;
  for (const auto& value : entries) {
    out.append(entry_json(value));
  }
  return out;
}

auto list_text(std::span<const catalog::entry> entries, bool local_only) -> std::string {
  if (entries.empty()) {
    // The sentence names WHICH sources were searched, so an operator who
    // passed --local is not left wondering whether shipped workflows exist.
    return local_only ? "no sandbox workflows found\n" : "no shipped + sandbox workflows found\n";
  }
  std::string out = std::format("{}  {}  {}  {}\n", pad("name", 24), pad("kind", 8), pad("phases", 20), "description");
  for (const auto& value : entries) {
    out.append(std::format("{}  {}  {}  {}\n", pad(catalog::effective_name(value), 24), pad(kind_text(value), 8),
                           pad(value.meta.phases, 20), value.meta.description));
  }
  return out;
}

auto show_text(const catalog::entry& value) -> std::string {
  std::string out;
  out.append(std::format("name:        {}\n", catalog::effective_name(value)));
  out.append(std::format("kind:        {}\n", kind_text(value)));
  out.append(std::format("path:        {}\n", value.path));
  out.append(std::format("meta:        {}\n", value.meta_found ? "present" : "absent"));
  // The remaining three are printed ONLY when non-empty. A bare
  // `description:` with nothing after it would read as a field that exists
  // and is blank, which is a different claim from "this workflow declares no
  // description".
  if (!value.meta.description.empty()) {
    out.append(std::format("description: {}\n", value.meta.description));
  }
  if (!value.meta.phases.empty()) {
    out.append(std::format("phases:      {}\n", value.meta.phases));
  }
  if (!value.meta.seam.empty()) {
    out.append(std::format("seam:        {}\n", value.meta.seam));
  }
  return out;
}

auto not_found_error(std::string_view name) -> std::string {
  return std::format("error: workflow '{}' not found\n", name);
}

} // namespace planar::engine::workflows::render
