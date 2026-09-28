/// @file root.cpp
/// @brief Implementation of `planar.engine.workbench.root` (plan 996, task
/// 6037). See root.cppm for the precedence chain and the safety argument.

module planar.engine.workbench.root;

import std;

namespace planar::engine::workbench::root {

namespace {

auto trim(std::string_view text) -> std::string_view {
  auto const first = text.find_first_not_of(" \t\r");
  if (first == std::string_view::npos) {
    return {};
  }
  auto const last = text.find_last_not_of(" \t\r");
  return text.substr(first, last - first + 1);
}

/// @brief Strip one layer of matching quotes, or return unset for an
/// unquoted scalar (this scan accepts string values only).
auto unquote(std::string_view text) -> std::optional<std::string> {
  if (text.size() < 2) {
    return std::nullopt;
  }
  bool const single = text.front() == '\'' && text.back() == '\'';
  bool const dbl    = text.front() == '"' && text.back() == '"';
  if (!single && !dbl) {
    return std::nullopt;
  }
  return std::string{text.substr(1, text.size() - 2)};
}

/// @brief Strip a trailing `#` comment, honouring quotes.
///
/// A `#` inside a quoted value is data, not a comment — `root = "a#b"` must
/// survive. Anything outside quotes from the first `#` onward is dropped.
auto strip_comment(std::string_view line) -> std::string_view {
  char in_quote = 0;
  for (std::size_t i = 0; i < line.size(); ++i) {
    char const c = line[i];
    if (in_quote != 0) {
      if (c == in_quote) {
        in_quote = 0;
      }
      continue;
    }
    if (c == '\'' || c == '"') {
      in_quote = c;
      continue;
    }
    if (c == '#') {
      return line.substr(0, i);
    }
  }
  return line;
}

/// @brief Normalize a table-header segment: `"github-issues"` -> `github-issues`.
auto unquote_segment(std::string_view segment) -> std::string {
  if (auto const inner = unquote(segment)) {
    return *inner;
  }
  return std::string{segment};
}

/// @brief Flatten a `[a.b."c"]` header into `a.b.c`.
auto flatten_header(std::string_view header) -> std::string {
  std::string out;
  std::size_t start    = 0;
  char        in_quote = 0;
  for (std::size_t i = 0; i <= header.size(); ++i) {
    bool const at_end = i == header.size();
    char const c      = at_end ? '.' : header[i];
    if (!at_end && in_quote != 0) {
      if (c == in_quote) {
        in_quote = 0;
      }
      continue;
    }
    if (!at_end && (c == '\'' || c == '"')) {
      in_quote = c;
      continue;
    }
    if (c != '.') {
      continue;
    }
    if (!out.empty()) {
      out += '.';
    }
    out += unquote_segment(trim(header.substr(start, i - start)));
    start = i + 1;
  }
  return out;
}

} // namespace

auto read_config_workbench_root(std::string_view content) -> std::optional<std::string> {
  std::string section;
  std::size_t start = 0;
  while (start <= content.size()) {
    auto const nl      = content.find('\n', start);
    auto const raw     = content.substr(start, nl == std::string_view::npos ? std::string_view::npos : nl - start);
    auto const line    = trim(strip_comment(raw));
    bool const is_last = nl == std::string_view::npos;
    start              = is_last ? content.size() + 1 : nl + 1;

    if (line.empty()) {
      if (is_last) {
        break;
      }
      continue;
    }
    if (line.front() == '[') {
      // `[[array-of-tables]]` is unsupported (as it is in zig's parser);
      // treating it as a plain header is harmless because it can never
      // spell `workbench`.
      auto const close = line.rfind(']');
      if (close != std::string_view::npos && close > 0) {
        section = flatten_header(trim(line.substr(1, close - 1)));
      }
      if (is_last) {
        break;
      }
      continue;
    }
    auto const eq = line.find('=');
    if (eq != std::string_view::npos) {
      auto const key   = flatten_header(trim(line.substr(0, eq)));
      auto const value = trim(line.substr(eq + 1));
      auto const full  = section.empty() ? key : std::format("{}.{}", section, key);
      if (full == "workbench.root") {
        if (auto text = unquote(value)) {
          return text;
        }
        return std::nullopt;
      }
    }
    if (is_last) {
      break;
    }
  }
  return std::nullopt;
}

auto expand_tilde(std::string_view path, const env_lookup& env) -> std::expected<std::string, root_error> {
  if (path == "~") {
    auto const home = env("HOME");
    if (!home || home->empty()) {
      return std::unexpected(root_error::unresolved);
    }
    return *home;
  }
  if (path.starts_with("~/")) {
    auto const home = env("HOME");
    if (!home || home->empty()) {
      return std::unexpected(root_error::unresolved);
    }
    return (std::filesystem::path{*home} / path.substr(2)).string();
  }
  return std::string{path};
}

auto resolve_root(const env_lookup& env, const std::function<std::optional<std::string>(const std::filesystem::path&)>& read_file)
    -> std::expected<std::string, root_error> {
  // Layer 1 — the env var. Set-but-empty falls through, which is why the
  // lookup must distinguish empty from absent.
  if (auto const raw = env("PLANAR_WORKBENCH_ROOT"); raw && !raw->empty()) {
    return expand_tilde(*raw, env);
  }

  // Layer 2 — `workbench.root` from the config file. An absent, unreadable
  // or unparseable file is SILENTLY skipped: a broken config must not make
  // the workbench unreachable.
  std::optional<std::filesystem::path> config_path;
  if (auto const raw = env("PLANAR_CONFIG_PATH"); raw && !raw->empty()) {
    auto const expanded = expand_tilde(*raw, env);
    if (expanded) {
      config_path = std::filesystem::path{*expanded};
    }
  } else if (auto const home = env("HOME"); home && !home->empty()) {
    config_path = std::filesystem::path{*home} / ".planar" / "config.toml";
  }

  if (config_path) {
    if (auto const content = read_file(*config_path)) {
      if (auto const value = read_config_workbench_root(*content); value && !value->empty()) {
        return expand_tilde(*value, env);
      }
    }
  }

  // Layer 3 — the built-in default.
  if (auto const home = env("HOME"); home && !home->empty()) {
    return (std::filesystem::path{*home} / ".planar" / "workbench").string();
  }

  return std::unexpected(root_error::unresolved);
}

auto resolve_root(const env_lookup& env) -> std::expected<std::string, root_error> {
  return resolve_root(env, [](const std::filesystem::path& path) -> std::optional<std::string> {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
      return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
  });
}

} // namespace planar::engine::workbench::root
