/// @file feature.cpp
/// @brief Implementation of `planar.engine.workbench.feature` (plan 996, task
/// 6037). See feature.cppm for the oracle-captured layout.

module planar.engine.workbench.feature;

import std;

namespace planar::engine::workbench::feature {

namespace {

constexpr std::size_t k_slug_max = 60;

/// @brief Join non-empty components with `/`, never doubling a separator.
///
/// Reproduces `std.fs.path.join`'s two observable behaviors this bucket
/// depends on: an EMPTY component vanishes entirely (which is how a global
/// -scope feature dir collapses the association level away), and a
/// component that already ends in `/` does not produce `//`.
auto join(std::initializer_list<std::string_view> parts) -> std::string {
  std::string out;
  for (auto const part : parts) {
    if (part.empty()) {
      continue;
    }
    if (!out.empty() && out.back() != '/') {
      out += '/';
    }
    out += part;
  }
  return out;
}

/// @brief Sanitize one path component: `/`, `\` and NUL become `_`; an
/// empty component or a bare `.` / `..` becomes `_`.
auto safe_path_segment(std::string_view raw) -> std::string {
  if (raw.empty() || raw == "." || raw == "..") {
    return "_";
  }
  std::string out{raw};
  for (char& ch : out) {
    if (ch == '/' || ch == '\\' || ch == '\0') {
      ch = '_';
    }
  }
  return out;
}

/// @brief Sanitize an association slug. Same as `safe_path_segment` except
/// that `:` is ALSO rewritten (association slugs are `kind:name`), and an
/// empty slug stays empty rather than becoming `_` — an empty association
/// must collapse the directory level, not create one called `_`.
auto safe_assoc_slug(std::string_view raw) -> std::string {
  std::string out{raw};
  for (char& ch : out) {
    if (ch == ':' || ch == '/' || ch == '\\' || ch == '\0') {
      ch = '_';
    }
  }
  if (out == "." || out == "..") {
    out.assign(out.size(), '_');
  }
  return out;
}

/// @brief `<safe-key>-<safe-slug>`, the feature directory's own name.
auto feature_name(std::string_view plan_key, std::string_view plan_slug) -> std::string {
  return std::format("{}-{}", safe_path_segment(plan_key), safe_path_segment(plan_slug));
}

auto is_alnum(char c) -> bool {
  auto const u = static_cast<unsigned char>(c);
  return (u >= '0' && u <= '9') || (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z');
}

auto to_lower(char c) -> char {
  auto const u = static_cast<unsigned char>(c);
  return (u >= 'A' && u <= 'Z') ? static_cast<char>(u - 'A' + 'a') : c;
}

auto trim_trailing_dashes(std::string& text) -> void {
  while (!text.empty() && text.back() == '-') {
    text.pop_back();
  }
}

auto replace_char(std::string_view text, char from, char to) -> std::string {
  std::string out{text};
  for (char& ch : out) {
    if (ch == from) {
      ch = to;
    }
  }
  return out;
}

} // namespace

auto slugify(std::string_view title) -> std::string {
  if (title.empty()) {
    return "untitled";
  }
  std::string out;
  out.reserve(title.size());
  bool in_separator = true; // suppresses a leading '-'
  for (char const c : title) {
    // Byte-wise, and deliberately so: every byte of a multi-byte UTF-8
    // sequence is non-alphanumeric here, so an accented word collapses to
    // its ASCII letters separated by `-` (`Héllo` -> `h-llo`). That is what
    // the oracle writes; normalizing Unicode would change filenames.
    if (is_alnum(c)) {
      out += to_lower(c);
      in_separator = false;
      continue;
    }
    if (!in_separator) {
      out += '-';
      in_separator = true;
    }
  }
  trim_trailing_dashes(out);
  if (out.size() > k_slug_max) {
    out.resize(k_slug_max);
    // Second trim: the truncation point may have landed on a separator,
    // and a filename ending in `-` is not what the oracle produces.
    trim_trailing_dashes(out);
  }
  if (out.empty()) {
    return "untitled";
  }
  return out;
}

auto feature_dir(std::string_view root, std::string_view assoc_slug, std::string_view plan_key, std::string_view plan_slug)
    -> std::string {
  auto const assoc = safe_assoc_slug(assoc_slug);
  auto const name  = feature_name(plan_key, plan_slug);
  return join({root, assoc, name});
}

auto stored_path(std::string_view assoc_slug, std::string_view plan_key, std::string_view plan_slug, std::string_view rel_path)
    -> std::string {
  auto const assoc = safe_assoc_slug(assoc_slug);
  auto const name  = feature_name(plan_key, plan_slug);
  return join({assoc, name, rel_path});
}

auto artifact_filename(std::int64_t id, std::string_view title, std::string_view kind) -> std::string {
  auto slug = slugify(title);
  if (slug.empty() || slug == "untitled") {
    slug = replace_char(kind, '_', '-');
  }
  return std::format("{}-{}.md", id, slug);
}

auto canonical_entity_kind(std::string_view kind) -> std::string_view {
  if (kind == "test_scenario") {
    return "scenario";
  }
  return kind;
}

} // namespace planar::engine::workbench::feature
