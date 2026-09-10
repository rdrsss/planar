/// @file toml.cpp
/// @brief Implementation of `planar.engine.config.toml` (see toml.cppm).

module;

#include <glaze/json/generic.hpp>
#include <glaze/toml.hpp>
#include <glaze/util/validate.hpp>

module planar.engine.config.toml;

import std;

namespace planar::engine::config {

namespace {

/// @brief Trim ASCII spaces, tabs and CR from both ends of `s`.
auto trim(std::string_view s) -> std::string_view {
  const auto first = s.find_first_not_of(" \t\r");
  if (first == std::string_view::npos) {
    return {};
  }
  return s.substr(first, s.find_last_not_of(" \t\r") - first + 1);
}

/// @brief Per-line lexical state carried across the section scanner, so a
/// `[` that merely opens a nested array element — or that sits inside a
/// multi-line string — is never mistaken for a table header.
struct scan_state {
  /// @brief The multi-line string delimiter currently open (`"""` or
  /// `'''`), or empty when not inside one.
  std::string_view multiline_delim;
  /// @brief Unclosed `[` count from a value that spans lines.
  int bracket_depth = 0;
};

/// @brief Advance `st` across one whole source line, ignoring comments and
/// string contents. Only called for lines the scanner has already decided
/// are NOT table headers.
void advance_scan(std::string_view line, scan_state& st) {
  std::size_t i = 0;

  // Finish an open multi-line string first: everything up to its closing
  // delimiter is inert.
  if (!st.multiline_delim.empty()) {
    const auto close = line.find(st.multiline_delim);
    if (close == std::string_view::npos) {
      return;
    }
    i                  = close + st.multiline_delim.size();
    st.multiline_delim = {};
  }

  while (i < line.size()) {
    const char c = line[i];
    if (c == '#') {
      return; // Rest of the line is a comment.
    }
    if (c == '"' || c == '\'') {
      const std::string_view triple = (c == '"') ? std::string_view{"\"\"\""} : std::string_view{"'''"};
      if (line.substr(i).starts_with(triple)) {
        const auto close = line.find(triple, i + triple.size());
        if (close == std::string_view::npos) {
          st.multiline_delim = triple;
          return;
        }
        i = close + triple.size();
        continue;
      }
      // Single-line string: skip to its unescaped closing quote. A literal
      // (single-quoted) string has no escapes.
      ++i;
      while (i < line.size()) {
        if (c == '"' && line[i] == '\\' && i + 1 < line.size()) {
          i += 2;
          continue;
        }
        if (line[i] == c) {
          break;
        }
        ++i;
      }
      ++i;
      continue;
    }
    if (c == '[') {
      ++st.bracket_depth;
    } else if (c == ']') {
      st.bracket_depth = std::max(0, st.bracket_depth - 1);
    }
    ++i;
  }
}

/// @brief If `line` is a table header at top level, return its raw inner
/// text (everything between the outermost brackets). Array-of-tables
/// (`[[x]]`) is deliberately NOT a header — both trees reject it, so it is
/// left in place for Glaze to fail on.
auto header_body(std::string_view line) -> std::optional<std::string_view> {
  const auto t = trim(line);
  if (t.size() < 2 || t.front() != '[' || t.starts_with("[[")) {
    return std::nullopt;
  }
  // Find the closing ']' outside any quoted segment.
  bool        in_basic = false;
  bool        in_lit   = false;
  std::size_t i        = 1;
  for (; i < t.size(); ++i) {
    const char c = t[i];
    if (in_basic) {
      if (c == '\\') {
        ++i;
      } else if (c == '"') {
        in_basic = false;
      }
      continue;
    }
    if (in_lit) {
      if (c == '\'') {
        in_lit = false;
      }
      continue;
    }
    if (c == '"') {
      in_basic = true;
    } else if (c == '\'') {
      in_lit = true;
    } else if (c == ']') {
      break;
    }
  }
  if (i >= t.size()) {
    return std::nullopt; // Unterminated header — let Glaze diagnose it.
  }
  const auto rest = trim(t.substr(i + 1));
  if (!rest.empty() && rest.front() != '#') {
    return std::nullopt; // Trailing junk — let Glaze diagnose it.
  }
  return t.substr(1, i - 1);
}

/// @brief Count the top-level `.` separators in a header body, i.e. the
/// header's depth minus one. Quoted segments may contain dots that are not
/// separators (`[external."a.b"]`).
auto header_depth(std::string_view body) -> std::size_t {
  std::size_t depth    = 1;
  bool        in_basic = false;
  bool        in_lit   = false;
  for (std::size_t i = 0; i < body.size(); ++i) {
    const char c = body[i];
    if (in_basic) {
      if (c == '\\') {
        ++i;
      } else if (c == '"') {
        in_basic = false;
      }
      continue;
    }
    if (in_lit) {
      if (c == '\'') {
        in_lit = false;
      }
      continue;
    }
    if (c == '"') {
      in_basic = true;
    } else if (c == '\'') {
      in_lit = true;
    } else if (c == '.') {
      ++depth;
    }
  }
  return depth;
}

/// @brief One `[header]` and the verbatim body lines under it, each paired
/// with the 1-based source line it came from.
struct section {
  std::string   header;                                    ///< The header line as written, empty for the pre-header root section.
  std::uint32_t header_line = 0;                           ///< 1-based source line of `header`.
  std::string   key;                                       ///< Normalized grouping key (the raw header body), empty for root.
  std::size_t   depth = 0;                                 ///< Header depth; 0 for root.
  std::vector<std::pair<std::string, std::uint32_t>> body; ///< Body lines + their 1-based source lines.
};

} // namespace

auto normalize_sections(std::string_view content) -> normalized_document {
  std::vector<section> sections;
  sections.push_back(section{}); // Root section (keys before any header).

  scan_state    st;
  std::uint32_t lineno = 0;
  for (const auto part : std::views::split(content, '\n')) {
    ++lineno;
    const std::string_view line{part.begin(), part.end()};
    if (st.multiline_delim.empty() && st.bracket_depth == 0) {
      if (auto body = header_body(line)) {
        sections.push_back(section{
            .header      = std::string(line),
            .header_line = lineno,
            .key         = std::string(trim(*body)),
            .depth       = header_depth(trim(*body)),
            .body        = {},
        });
        continue;
      }
    }
    advance_scan(line, st);
    sections.back().body.emplace_back(std::string(line), lineno);
  }

  // Merge sections sharing a header key, keeping first-appearance order,
  // then emit shallowest-first so Glaze never sees a header whose path a
  // deeper header already materialized (toml/read.hpp:2010-2012).
  std::vector<section> merged;
  for (auto& s : sections) {
    auto it = std::ranges::find_if(merged, [&](const section& m) { return m.depth != 0 && m.key == s.key; });
    if (s.depth != 0 && it != merged.end()) {
      it->body.insert(it->body.end(), s.body.begin(), s.body.end());
      continue;
    }
    merged.push_back(std::move(s));
  }
  std::ranges::stable_sort(merged, {}, &section::depth);

  normalized_document out;
  const auto          emit = [&](const std::string& text, std::uint32_t origin) {
    out.text.append(text);
    out.text.push_back('\n');
    out.line_origin.push_back(origin);
  };
  for (const auto& s : merged) {
    if (s.depth != 0) {
      emit(s.header, s.header_line);
    }
    for (const auto& [text, origin] : s.body) {
      emit(text, origin);
    }
  }
  return out;
}

namespace {

/// @brief Best-effort source location for a semantic rejection raised AFTER
/// Glaze has already parsed the document, where no byte offset survives.
/// Finds the LAST `<leaf> =` assignment in `content` (last-wins, matching
/// TOML's own override order) and points at the first character of its
/// value — the same character `parse.zig` reports for `x = 1.5` (line 1,
/// col 5 there is the `1`; zig's own float diagnostic lands one further in,
/// on the `.`, which is why toml_error documents "equivalent", not "identical").
/// @param content The ORIGINAL document (not the normalized rewrite).
/// @param dotted_key The full flattened key whose value was rejected.
/// @return The 1-based line/column pair, or {0, 0} when not locatable.
auto locate_assignment(std::string_view content, std::string_view dotted_key) -> std::pair<std::uint32_t, std::uint32_t> {
  const auto dot  = dotted_key.rfind('.');
  const auto leaf = dot == std::string_view::npos ? dotted_key : dotted_key.substr(dot + 1);
  if (leaf.empty()) {
    return {0, 0};
  }

  std::pair<std::uint32_t, std::uint32_t> found{0, 0};
  std::uint32_t                           lineno = 0;
  for (const auto part : std::views::split(content, '\n')) {
    ++lineno;
    const std::string_view line{part.begin(), part.end()};
    const auto             lead = line.find_first_not_of(" \t");
    if (lead == std::string_view::npos) {
      continue;
    }
    auto rest = line.substr(lead);
    // Accept both the bare key and a quoted key.
    if (rest.starts_with('"') || rest.starts_with('\'')) {
      const char q = rest.front();
      if (rest.size() > leaf.size() + 1 && rest.substr(1).starts_with(leaf) && rest[leaf.size() + 1] == q) {
        rest = rest.substr(leaf.size() + 2);
      } else {
        continue;
      }
    } else if (rest.starts_with(leaf)) {
      rest = rest.substr(leaf.size());
    } else {
      continue;
    }
    const auto eq = rest.find_first_not_of(" \t");
    if (eq == std::string_view::npos || rest[eq] != '=') {
      continue;
    }
    const auto val = rest.find_first_not_of(" \t", eq + 1);
    if (val == std::string_view::npos) {
      continue;
    }
    const auto col = (line.size() - rest.size()) + val + 1;
    found          = {lineno, static_cast<std::uint32_t>(col)};
  }
  return found;
}

/// @brief Recursively flatten a parsed `glz::generic_i64` node into `out`,
/// joining nested object keys with '.'. `prefix` is the dotted path
/// accumulated so far ("" at the document root). `source` is the ORIGINAL
/// document, used only to locate a rejection.
auto flatten(const glz::generic_i64& node, const std::string& prefix, std::string_view source, toml_map& out)
    -> std::expected<void, toml_error> {
  const auto reject = [&](std::string message) -> std::unexpected<toml_error> {
    const auto [line, column] = locate_assignment(source, prefix);
    return std::unexpected(toml_error{.line = line, .column = column, .message = std::move(message)});
  };

  if (node.is_object()) {
    for (const auto& [key, child] : node.get_object()) {
      const std::string full_key = prefix.empty() ? key : std::format("{}.{}", prefix, key);
      if (auto flattened = flatten(child, full_key, source, out); !flattened) {
        return flattened;
      }
    }
    return {};
  }

  if (node.is_string()) {
    out[prefix] = toml_value{.kind_ = toml_value::kind::string, .string_ = node.get_string()};
    return {};
  }
  if (node.is_boolean()) {
    out[prefix] = toml_value{.kind_ = toml_value::kind::boolean, .bool_ = node.get_boolean()};
    return {};
  }
  if (node.is_int64()) {
    out[prefix] = toml_value{.kind_ = toml_value::kind::integer, .int_ = node.get<std::int64_t>()};
    return {};
  }
  if (node.is_array()) {
    std::vector<std::string> items;
    items.reserve(node.get_array().size());
    for (const auto& elem : node.get_array()) {
      if (!elem.is_string()) {
        // Only string arrays are supported (mirrors zig's parse.zig
        // restriction, whose own message this reuses) — the only
        // array-typed default key
        // (external.github-projects.parent_field_names) is a string
        // array; anything else is out of this config schema's shape.
        return reject("only string arrays are supported");
      }
      items.push_back(elem.get_string());
    }
    out[prefix] = toml_value{.kind_ = toml_value::kind::array, .array_ = std::move(items)};
    return {};
  }

  // Float (is_double() without is_int64()) and null have no counterpart in
  // Planar's config schema — mirrors zig's parse.zig, which rejects both
  // (float values are not supported; TOML has no native null).
  return reject("float values are not supported");
}

} // namespace

auto parse_toml(std::string_view content) -> std::expected<toml_map, toml_error> {
  // Glaze's own top-level `read<Opts>` guard rejects a zero-length buffer
  // outright (`error_code::no_read_input`) for EVERY target type, TOML
  // included — short-circuit before ever calling into Glaze so "no file
  // content" and "an empty file" both mean "contributes nothing", matching
  // zig's parse.zig ("empty file returns empty map").
  if (content.empty()) {
    return toml_map{};
  }

  // Rewrite table headers first — see normalize_sections and toml.cppm's
  // header comment (task 6082): Glaze's `ensure_map_path` refuses a header
  // whose path a DEEPER header already materialized, which makes the legal
  // (and Zig-accepted) `[external.jira.status]` … `[external.jira]`
  // ordering fail the entire config load.
  const auto doc = normalize_sections(content);

  // Read into a `std::map<std::string, generic_i64>` — NOT a bare
  // `glz::generic_i64` — for the document root. This is not a stylistic
  // choice: `glz::generic_i64`'s own `glz::meta` unwraps it to its
  // internal `data` variant, so a bare top-level `glz::read_toml(generic_i64&,
  // ...)` dispatches through Glaze's VARIANT reader (toml/read.hpp's
  // `from<TOML, T> requires is_variant<T>`), which decides what a
  // document IS purely from its first non-whitespace/non-comment
  // character. Every real Planar config file's first significant
  // character is `[` (a table header — `defaults.toml` opens with
  // `[defaults]`), and the variant reader's `case '['` branch
  // unconditionally treats a leading `[` as "the whole document is a TOML
  // ARRAY literal" (toml/read.hpp:2692-2707) — it has no lookahead to
  // distinguish `[defaults]\nvendor = "x"` (a table header) from
  // `[1, 2, 3]` (an array value). A bare `glz::generic_i64` therefore
  // silently misparses (or, here, error_code::syntax_errors on) any
  // document starting with a section header. Reading into a genuine
  // `readable_map_t` (a `std::map`) instead routes through the DIFFERENT
  // `from<TOML, T> requires readable_map_t<T>` specialization
  // (toml/read.hpp:2202), which correctly implements table-header/
  // section-path handling for its whole `while` loop. Verified against
  // both code paths with a standalone probe before writing this
  // workaround, and pinned by toml.t.cpp's
  // "generic_i64 at the document root still mis-dispatches" test so a
  // Glaze bump that fixes it is noticed. This is a real, narrow gap in
  // Glaze's generic-TOML reading (not a wholesale invalidation of D12:
  // Glaze's TYPED-struct TOML reading, which `docs/toml.md`'s own examples
  // use, has no such problem — only the schema-less
  // `glz::generic`/`generic_i64` path does, and only when read directly at
  // top level).
  std::map<std::string, glz::generic_i64, std::less<>> root;
  if (const auto ec = glz::read_toml(root, doc.text)) {
    // Map Glaze's byte offset into the NORMALIZED text back onto the
    // operator's own line numbering. Section bodies are copied verbatim by
    // normalize_sections, so the column carries over untouched.
    const auto    info = glz::detail::get_source_info(doc.text, ec.count);
    std::uint32_t line = 0;
    if (info.line >= 1 && info.line <= doc.line_origin.size()) {
      line = doc.line_origin[info.line - 1];
    }
    return std::unexpected(toml_error{
        .line    = line,
        .column  = line == 0 ? 0 : static_cast<std::uint32_t>(info.column),
        .message = glz::format_error(ec),
    });
  }

  toml_map out;
  for (const auto& [key, child] : root) {
    if (auto flattened = flatten(child, key, content, out); !flattened) {
      return std::unexpected(flattened.error());
    }
  }
  return out;
}

} // namespace planar::engine::config
