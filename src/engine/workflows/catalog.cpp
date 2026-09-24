/// @file catalog.cpp
/// @brief Implementation of `planar.engine.workflows.catalog` (plan 996, task
/// 6096). See catalog.cppm for scope, the oracle-derived parsing quirks, and
/// the cut list.

module planar.engine.workflows.catalog;

import std;

namespace planar::engine::workflows::catalog {

namespace {

constexpr std::string_view k_meta_open  = "--[[ @meta";
constexpr std::string_view k_meta_close = "--]]";
constexpr std::string_view k_trim_chars = " \t\r";

/// @brief Trim leading and trailing spaces, tabs and CRs.
///
/// The `\r` is why CRLF workflow files parse identically to LF ones without
/// any newline normalization pass.
auto trim(std::string_view text, std::string_view chars = k_trim_chars) -> std::string_view {
  const auto first = text.find_first_not_of(chars);
  if (first == std::string_view::npos) {
    return {};
  }
  const auto last = text.find_last_not_of(chars);
  return text.substr(first, last - first + 1);
}

/// @brief Split on '\n', SKIPPING empty segments.
///
/// This mirrors zig's `std.mem.tokenizeScalar` rather than `splitScalar`, and
/// the difference is observable: because empty segments are dropped, a file
/// whose first two lines are blank still presents `--[[ @meta` as its FIRST
/// token, and the opener is accepted. Verified against the oracle (fixture
/// `lead.lua` -> name `leading-blanks`). A `splitScalar` equivalent would
/// reject that file, silently losing its metadata.
auto tokenize_lines(std::string_view content) -> std::vector<std::string_view> {
  std::vector<std::string_view> out;
  std::size_t                   pos = 0;
  while (pos < content.size()) {
    const auto next = content.find('\n', pos);
    const auto end  = next == std::string_view::npos ? content.size() : next;
    if (end > pos) {
      out.push_back(content.substr(pos, end - pos));
    }
    if (next == std::string_view::npos) {
      break;
    }
    pos = next + 1;
  }
  return out;
}

auto name_less(const entry& lhs, const entry& rhs) -> bool {
  return effective_name(lhs).compare(effective_name(rhs)) < 0;
}

} // namespace

auto parse_meta(std::string_view content) -> parse_result {
  const auto lines = tokenize_lines(content);
  if (lines.empty()) {
    return {};
  }
  if (trim(lines.front()) != k_meta_open) {
    return {};
  }

  parse_result out;
  out.found = true;
  for (std::size_t i = 1; i < lines.size(); ++i) {
    const auto line = trim(lines[i]);
    if (line == k_meta_close) {
      break;
    }
    // `tokenize_lines` already dropped genuinely empty segments; a
    // whitespace-only line still reaches here and trims to empty.
    if (line.empty()) {
      continue;
    }
    const auto colon = line.find(':');
    if (colon == std::string_view::npos) {
      continue;
    }
    const auto key   = trim(line.substr(0, colon), " \t");
    const auto value = trim(line.substr(colon + 1), " \t");
    if (key == "name") {
      out.meta.name = value;
    } else if (key == "description") {
      out.meta.description = value;
    } else if (key == "phases") {
      out.meta.phases = value;
    } else if (key == "seam") {
      out.meta.seam = value;
    }
    // Unknown keys are silently skipped: a workflow may carry annotations
    // this version does not read, and refusing the file over one would make
    // the catalog brittle against its own future.
  }
  // NOTE there is deliberately no "unterminated block" error path. A block
  // never closed by `--]]` parses to EOF and reports `found = true` --
  // oracle-verified (fixture `noclose.lua` -> name `no-close`).
  return out;
}

auto effective_name(const entry& value) -> std::string_view {
  if (value.meta_found && !value.meta.name.empty()) {
    return value.meta.name;
  }
  std::string_view filename{value.filename};
  if (filename.ends_with(".lua")) {
    return filename.substr(0, filename.size() - 4);
  }
  return filename;
}

auto scan(const std::filesystem::path& dir_path, bool is_local) -> std::vector<entry> {
  std::vector<entry> out;

  std::error_code ec;
  if (!std::filesystem::is_directory(dir_path, ec) || ec) {
    // Absent or inaccessible directory is an EMPTY list, not an error.
    return out;
  }
  std::filesystem::directory_iterator it(dir_path, ec);
  if (ec) {
    return out;
  }

  for (const auto& item : it) {
    std::error_code entry_ec;
    if (!item.is_regular_file(entry_ec) || entry_ec) {
      continue;
    }
    const auto filename = item.path().filename().string();
    if (!filename.ends_with(".lua")) {
      continue;
    }

    entry value{
        .path       = item.path().string(),
        .filename   = filename,
        .meta       = {},
        .meta_found = false,
        .is_local   = is_local,
    };

    std::ifstream file(item.path(), std::ios::binary);
    if (file) {
      // The Zig original caps a read at 256 KiB; a workflow's @meta block
      // lives at the very top, so reading the whole file is only ever needed
      // for small files in practice. The cap is preserved so a pathological
      // file cannot be slurped whole.
      constexpr std::streamsize k_read_limit = 256 * 1024;
      std::string               content(static_cast<std::size_t>(k_read_limit), '\0');
      file.read(content.data(), k_read_limit);
      content.resize(static_cast<std::size_t>(file.gcount()));

      auto parsed      = parse_meta(content);
      value.meta       = std::move(parsed.meta);
      value.meta_found = parsed.found;
    }
    // An unreadable file still yields an entry with no meta: the workflow
    // exists even when its metadata cannot be read, and omitting it would be
    // a worse answer than showing it bare.

    out.push_back(std::move(value));
  }

  std::ranges::sort(out, name_less);
  return out;
}

auto resolve_planar_home(const std::function<std::optional<std::string>(std::string_view)>& env_lookup) -> std::filesystem::path {
  if (const auto explicit_home = env_lookup("PLANAR_HOME"); explicit_home.has_value()) {
    return std::filesystem::path(*explicit_home);
  }
  const auto home = env_lookup("HOME");
  // The `/tmp` fallback is the Zig original's. It means a process with no
  // home at all still resolves to an absolute, harmless location rather than
  // to a relative path that would land wherever the cwd happens to be.
  return std::filesystem::path(home.value_or(std::string{"/tmp"})) / ".planar";
}

auto resolve_dirs(const std::function<std::optional<std::string>(std::string_view)>& env_lookup) -> directories {
  const auto planar_home = resolve_planar_home(env_lookup);

  // `PLANAR_WORKFLOWS_DIR` overrides ONLY the shipped directory; the sandbox
  // stays under PLANAR_HOME either way.
  const auto shipped_override = env_lookup("PLANAR_WORKFLOWS_DIR");
  return directories{
      .shipped = shipped_override.has_value() ? std::filesystem::path(*shipped_override) : planar_home / "workflows",
      .sandbox = planar_home / "local" / "workflows",
  };
}

auto list(const directories& dirs, bool local_only) -> std::vector<entry> {
  std::vector<entry> out;
  if (!local_only) {
    out = scan(dirs.shipped, false);
  }
  // Concatenated, NOT merge-sorted: each directory is sorted on its own and
  // the sandbox block follows the shipped block wholesale. Oracle-verified --
  // a sandbox `aaa-local` lists AFTER shipped `finalize-closeout`.
  auto sandbox = scan(dirs.sandbox, true);
  out.insert(out.end(), std::make_move_iterator(sandbox.begin()), std::make_move_iterator(sandbox.end()));
  return out;
}

auto find(const directories& dirs, std::string_view name) -> std::optional<entry> {
  // Shipped first: a name present in BOTH resolves to the shipped copy.
  for (const auto& [dir_path, is_local] :
       std::array<std::pair<std::filesystem::path, bool>, 2>{{{dirs.shipped, false}, {dirs.sandbox, true}}}) {
    for (auto& value : scan(dir_path, is_local)) {
      if (effective_name(value) == name) {
        return value;
      }
    }
  }
  return std::nullopt;
}

} // namespace planar::engine::workflows::catalog
