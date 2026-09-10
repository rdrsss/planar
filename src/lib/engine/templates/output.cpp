/// @file output.cpp
/// @brief Implementation of `planar.engine.templates.output` (plan 996,
/// task 6190). See output.cppm for the four distinct empty-output
/// spellings and the two leaves that ignore flags they declare.

module planar.engine.templates.output;

import std;
import planar.json_text;
import planar.engine.templates.validate;

namespace planar::engine::templates {

namespace {

using json_text::append_json_string;

/// @brief Column width for SET/SYSTEM/KIND in `list`'s text mode.
///
/// zig's format string is `"{s:<20} {s:<20} {s:<20} {s}\n"` — LEFT-aligned,
/// padded to 20, one literal space between columns, and the final column
/// unpadded. An over-long value is never truncated; it pushes the rest of
/// the row right.
constexpr std::size_t k_col = 20;

/// @brief The rule under `list`'s header: exactly 70 dashes.
constexpr std::size_t k_rule = 70;

/// @brief Left-align `text` in a `k_col`-wide field, never truncating.
/// @param text The value.
/// @return The padded value.
auto pad(std::string_view text) -> std::string {
  std::string out{text};
  if (out.size() < k_col) {
    out.append(k_col - out.size(), ' ');
  }
  return out;
}

/// @brief Whether a filter is active. An ABSENT filter and an EMPTY one
/// both mean "no filter" — see `merge_list_rows`'s contract.
/// @param filter The filter.
/// @return `true` when the filter should be applied.
auto filter_active(const std::optional<std::string_view>& filter) -> bool {
  return filter.has_value() && !filter->empty();
}

} // namespace

auto merge_list_rows(std::span<const list_row> disk, std::span<const list_row> embedded,
                     std::optional<std::string_view> system_filter, std::optional<std::string_view> set_filter)
    -> std::vector<list_row> {
  std::vector<list_row> merged;
  merged.reserve(disk.size() + embedded.size());

  for (auto const& e : disk) {
    merged.push_back(e);
  }
  // Disk supersedes embedded on a matching triple — so a post-`init` root
  // reports every row as `disk`.
  for (auto const& e : embedded) {
    auto const shadowed = std::ranges::any_of(
        disk, [&](auto const& d) { return d.set_name == e.set_name && d.system == e.system && d.kind == e.kind; });
    if (!shadowed) {
      merged.push_back(e);
    }
  }

  std::vector<list_row> kept;
  kept.reserve(merged.size());
  for (auto& row : merged) {
    if (filter_active(system_filter) && row.system != *system_filter) {
      continue;
    }
    if (filter_active(set_filter) && row.set_name != *set_filter) {
      continue;
    }
    kept.push_back(std::move(row));
  }

  std::ranges::sort(kept, [](const list_row& a, const list_row& b) {
    return std::tie(a.set_name, a.system, a.kind) < std::tie(b.set_name, b.system, b.kind);
  });
  return kept;
}

auto list_text(std::span<const list_row> rows) -> std::string {
  if (rows.empty()) {
    return "no templates found\n";
  }
  std::string out;
  out += std::format("{} {} {} {}\n", pad("SET"), pad("SYSTEM"), pad("KIND"), "SOURCE");
  out.append(k_rule, '-');
  out += '\n';
  for (auto const& row : rows) {
    out += std::format("{} {} {} {}\n", pad(row.set_name), pad(row.system), pad(row.kind), row.source);
  }
  return out;
}

auto list_json(std::span<const list_row> rows) -> std::string {
  // NDJSON. ZERO BYTES when empty — not `[]`. See output.cppm.
  std::string out;
  for (auto const& row : rows) {
    out += R"({"set":)";
    append_json_string(out, row.set_name);
    out += R"(,"system":)";
    append_json_string(out, row.system);
    out += R"(,"kind":)";
    append_json_string(out, row.kind);
    out += R"(,"source":)";
    append_json_string(out, row.source);
    out += R"(,"path":)";
    append_json_string(out, row.path);
    out += "}\n";
  }
  return out;
}

auto show_text(std::string_view raw) -> std::string {
  std::string out{raw};
  // A newline is appended ONLY when one is missing, so a file that already
  // ends in `\n` does not gain a blank line. An EMPTY template emits zero
  // bytes rather than a bare newline — zig guards on `raw.len > 0`.
  if (!out.empty() && out.back() != '\n') {
    out += '\n';
  }
  return out;
}

auto init_text(std::span<const std::string> created) -> std::string {
  if (created.empty()) {
    return "templates init: nothing to do (all templates already present)\n";
  }
  std::string out = std::format("templates init: wrote {} file(s)\n", created.size());
  for (auto const& path : created) {
    out += std::format("  {}\n", path);
  }
  return out;
}

auto init_json(std::span<const std::string> created) -> std::string {
  std::string out;
  for (auto const& path : created) {
    out += R"({"path":)";
    append_json_string(out, path);
    out += "}\n";
  }
  return out;
}

auto validate_ok(std::string_view set_name, std::string_view system, std::string_view kind, bool json) -> std::string {
  if (!json) {
    return std::format("ok: {}/{}/{}\n", set_name, system, kind);
  }
  // The three identifiers go through `append_json_string`, same as the
  // `issues` array below (task 6213). They used to be interpolated RAW,
  // because zig's handler built this envelope with `{s}` inside a literal
  // rather than through `encodeJsonString` -- so a set name carrying a
  // double quote emitted a document no parser reads:
  //
  //   {"ok":true,"set":"ev"il","system":"jira","kind":"epic","issues":[]}
  //
  // A set name is a DIRECTORY NAME under `~/.planar/templates/`, so every
  // byte POSIX allows in a path reaches here. Reproducing that was correct
  // while the oracle existed to be diffed against; decision 1067 retired
  // that rule for divergences with real consequences, and emitting invalid
  // JSON from the one flag whose entire contract is "this parses" is one.
  std::string out{R"({"ok":true,"set":)"};
  append_json_string(out, set_name);
  out += R"(,"system":)";
  append_json_string(out, system);
  out += R"(,"kind":)";
  append_json_string(out, kind);
  out += R"(,"issues":[]})"
         "\n";
  return out;
}

auto validate_issues(std::string_view set_name, std::string_view system, std::string_view kind, std::string_view path,
                     std::span<const validation_issue> issues, bool json) -> std::string {
  if (!json) {
    std::string out;
    for (auto const& issue : issues) {
      // Every line leads with the RESOLVED path, repeated per issue.
      out += std::format("ISSUE: {} [{}]: {}\n", path, issue.json_path, issue.message);
    }
    return out;
  }

  // Same three identifiers, same escaping (task 6213). The `issues` array
  // below was always escaped, which is exactly what made the header's
  // asymmetry easy to miss.
  std::string out{R"({"ok":false,"set":)"};
  append_json_string(out, set_name);
  out += R"(,"system":)";
  append_json_string(out, system);
  out += R"(,"kind":)";
  append_json_string(out, kind);
  out += R"(,"issues":[)";
  bool first = true;
  for (auto const& issue : issues) {
    if (!first) {
      out += ',';
    }
    first = false;
    out += R"({"json_path":)";
    append_json_string(out, issue.json_path);
    out += R"(,"message":)";
    append_json_string(out, issue.message);
    out += '}';
  }
  out += "]}\n";
  return out;
}

auto validate_summary(std::size_t count, std::string_view set_name, std::string_view system, std::string_view kind)
    -> std::string {
  return std::format("{} issue(s) in {}/{}/{}", count, set_name, system, kind);
}

auto not_found_message(std::string_view set_name, std::string_view system, std::string_view kind) -> std::string {
  return std::format("template {}/{}/{} not found", set_name, system, kind);
}

} // namespace planar::engine::templates
