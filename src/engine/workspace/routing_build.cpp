/// @file routing_build.cpp
/// @brief The WRITE half of `planar.engine.workspace.routing` (plan 996,
/// task 6275) — capability detection, cross-repo dependency inference, entry
/// points, the language census, the operator-override merge, and the JSON
/// encoder.
///
/// Port target: the builder half of zig/src/engine/workspace/routing.zig.
/// The decoder and the two `show` renderers live beside this in routing.cpp.
///
/// Read routing.cppm's header first: it carries the two deliberate
/// divergences (the `languages` key order, and the slash-bearing capability
/// pattern that never matches) with the captures behind each.
///
/// ## Two functions in the original are DEAD and are not ported
///
/// `globExists` and `pathExistsJoin` are defined in routing.zig and called
/// from nowhere in it — confirmed by grepping the whole Zig tree, not just
/// the file. They are omitted rather than transcribed, because a port that
/// carries unreachable code forward makes the next reader hunt for the
/// caller that does not exist.
///
/// ## Symlinks are skipped, and that follows from `entry.kind`
///
/// Zig's directory iterator reports a symlink as `.sym_link`, which is
/// neither `.file` nor `.directory`, so every walk here — the language
/// census, the entry-point scan, the `package main` search — silently drops
/// them. `std::filesystem::directory_entry::is_directory()` FOLLOWS
/// symlinks, so a naive port would descend into them and diverge. Every
/// walk below therefore tests `symlink_status()` first. The one exception is
/// `pattern_finds`, which matches an entry's NAME with no kind test at all
/// and so matches directories and symlinks alike — that is the original's
/// behaviour and is preserved.

module planar.engine.workspace.routing;

import std;
import planar.db;
import planar.json_dom;

namespace planar::engine::workspace::routing {

namespace {

// ===========================================================================
// Filesystem primitives
// ===========================================================================

/// @brief Does `path` exist? Mirrors zig's `access`-based `pathExists`.
auto path_exists(const std::filesystem::path& path) -> bool {
  std::error_code ec;
  return std::filesystem::exists(path, ec) && !ec;
}

/// @brief One directory entry, reduced to what every walk here needs.
struct dir_entry {
  std::string name;
  bool        is_dir  = false;
  bool        is_file = false;
};

/// @brief List `dir`'s entries, or nothing when it cannot be opened.
///
/// `is_dir` / `is_file` are computed from the entry's OWN status, not the
/// followed one, so a symlink is neither — see this file's header.
auto list_dir(const std::filesystem::path& dir) -> std::vector<dir_entry> {
  std::vector<dir_entry>              out;
  std::error_code                     ec;
  std::filesystem::directory_iterator it(dir, ec);
  if (ec) {
    return out;
  }
  for (const auto& entry : it) {
    std::error_code sec;
    const auto      status = entry.symlink_status(sec);
    if (sec) {
      continue;
    }
    out.push_back(dir_entry{
        .name    = entry.path().filename().string(),
        .is_dir  = std::filesystem::is_directory(status),
        .is_file = std::filesystem::is_regular_file(status),
    });
  }
  return out;
}

/// @brief Read a whole file, or nullopt.
///
/// The original caps each read (64 KiB for a README, 512 KiB for a Go
/// source, 2 MiB for `go.mod` / `package.json`). Those caps only ever turn a
/// pathological file into a skipped one, and every caller here already
/// treats an unreadable file as absent, so they are not reproduced as
/// separate limits.
auto read_file(const std::filesystem::path& path) -> std::optional<std::string> {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return std::nullopt;
  }
  return std::string{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

/// @brief Trim `chars` from both ends.
auto trim(std::string_view text, std::string_view chars) -> std::string_view {
  const auto first = text.find_first_not_of(chars);
  if (first == std::string_view::npos) {
    return {};
  }
  const auto last = text.find_last_not_of(chars);
  return text.substr(first, last - first + 1);
}

/// @brief Split on `sep`, keeping empty fields — zig's `splitScalar`.
auto split(std::string_view text, char sep) -> std::vector<std::string_view> {
  std::vector<std::string_view> out;
  std::size_t                   start = 0;
  while (true) {
    const auto hit = text.find(sep, start);
    if (hit == std::string_view::npos) {
      out.push_back(text.substr(start));
      return out;
    }
    out.push_back(text.substr(start, hit - start));
    start = hit + 1;
  }
}

/// @brief Sort ascending, then drop adjacent duplicates.
auto sort_unique(std::vector<std::string>& values) -> void {
  std::ranges::sort(values);
  const auto stale = std::ranges::unique(values);
  values.erase(stale.begin(), stale.end());
}

// ===========================================================================
// Capability rules
// ===========================================================================

/// @brief Zig's `simpleGlobMatch`, transcribed including its quirks.
///
/// A pattern with NO `*` is an exact comparison. Otherwise it is split on
/// `*`: the first non-empty field must be a PREFIX, each later field must
/// occur in order after the previous one, and — when the pattern does not
/// END in `*` — the text after the LAST `*` must be a suffix.
///
/// The quirk worth naming: the suffix test re-derives its own tail from the
/// pattern rather than reusing the loop's last field, so `*` alone matches
/// everything (no fields are non-empty, and the pattern ends in `*` so the
/// suffix test is skipped). Oracle-captured: a rule with `match_all = ["*"]`
/// fires on any repository.
auto simple_glob_match(std::string_view pattern, std::string_view candidate) -> bool {
  if (pattern.find('*') == std::string_view::npos) {
    return pattern == candidate;
  }

  bool        first  = true;
  std::size_t cursor = 0;
  for (const auto part : split(pattern, '*')) {
    if (part.empty()) {
      first = false;
      continue;
    }
    if (first) {
      if (!candidate.starts_with(part)) {
        return false;
      }
      cursor = part.size();
      first  = false;
      continue;
    }
    const auto hit = candidate.find(part, cursor);
    if (hit == std::string_view::npos) {
      return false;
    }
    cursor = hit + part.size();
  }

  if (!pattern.empty() && pattern.back() != '*') {
    const auto last   = pattern.find_last_of('*');
    const auto suffix = pattern.substr(last == std::string_view::npos ? 0 : last + 1);
    if (!suffix.empty() && !candidate.ends_with(suffix)) {
      return false;
    }
  }
  return true;
}

/// @brief Does any entry of `root` match `pattern`?
///
/// A slash-bearing pattern scans the named subdirectory: everything before
/// the LAST `/` is a directory path relative to `root`, and the remainder is
/// globbed against that directory's entries. `proto/*.proto` therefore looks
/// in `<root>/proto` for anything ending `.proto`.
///
/// FIXED AT TASK 6322 (decision 1067's FIX set). This branch previously
/// returned false for ANY pattern containing `/`, reproducing a genuine
/// use-after-free in the oracle: `patternFinds` freed the joined path before
/// `openDir` saw it, and the `catch return false` swallowed the failure. The
/// observable consequence was that every slash-bearing capability pattern
/// silently matched nothing -- including the shipped `protobuf` rule's
/// `proto/*.proto` arm, and any subdirectory glob in an operator's own rules
/// file, with no diagnostic. Measured deterministic over five builds and
/// three fixtures before it was reproduced, and again before it was fixed.
///
/// Reproducing it was correct while the oracle existed (D2), because
/// repairing it emits capability tags the oracle never emits and the routing
/// table is a persisted WRITE path. Decision 1067 ended that: the bug-for-bug
/// rule does not outlive the oracle, and this row was in its FIX set.
///
/// A traversing pattern (`..`) is refused rather than followed -- the rules
/// file is operator-authored but the scan is rooted at a project directory,
/// and escaping it would let a rule probe arbitrary paths.
///
/// No entry-kind test, deliberately: the original matches a directory's name
/// as readily as a file's, and that part was never the defect.
auto pattern_finds(const std::filesystem::path& root, std::string_view pattern) -> bool {
  if (pattern.empty()) {
    return false;
  }

  auto       scan_root = root;
  auto       leaf      = pattern;
  auto const slash     = pattern.find_last_of('/');
  if (slash != std::string_view::npos) {
    auto const dir_part = pattern.substr(0, slash);
    leaf                = pattern.substr(slash + 1);
    if (leaf.empty() || dir_part.empty()) {
      return false;
    }
    // Refuse traversal rather than normalizing it away: a rule that escapes
    // the project root is a rules-file bug, and silently clamping it would
    // hide that the same way the original defect hid itself.
    for (auto const part : split(dir_part, '/')) {
      if (part == ".." || part.empty()) {
        return false;
      }
      scan_root /= part;
    }
  }

  for (const auto& entry : list_dir(scan_root)) {
    if (simple_glob_match(leaf, entry.name)) {
      return true;
    }
  }
  return false;
}

/// @brief Does `path` declare `package main`?
///
/// The FIRST non-blank, non-`//` line decides: if it opens with `package `
/// the answer is whether the remainder is `main`, and if it does not the
/// file is rejected outright rather than searched further.
auto file_declares_package_main(const std::filesystem::path& path) -> bool {
  const auto raw = read_file(path);
  if (!raw.has_value()) {
    return false;
  }
  for (const auto line : split(*raw, '\n')) {
    const auto text = trim(line, " \t\r");
    if (text.empty() || text.starts_with("//")) {
      continue;
    }
    if (text.starts_with("package ")) {
      return trim(text.substr(std::string_view{"package "}.size()), " \t") == "main";
    }
    return false;
  }
  return false;
}

/// @brief Does any `.go` file directly in `dir` declare `package main`?
auto has_go_main_in_dir(const std::filesystem::path& dir) -> bool {
  for (const auto& entry : list_dir(dir)) {
    if (!entry.is_file || !std::string_view{entry.name}.ends_with(".go")) {
      continue;
    }
    if (file_declares_package_main(dir / entry.name)) {
      return true;
    }
  }
  return false;
}

/// @brief Does the project declare a Go `main`, at its root or under `cmd/`?
///
/// Exactly two levels: `<root>/*.go` and `<root>/cmd/<any>/*.go`. A `main`
/// nested any deeper is not found.
auto has_go_main(const std::filesystem::path& root) -> bool {
  if (has_go_main_in_dir(root)) {
    return true;
  }
  const auto cmd_dir = root / "cmd";
  for (const auto& entry : list_dir(cmd_dir)) {
    if (entry.is_dir && has_go_main_in_dir(cmd_dir / entry.name)) {
      return true;
    }
  }
  return false;
}

/// @brief Does `package.json` declare `dep` in either dependency block?
auto package_has_dep(const std::filesystem::path& root, std::string_view dep) -> bool {
  const auto raw = read_file(root / "package.json");
  if (!raw.has_value()) {
    return false;
  }
  const auto parsed = json_dom::parse_json(*raw);
  if (!parsed.has_value() || parsed->kind != json_dom::json_kind::object) {
    return false;
  }
  for (const auto* block : {parsed->find("dependencies"), parsed->find("devDependencies")}) {
    if (block != nullptr && block->kind == json_dom::json_kind::object && block->find(dep) != nullptr) {
      return true;
    }
  }
  return false;
}

/// @brief Do this rule's pattern lists match `root`?
///
/// A rule with BOTH lists empty matches NOTHING — oracle-captured with a
/// bare `[[rule]]` header, which produced no capability at all. That is what
/// keeps a malformed or half-written rule inert rather than universal.
auto rule_matches(const std::filesystem::path& root, const capability_rule& rule) -> bool {
  if (rule.match_all.empty() && rule.match_any.empty()) {
    return false;
  }
  for (const auto& pattern : rule.match_all) {
    if (!pattern_finds(root, pattern)) {
      return false;
    }
  }
  if (!rule.match_any.empty()) {
    if (!std::ranges::any_of(rule.match_any, [&](const std::string& p) { return pattern_finds(root, p); })) {
      return false;
    }
  }
  return true;
}

/// @brief The capability tags `root` earns, in RULE order.
///
/// The one cross-rule interaction: `go-service` suppresses `go-library`,
/// because both are keyed on `go.mod` and a binary is not also a library for
/// this purpose. Applied after the whole rule sweep, so it does not depend
/// on which of the two was evaluated first.
auto detect_capabilities(const std::filesystem::path& root, const std::vector<capability_rule>& rules)
    -> std::vector<std::string> {
  std::vector<std::string> out;
  for (const auto& rule : rules) {
    if (!rule_matches(root, rule)) {
      continue;
    }
    if (!rule.package_dep.empty() && !package_has_dep(root, rule.package_dep)) {
      continue;
    }
    if (rule.go_main && !has_go_main(root)) {
      continue;
    }
    out.push_back(rule.tag);
  }
  if (std::ranges::contains(out, "go-service")) {
    const auto stale = std::ranges::remove(out, "go-library");
    out.erase(stale.begin(), stale.end());
  }
  return out;
}

// ===========================================================================
// README summary
// ===========================================================================

/// @brief The first prose paragraph of `filename`, flattened to one line.
///
/// Paragraphs are blank-line separated. A paragraph whose FIRST line is a
/// heading (`#` after trimming) contributes its remaining lines only, and a
/// heading-only paragraph is skipped entirely — which is why a README
/// containing nothing but `# beta` yields an EMPTY summary. Oracle-captured
/// on exactly that file.
///
/// Surviving lines are trimmed and joined with single spaces.
auto first_paragraph(const std::filesystem::path& filename) -> std::string {
  const auto raw = read_file(filename);
  if (!raw.has_value()) {
    return {};
  }
  std::string normalized;
  normalized.reserve(raw->size());
  for (std::size_t i = 0; i < raw->size(); ++i) {
    if ((*raw)[i] == '\r' && i + 1 < raw->size() && (*raw)[i + 1] == '\n') {
      continue;
    }
    normalized.push_back((*raw)[i]);
  }

  std::vector<std::vector<std::string_view>> paragraphs;
  std::vector<std::string_view>              current;
  for (const auto line : split(normalized, '\n')) {
    if (trim(line, " \t").empty()) {
      if (!current.empty()) {
        paragraphs.push_back(std::move(current));
        current.clear();
      }
      continue;
    }
    current.push_back(line);
  }
  if (!current.empty()) {
    paragraphs.push_back(std::move(current));
  }

  for (const auto& paragraph : paragraphs) {
    std::size_t start = 0;
    if (!paragraph.empty() && trim(paragraph[0], " \t").starts_with("#")) {
      if (paragraph.size() == 1) {
        continue;
      }
      start = 1;
    }
    std::string out;
    for (std::size_t i = start; i < paragraph.size(); ++i) {
      if (i > start) {
        out += ' ';
      }
      out += trim(paragraph[i], " \t");
    }
    const auto text = trim(out, " \t");
    if (!text.empty()) {
      return std::string{text};
    }
  }
  return {};
}

/// @brief The first README-ish file's summary, or empty.
///
/// The four names are tried in order. On a case-insensitive filesystem the
/// first two name the same file, which is harmless: the loop stops at the
/// first NON-EMPTY paragraph either way.
auto first_paragraph_in_root(const std::filesystem::path& root) -> std::string {
  for (const auto* name : {"README.md", "readme.md", "README", "readme"}) {
    const auto path = root / name;
    if (!path_exists(path)) {
      continue;
    }
    auto paragraph = first_paragraph(path);
    if (!paragraph.empty()) {
      return paragraph;
    }
  }
  return {};
}

// ===========================================================================
// Cross-repo dependencies
// ===========================================================================

/// @brief One workspace member, as the builder reads it.
struct member_row {
  std::int64_t id = 0;
  std::string  slug;
  std::string  root_path;
  std::string  git_remote;
};

/// @brief The inferred sibling dependencies, plus where they came from.
struct inferred_deps {
  std::vector<std::string> deps;   ///< Sibling slugs, sorted and unique.
  std::string              source; ///< `go.mod`, `package.json`, or empty.
};

/// @brief Add every sibling named by a `workspace:` dependency value.
auto collect_workspace_deps(const json_dom::json_value* block, std::span<const member_row> siblings, std::string_view self,
                            std::vector<std::string>& out) -> void {
  if (block == nullptr || block->kind != json_dom::json_kind::object) {
    return;
  }
  for (const auto& [key, value] : block->object) {
    if (value.kind != json_dom::json_kind::string || !std::string_view{value.string}.starts_with("workspace:")) {
      continue;
    }
    for (const auto& sibling : siblings) {
      if (sibling.slug != self && sibling.slug == key) {
        out.push_back(sibling.slug);
        break;
      }
    }
  }
}

/// @brief Infer which SIBLING projects this one depends on.
///
/// Two sources, both consulted, in this order:
///
///   - `go.mod`: every line containing `=>` whose right-hand side is a
///     RELATIVE or ABSOLUTE path. The path is resolved against the project
///     root and compared to each sibling's recorded `root_path` as a STRING.
///     Note the left-hand side is never examined, and the scan is not
///     limited to a `replace` block.
///   - `package.json`: `dependencies` and `devDependencies` entries whose
///     VALUE begins `workspace:` and whose KEY is a sibling slug.
///
/// `source` records only the FIRST source to contribute: when `go.mod`
/// matched, a later `package.json` match does not overwrite it. That is
/// visible in `depends_on_source` and in every `dependency_edges.reason`
/// derived from it.
auto infer_dependencies(const std::filesystem::path& project_root, std::string_view self, std::span<const member_row> siblings)
    -> inferred_deps {
  std::vector<std::string> deps;
  std::string              source;

  const auto gomod = project_root / "go.mod";
  if (path_exists(gomod)) {
    const auto body = read_file(gomod).value_or(std::string{});
    for (const auto line : split(body, '\n')) {
      const auto arrow = line.find("=>");
      if (arrow == std::string_view::npos) {
        continue;
      }
      const auto rhs = trim(line.substr(arrow + 2), " \t\r");
      if (rhs.empty() || (!rhs.starts_with(".") && !rhs.starts_with("/"))) {
        continue;
      }
      const std::filesystem::path target =
          rhs.starts_with("/") ? std::filesystem::path{trim(rhs, " \t")} : (project_root / rhs).lexically_normal();
      // `lexically_normal` leaves a trailing separator on a path that had
      // one; the recorded `root_path` never does, so strip it before the
      // string compare that decides the match.
      auto resolved = target.string();
      while (resolved.size() > 1 && resolved.back() == '/') {
        resolved.pop_back();
      }
      for (const auto& sibling : siblings) {
        if (sibling.slug != self && sibling.root_path == resolved) {
          deps.push_back(sibling.slug);
          source = "go.mod";
        }
      }
    }
  }

  const auto package_json = project_root / "package.json";
  if (path_exists(package_json)) {
    const auto raw = read_file(package_json).value_or(std::string{});
    if (!raw.empty()) {
      const auto parsed = json_dom::parse_json(raw);
      if (parsed.has_value() && parsed->kind == json_dom::json_kind::object) {
        collect_workspace_deps(parsed->find("dependencies"), siblings, self, deps);
        collect_workspace_deps(parsed->find("devDependencies"), siblings, self, deps);
        if (source.empty() && !deps.empty()) {
          source = "package.json";
        }
      }
    }
  }

  sort_unique(deps);
  return inferred_deps{.deps = std::move(deps), .source = std::move(source)};
}

/// @brief Up to five notable paths, sorted.
///
/// Three sources: `cmd/<x>/main.go` for every `cmd` subdirectory that has
/// one, a fixed candidate list, and every non-dot FILE under `bin/`. The
/// truncation to five happens AFTER the sort, so which five survive is
/// deterministic — oracle-captured on a repository with nine candidates,
/// which kept `Cargo.toml`, `Makefile`, `bin/zz`, `cmd/aa/main.go`,
/// `cmd/bb/main.go` and dropped the rest.
auto detect_entry_points(const std::filesystem::path& root) -> std::vector<std::string> {
  std::vector<std::string> found;

  const auto cmd_dir = root / "cmd";
  if (path_exists(cmd_dir)) {
    for (const auto& entry : list_dir(cmd_dir)) {
      if (!entry.is_dir) {
        continue;
      }
      const auto relative = std::format("cmd/{}/main.go", entry.name);
      if (path_exists(root / relative)) {
        found.push_back(relative);
      }
    }
  }

  static constexpr std::array<std::string_view, 11> k_candidates{
      "src/index.ts", "src/index.tsx", "src/index.js", "src/index.jsx",  "main.py", "manage.py",
      "Makefile",     "Cargo.toml",    "package.json", "pyproject.toml", "go.mod",
  };
  for (const auto candidate : k_candidates) {
    if (path_exists(root / candidate)) {
      found.emplace_back(candidate);
    }
  }

  const auto bin_dir = root / "bin";
  if (path_exists(bin_dir)) {
    for (const auto& entry : list_dir(bin_dir)) {
      if (entry.is_file && !entry.name.empty() && entry.name.front() != '.') {
        found.push_back(std::format("bin/{}", entry.name));
      }
    }
  }

  sort_unique(found);
  if (found.size() > 5) {
    found.resize(5);
  }
  return found;
}

// ===========================================================================
// Language census
// ===========================================================================

/// @brief The extension-to-language table, transcribed verbatim.
auto language_for_ext(std::string_view ext) -> std::optional<std::string_view> {
  static const std::unordered_map<std::string_view, std::string_view> k_table{
      {".go", "go"},
      {".js", "javascript"},
      {".jsx", "javascript"},
      {".mjs", "javascript"},
      {".cjs", "javascript"},
      {".ts", "typescript"},
      {".tsx", "typescript"},
      {".py", "python"},
      {".rs", "rust"},
      {".java", "java"},
      {".kt", "kotlin"},
      {".rb", "ruby"},
      {".sh", "shell"},
      {".bash", "shell"},
      {".zsh", "shell"},
      {".md", "markdown"},
      {".markdown", "markdown"},
      {".toml", "toml"},
      {".yaml", "yaml"},
      {".yml", "yaml"},
      {".json", "json"},
      {".html", "html"},
      {".css", "css"},
      {".scss", "css"},
      {".sql", "sql"},
      {".proto", "protobuf"},
      {".c", "c"},
      {".h", "c"},
      {".cpp", "cpp"},
      {".cc", "cpp"},
      {".hpp", "cpp"},
      {".swift", "swift"},
      {".m", "objective-c"},
      {".mm", "objective-c"},
      {".lua", "lua"},
      {".php", "php"},
      {".cs", "csharp"},
  };
  const auto found = k_table.find(ext);
  return found == k_table.end() ? std::nullopt : std::optional{found->second};
}

/// @brief Zig's `std.fs.path.extension`: the last `.` onwards, unless it is
/// the first byte of the name.
///
/// So a dotfile such as `.gitignore` has NO extension, while `.hidden.go`
/// has `.go` and is counted. Dot-FILES are walked; dot-DIRECTORIES are not.
auto extension_of(std::string_view name) -> std::string_view {
  const auto dot = name.find_last_of('.');
  if (dot == std::string_view::npos || dot == 0) {
    return {};
  }
  return name.substr(dot);
}

/// @brief Directories the census never descends into.
auto should_skip_dir(std::string_view name) -> bool {
  if (name.empty() || name.front() == '.') {
    return true;
  }
  return name == "node_modules" || name == "vendor" || name == "target" || name == "dist" || name == "build" || name == ".venv";
}

/// @brief Recursively count files per language.
///
/// The `max_files` cap is SOFT and deliberately so. The original increments
/// its counter per file and returns from the CURRENT directory once it is
/// exceeded, which leaves the parent loop free to descend into further
/// siblings and count more. Reproduced exactly — a hard cap would change
/// which files are counted on a large repository.
auto walk_language(const std::filesystem::path& dir, std::size_t max_files, std::size_t& visited,
                   std::unordered_map<std::string, std::int64_t>& counts) -> void {
  for (const auto& entry : list_dir(dir)) {
    if (entry.is_dir) {
      if (should_skip_dir(entry.name)) {
        continue;
      }
      walk_language(dir / entry.name, max_files, visited, counts);
    } else if (entry.is_file) {
      visited += 1;
      if (visited > max_files) {
        return;
      }
      const auto language = language_for_ext(extension_of(entry.name));
      if (language.has_value()) {
        counts[std::string{*language}] += 1;
      }
    }
  }
}

/// @brief Each language's share of the counted files, rounded to two places.
///
/// Returns EMPTY when nothing was counted — the oracle emits `{}` rather
/// than a map of zeroes, captured on a repository holding one `.bin` file.
///
/// The shares are `round(pct * 100) / 100`, so a language present but below
/// half a percent rounds to a literal `0` and is still emitted. Captured on
/// a 400-file repository with one Go file: `"go": 0, "markdown": 1`.
///
/// DIVERGENCE: the result is SORTED BY KEY. The oracle's order is a hash
/// artifact — see routing.cppm's header.
auto count_languages(const std::filesystem::path& root, std::size_t max_files) -> std::vector<std::pair<std::string, double>> {
  std::unordered_map<std::string, std::int64_t> counts;
  std::size_t                                   visited = 0;
  walk_language(root, max_files, visited, counts);

  std::int64_t total = 0;
  for (const auto& [_, count] : counts) {
    total += count;
  }
  std::vector<std::pair<std::string, double>> out;
  if (total == 0) {
    return out;
  }
  out.reserve(counts.size());
  for (const auto& [language, count] : counts) {
    const auto share = static_cast<double>(count) / static_cast<double>(total);
    out.emplace_back(language, std::round(share * 100.0) / 100.0);
  }
  std::ranges::sort(out, {}, &std::pair<std::string, double>::first);
  return out;
}

// ===========================================================================
// Database reads
// ===========================================================================

/// @brief Run a one-parameter query returning a single id column.
auto query_ids(db::connection& conn, std::string_view sql, std::int64_t arg) -> std::optional<std::vector<std::int64_t>> {
  auto stmt = conn.prepare(sql);
  if (!stmt.has_value() || !stmt->bind_int64(1, arg).has_value()) {
    return std::nullopt;
  }
  std::vector<std::int64_t> out;
  while (true) {
    auto step = stmt->step();
    if (!step.has_value()) {
      return std::nullopt;
    }
    if (*step == db::step_result::done) {
      return out;
    }
    out.push_back(stmt->column_int64(0));
  }
}

/// @brief Run a two-parameter query returning a single id column.
auto query_ids2(db::connection& conn, std::string_view sql, std::int64_t arg) -> std::optional<std::vector<std::int64_t>> {
  auto stmt = conn.prepare(sql);
  if (!stmt.has_value() || !stmt->bind_int64(1, arg).has_value() || !stmt->bind_int64(2, arg).has_value()) {
    return std::nullopt;
  }
  std::vector<std::int64_t> out;
  while (true) {
    auto step = stmt->step();
    if (!step.has_value()) {
      return std::nullopt;
    }
    if (*step == db::step_result::done) {
      return out;
    }
    out.push_back(stmt->column_int64(0));
  }
}

/// @brief Run a one-parameter `count(*)` query.
auto scalar_count(db::connection& conn, std::string_view sql, std::int64_t arg) -> std::optional<std::int64_t> {
  auto stmt = conn.prepare(sql);
  if (!stmt.has_value() || !stmt->bind_int64(1, arg).has_value()) {
    return std::nullopt;
  }
  auto step = stmt->step();
  if (!step.has_value()) {
    return std::nullopt;
  }
  return *step == db::step_result::done ? 0 : stmt->column_int64(0);
}

/// @brief The org's member projects, ordered by slug.
auto list_members(db::connection& conn, std::int64_t org_id) -> std::optional<std::vector<member_row>> {
  auto stmt = conn.prepare("select p.id, p.slug, coalesce(p.root_path, ''), coalesce(p.git_remote, '')\n"
                           "from projects p\n"
                           "join project_associations pa on pa.project_id = p.id\n"
                           "where pa.association_id = ?\n"
                           "order by p.slug");
  if (!stmt.has_value() || !stmt->bind_int64(1, org_id).has_value()) {
    return std::nullopt;
  }
  std::vector<member_row> out;
  while (true) {
    auto step = stmt->step();
    if (!step.has_value()) {
      return std::nullopt;
    }
    if (*step == db::step_result::done) {
      return out;
    }
    out.push_back(member_row{
        .id         = stmt->column_int64(0),
        .slug       = stmt->column_text(1),
        .root_path  = stmt->column_text(2),
        .git_remote = stmt->column_text(3),
    });
  }
}

/// @brief One project's Planar workload.
///
/// `active_plans` is a UNION of two arms: plans scoped directly to the repo,
/// and plans linked to it by a `touches` edge. The task and question counts
/// are repo-scope ONLY and do NOT follow `touches`, which is why an active
/// plan can appear here with a zero task count.
///
/// `recent_session_ids` is always empty. The field is emitted, decoded and
/// never populated — in the oracle too.
auto load_planar_focus(db::connection& conn, std::int64_t project_id) -> std::optional<planar_focus> {
  auto active = query_ids2(conn,
                           "select distinct id from plans\n"
                           "where status = 'active' and (\n"
                           "  (scope_kind = 'repo' and scope_id = ?)\n"
                           "  or id in (\n"
                           "    select from_id from entity_links\n"
                           "    where from_kind = 'plan' and to_kind = 'repo' and to_id = ? and relationship = 'touches'\n"
                           "  )\n"
                           ")\n"
                           "order by id",
                           project_id);
  if (!active.has_value()) {
    return std::nullopt;
  }
  auto open_tasks = scalar_count(conn,
                                 "select count(*) from tasks\n"
                                 "where status in ('todo','doing','blocked') and scope_kind = 'repo' and scope_id = ?",
                                 project_id);
  if (!open_tasks.has_value()) {
    return std::nullopt;
  }
  auto open_questions = scalar_count(conn,
                                     "select count(*) from questions\n"
                                     "where status = 'open' and scope_kind = 'repo' and scope_id = ?",
                                     project_id);
  if (!open_questions.has_value()) {
    return std::nullopt;
  }
  return planar_focus{
      .active_plans       = *std::move(active),
      .open_tasks         = *open_tasks,
      .open_questions     = *open_questions,
      .recent_session_ids = {},
  };
}

/// @brief The build timestamp, from SQLite rather than the host clock.
auto db_now(db::connection& conn) -> std::optional<std::string> {
  auto stmt = conn.prepare("select strftime('%Y-%m-%dT%H:%M:%SZ','now')");
  if (!stmt.has_value()) {
    return std::nullopt;
  }
  auto step = stmt->step();
  if (!step.has_value() || *step == db::step_result::done) {
    return std::nullopt;
  }
  return stmt->column_text(0);
}

/// @brief The prose reason a dependency edge carries, from its source tag.
///
/// An UNRECOGNISED source passes through unchanged rather than mapping to a
/// default, so an empty source yields an empty reason.
auto reason_for(std::string_view source) -> std::string_view {
  if (source == "go.mod") {
    return "go.mod replace";
  }
  if (source == "package.json") {
    return "package.json workspace dep";
  }
  if (source == "manual") {
    return "operator override";
  }
  return source;
}

// ===========================================================================
// The TOML sliver the capability-rules file needs
// ===========================================================================

/// @brief The value after `=`, or nullopt when the line has no `=`.
auto rhs_of(std::string_view line) -> std::optional<std::string_view> {
  const auto eq = line.find('=');
  if (eq == std::string_view::npos) {
    return std::nullopt;
  }
  return trim(line.substr(eq + 1), " \t");
}

/// @brief A DOUBLE-quoted scalar, with NO escape processing.
///
/// The original slices between the first and last byte after checking both
/// are `"`, so `"a\"b"` yields the six bytes `a\"b` verbatim. A
/// SINGLE-quoted value is rejected here even though the array parser accepts
/// one — oracle-captured, `tag = 'x'` exits 2 while `match_all = ['x']`
/// succeeds. The asymmetry is the original's.
auto parse_toml_string(std::string_view line) -> std::expected<std::string, rules_error> {
  const auto rhs = rhs_of(line);
  if (!rhs.has_value()) {
    return std::unexpected(rules_error::invalid);
  }
  if (rhs->size() < 2 || rhs->front() != '"' || rhs->back() != '"') {
    return std::unexpected(rules_error::invalid);
  }
  return std::string{rhs->substr(1, rhs->size() - 2)};
}

/// @brief Scan one quoted string starting at `pos`.
auto scan_quoted(std::string_view text, std::size_t& pos, std::string& out) -> bool {
  const char quote = text[pos];
  ++pos;
  while (pos < text.size()) {
    const char ch = text[pos++];
    if (ch == quote) {
      return true;
    }
    // Only DOUBLE-quoted strings process escapes; a single-quoted literal
    // takes every byte as-is.
    if (quote == '"' && ch == '\\') {
      if (pos >= text.size()) {
        return false;
      }
      switch (const char esc = text[pos++]) {
      case '"':
        out += '"';
        break;
      case '\\':
        out += '\\';
        break;
      case 'n':
        out += '\n';
        break;
      case 'r':
        out += '\r';
        break;
      case 't':
        out += '\t';
        break;
      case '0':
        out += '\0';
        break;
      default:
        (void)esc;
        return false;
      }
      continue;
    }
    out += ch;
  }
  return false;
}

/// @brief An inline array of strings.
///
/// The whole malformed-input surface below was oracle-captured one line at a
/// time, because the split between the two error arms is not derivable from
/// the input's shape:
///
///     match_all = [oops]      ParseFailed   exit 1
///     match_all = ["a"        ParseFailed   exit 1
///     match_all = 1.5         ParseFailed   exit 1   (floats are unsupported)
///     match_all = ["a"] zz    ParseFailed   exit 1   (trailing junk)
///     match_all = "a"         InvalidInput  exit 2   (parses, is not an array)
///     match_all = 7           InvalidInput  exit 2
///     match_all = true        InvalidInput  exit 2
///     match_all               InvalidInput  exit 2   (no `=` at all)
///     match_all = []          OK — and the rule is then INERT
///     match_all = ['a']       OK — single quotes are fine HERE
///
/// The distinction is whether the ORIGINAL's full TOML parser would accept
/// the value at all: a value it parses and that simply is not an array is
/// `InvalidInput`, and one it cannot parse is `ParseFailed`.
auto parse_toml_string_array(std::string_view line) -> std::expected<std::vector<std::string>, rules_error> {
  const auto rhs_opt = rhs_of(line);
  if (!rhs_opt.has_value()) {
    return std::unexpected(rules_error::invalid);
  }
  const auto rhs = *rhs_opt;
  if (rhs.empty()) {
    return std::unexpected(rules_error::parse_failed);
  }

  if (rhs.front() != '[') {
    // Not an array. Decide between "parses as some other TOML scalar"
    // (InvalidInput) and "does not parse at all" (ParseFailed).
    std::size_t pos = 0;
    if (rhs.front() == '"' || rhs.front() == '\'') {
      std::string discard;
      if (!scan_quoted(rhs, pos, discard)) {
        return std::unexpected(rules_error::parse_failed);
      }
    } else if (rhs.front() == '-' || (rhs.front() >= '0' && rhs.front() <= '9')) {
      if (rhs.front() == '-') {
        ++pos;
      }
      const auto digits_from = pos;
      while (pos < rhs.size() && rhs[pos] >= '0' && rhs[pos] <= '9') {
        ++pos;
      }
      // A `.` after the digits is a float, and floats are unsupported.
      if (pos == digits_from || (pos < rhs.size() && rhs[pos] == '.')) {
        return std::unexpected(rules_error::parse_failed);
      }
    } else if (rhs.starts_with("true")) {
      pos = 4;
    } else if (rhs.starts_with("false")) {
      pos = 5;
    } else {
      return std::unexpected(rules_error::parse_failed);
    }
    // Trailing junk after an otherwise-valid scalar is a parse failure, not
    // a type mismatch.
    if (!trim(rhs.substr(pos), " \t").empty()) {
      return std::unexpected(rules_error::parse_failed);
    }
    return std::unexpected(rules_error::invalid);
  }

  std::vector<std::string> out;
  std::size_t              pos = 1;
  while (true) {
    while (pos < rhs.size() && (rhs[pos] == ' ' || rhs[pos] == '\t')) {
      ++pos;
    }
    if (pos >= rhs.size()) {
      return std::unexpected(rules_error::parse_failed);
    }
    if (rhs[pos] == ']') {
      ++pos;
      break;
    }
    if (rhs[pos] != '"' && rhs[pos] != '\'') {
      return std::unexpected(rules_error::parse_failed);
    }
    std::string item;
    if (!scan_quoted(rhs, pos, item)) {
      return std::unexpected(rules_error::parse_failed);
    }
    out.push_back(std::move(item));
    while (pos < rhs.size() && (rhs[pos] == ' ' || rhs[pos] == '\t')) {
      ++pos;
    }
    if (pos >= rhs.size()) {
      return std::unexpected(rules_error::parse_failed);
    }
    if (rhs[pos] == ',') {
      ++pos;
    } else if (rhs[pos] != ']') {
      return std::unexpected(rules_error::parse_failed);
    }
  }
  if (!trim(rhs.substr(pos), " \t").empty()) {
    return std::unexpected(rules_error::parse_failed);
  }
  return out;
}

// ===========================================================================
// The JSON encoder
// ===========================================================================
//
// The layout rules — two-space indent, `": "`, `[]` / `{}` collapsed inline
// while a non-empty container always breaks, no trailing newline — are
// `planar.json_dom`'s `stringify_indent2`, which exists precisely because
// they are a byte contract with the oracle rather than a formatting taste.
// So this builds a DOM in the field order the Zig struct declares and hands
// it over, instead of carrying a second indentation engine that could drift
// from the first.
//
// `json_value::object` is insertion-ordered, so the DOM preserves both the
// struct's field order and this builder's chosen `languages` key order.

/// @brief A JSON string node.
auto node(std::string_view text) -> json_dom::json_value {
  return json_dom::json_value{.kind = json_dom::json_kind::string, .string = std::string{text}};
}

/// @brief A JSON integer node.
auto node(std::int64_t value) -> json_dom::json_value {
  return json_dom::json_value{.kind = json_dom::json_kind::integer, .integer = value};
}

/// @brief A JSON array of strings.
auto node(std::span<const std::string> values) -> json_dom::json_value {
  json_dom::json_value out{.kind = json_dom::json_kind::array};
  for (const auto& value : values) {
    out.array.push_back(node(value));
  }
  return out;
}

/// @brief A JSON array of integers.
auto node(std::span<const std::int64_t> values) -> json_dom::json_value {
  json_dom::json_value out{.kind = json_dom::json_kind::array};
  for (const auto value : values) {
    out.array.push_back(node(value));
  }
  return out;
}

/// @brief A JSON object from ordered members.
auto object(std::vector<std::pair<std::string, json_dom::json_value>> members) -> json_dom::json_value {
  return json_dom::json_value{.kind = json_dom::json_kind::object, .object = std::move(members)};
}

} // namespace

auto rules_error_name(rules_error value) -> std::string_view {
  switch (value) {
  case rules_error::parse_failed:
    return "ParseFailed";
  case rules_error::invalid:
    return "InvalidInput";
  }
  return "InvalidInput";
}

auto default_capability_rules() -> std::vector<capability_rule> {
  return {
      {.tag = "go-service", .match_all = {"go.mod"}, .match_any = {}, .package_dep = "", .go_main = true},
      {.tag = "go-library", .match_all = {"go.mod"}, .match_any = {}, .package_dep = "", .go_main = false},
      {.tag = "node-service", .match_all = {"package.json"}, .match_any = {}, .package_dep = "express", .go_main = false},
      {.tag = "react-app", .match_all = {"package.json"}, .match_any = {}, .package_dep = "react", .go_main = false},
      {.tag = "rust", .match_all = {"Cargo.toml"}, .match_any = {}, .package_dep = "", .go_main = false},
      {.tag         = "python",
       .match_all   = {},
       .match_any   = {"pyproject.toml", "setup.py", "requirements.txt"},
       .package_dep = "",
       .go_main     = false},
      // The second pattern is DEAD in the oracle — it contains a slash. See
      // routing.cppm's header; `*.proto` is the only arm that ever fires.
      {.tag = "protobuf", .match_all = {}, .match_any = {"*.proto", "proto/*.proto"}, .package_dep = "", .go_main = false},
  };
}

auto load_capability_rules(const std::filesystem::path& path) -> std::expected<std::vector<capability_rule>, rules_error> {
  const auto raw = read_file(path);
  if (!raw.has_value()) {
    // An ABSENT file is not an error; the caller reads the empty result as
    // "use the defaults".
    return std::vector<capability_rule>{};
  }

  std::vector<capability_rule>   rules;
  std::optional<capability_rule> current;
  for (const auto raw_line : split(*raw, '\n')) {
    // `#` starts a comment ANYWHERE on the line, including inside what would
    // otherwise be a quoted value. Oracle-captured: `tag = "c" # trailing`
    // yields the tag `c`.
    const auto hash    = raw_line.find('#');
    const auto trimmed = trim(hash == std::string_view::npos ? raw_line : raw_line.substr(0, hash), " \t\r");
    if (trimmed.empty()) {
      continue;
    }
    if (trimmed == "[[rule]]") {
      if (current.has_value()) {
        rules.push_back(*std::move(current));
      }
      current = capability_rule{};
      continue;
    }
    // A key/value line BEFORE any `[[rule]]` header is silently dropped.
    // Oracle-captured: a file that is nothing but such lines yields zero
    // rules, so the caller falls back to the defaults.
    if (!current.has_value()) {
      continue;
    }
    // Keys are matched by PREFIX, not by equality, so `tagx = "v"` sets
    // `tag`. Oracle-captured.
    if (trimmed.starts_with("tag")) {
      auto value = parse_toml_string(trimmed);
      if (!value.has_value()) {
        return std::unexpected(value.error());
      }
      current->tag = *std::move(value);
    } else if (trimmed.starts_with("match_all")) {
      auto value = parse_toml_string_array(trimmed);
      if (!value.has_value()) {
        return std::unexpected(value.error());
      }
      current->match_all = *std::move(value);
    } else if (trimmed.starts_with("match_any")) {
      auto value = parse_toml_string_array(trimmed);
      if (!value.has_value()) {
        return std::unexpected(value.error());
      }
      current->match_any = *std::move(value);
    } else if (trimmed.starts_with("package_dep")) {
      auto value = parse_toml_string(trimmed);
      if (!value.has_value()) {
        return std::unexpected(value.error());
      }
      current->package_dep = *std::move(value);
    } else if (trimmed.starts_with("go_main")) {
      // A SUBSTRING test, not a boolean parse. Oracle-captured:
      // `go_main = "untrue"` contains `true` and therefore enables the
      // check.
      current->go_main = trimmed.find("true") != std::string_view::npos;
    }
  }
  if (current.has_value()) {
    rules.push_back(*std::move(current));
  }
  return rules;
}

auto load_overrides(const std::filesystem::path& path) -> std::expected<manual_edits, decode_error> {
  const auto raw = read_file(path);
  if (!raw.has_value()) {
    return manual_edits{};
  }

  auto parsed = json_dom::parse_json_reason(*raw);
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
  if (parsed->kind != json_dom::json_kind::object) {
    return std::unexpected(decode_error::invalid);
  }

  manual_edits out;
  if (const auto* version = parsed->find("schema_version"); version != nullptr && version->kind == json_dom::json_kind::integer) {
    out.schema_version = version->integer;
  }
  const auto* projects = parsed->find("projects");
  if (projects == nullptr || projects->kind != json_dom::json_kind::object) {
    return out;
  }
  for (const auto& [slug, value] : projects->object) {
    if (value.kind != json_dom::json_kind::object) {
      continue;
    }
    project_override entry;
    if (const auto* summary = value.find("summary"); summary != nullptr && summary->kind == json_dom::json_kind::string) {
      entry.summary = summary->string;
    }
    // PRESENT-AS-AN-ARRAY is the whole test, empty included. The original
    // spells this `caps.len > 0 or hasArray(...)`, whose first arm can only
    // be true when the second is, so the two collapse to this one condition.
    // The empty case is observable: `{"capabilities": []}` clears the tags
    // AND sets `capabilities_source` to `manual`. Oracle-captured.
    for (const auto& [name, slot] :
         {std::pair{"capabilities", &entry.capabilities}, std::pair{"depends_on", &entry.depends_on}}) {
      const auto* array = value.find(name);
      if (array == nullptr || array->kind != json_dom::json_kind::array) {
        continue;
      }
      std::vector<std::string> items;
      for (const auto& item : array->array) {
        if (item.kind == json_dom::json_kind::string) {
          items.push_back(item.string);
        }
      }
      *slot = std::move(items);
    }
    out.projects.emplace_back(slug, std::move(entry));
  }
  return out;
}

auto apply_overrides(routing_table& table, const manual_edits& values) -> void {
  for (auto& project : table.projects) {
    const auto found = std::ranges::find(values.projects, project.slug, &std::pair<std::string, project_override>::first);
    if (found == values.projects.end()) {
      continue;
    }
    const auto& entry = found->second;
    if (entry.summary.has_value()) {
      project.summary        = *entry.summary;
      project.summary_source = "manual";
    }
    if (entry.capabilities.has_value()) {
      project.capabilities = *entry.capabilities;
      sort_unique(project.capabilities);
      project.capabilities_source = "manual";
    }
    if (entry.depends_on.has_value()) {
      project.depends_on = *entry.depends_on;
      sort_unique(project.depends_on);
      project.depends_on_source = "manual";
    }
  }
}

auto build(db::connection& conn, std::int64_t org_id, std::string_view org_slug, std::string_view org_name,
           const std::vector<capability_rule>& rules) -> std::expected<routing_table, build_error> {
  auto members = list_members(conn, org_id);
  if (!members.has_value()) {
    return std::unexpected(build_error::query_failed);
  }
  auto stamp = db_now(conn);
  if (!stamp.has_value()) {
    return std::unexpected(build_error::query_failed);
  }

  routing_table table{
      .schema_version    = schema_version_current,
      .workspace_id      = org_id,
      .workspace_slug    = std::string{org_slug},
      .workspace_name    = std::string{org_name},
      .generated_at      = *std::move(stamp),
      .generator_version = std::string{generator_version_static},
      .projects          = {},
      .cross             = {},
  };

  for (const auto& member : *members) {
    const std::filesystem::path root{member.root_path};

    auto capabilities = detect_capabilities(root, rules);
    sort_unique(capabilities);
    auto deps    = infer_dependencies(root, member.slug, *members);
    auto summary = first_paragraph_in_root(root);
    auto focus   = load_planar_focus(conn, member.id);
    if (!focus.has_value()) {
      return std::unexpected(build_error::query_failed);
    }

    const bool has_capabilities = !capabilities.empty();
    table.projects.push_back(project_route{
        .slug       = member.slug,
        .root_path  = member.root_path,
        .git_remote = member.git_remote,
        .summary    = summary,
        // Keyed on the SUMMARY being non-empty, not on a README existing —
        // a README holding nothing but a heading yields both fields empty.
        .summary_source      = summary.empty() ? "" : "readme",
        .capabilities        = std::move(capabilities),
        .capabilities_source = has_capabilities ? "static" : "",
        .depends_on          = std::move(deps.deps),
        .depends_on_source   = deps.source,
        .entry_points        = detect_entry_points(root),
        .languages           = count_languages(root, 5000),
        .focus               = *std::move(focus),
    });
  }

  auto plans = query_ids(conn,
                         "select id from plans\n"
                         "where scope_kind = 'association' and scope_id = ?\n"
                         "order by id",
                         org_id);
  if (!plans.has_value()) {
    return std::unexpected(build_error::query_failed);
  }
  auto questions = query_ids(conn,
                             "select id from questions\n"
                             "where scope_kind = 'association' and scope_id = ? and status = 'open'\n"
                             "order by id",
                             org_id);
  if (!questions.has_value()) {
    return std::unexpected(build_error::query_failed);
  }

  std::vector<dependency_edge> edges;
  for (const auto& project : table.projects) {
    const auto reason = reason_for(project.depends_on_source);
    for (const auto& target : project.depends_on) {
      edges.push_back(dependency_edge{.from = project.slug, .to = target, .reason = std::string{reason}});
    }
  }
  std::ranges::sort(
      edges, [](const dependency_edge& a, const dependency_edge& b) { return a.from != b.from ? a.from < b.from : a.to < b.to; });

  table.cross = cross_repo{
      .plans_scoped_to_org     = *std::move(plans),
      .questions_scoped_to_org = *std::move(questions),
      .dependency_edges        = std::move(edges),
  };
  return table;
}

auto encode(const routing_table& table) -> std::string {
  // Member order is the Zig struct's DECLARATION order, which is what
  // `std.json.Stringify` emits and therefore what the file must carry.
  json_dom::json_value projects{.kind = json_dom::json_kind::array};
  for (const auto& project : table.projects) {
    json_dom::json_value languages{.kind = json_dom::json_kind::object};
    for (const auto& [language, share] : project.languages) {
      languages.object.emplace_back(language, json_dom::json_value{.kind = json_dom::json_kind::floating, .floating = share});
    }
    projects.array.push_back(object({
        {"slug", node(project.slug)},
        {"root_path", node(project.root_path)},
        {"git_remote", node(project.git_remote)},
        {"summary", node(project.summary)},
        {"summary_source", node(project.summary_source)},
        {"capabilities", node(std::span{project.capabilities})},
        {"capabilities_source", node(project.capabilities_source)},
        {"depends_on", node(std::span{project.depends_on})},
        {"depends_on_source", node(project.depends_on_source)},
        {"entry_points", node(std::span{project.entry_points})},
        {"languages", std::move(languages)},
        {"planar_focus", object({
                             {"active_plans", node(std::span{project.focus.active_plans})},
                             {"open_tasks", node(project.focus.open_tasks)},
                             {"open_questions", node(project.focus.open_questions)},
                             {"recent_session_ids", node(std::span{project.focus.recent_session_ids})},
                         })},
    }));
  }

  json_dom::json_value edges{.kind = json_dom::json_kind::array};
  for (const auto& edge : table.cross.dependency_edges) {
    edges.array.push_back(object({
        {"from", node(edge.from)},
        {"to", node(edge.to)},
        {"reason", node(edge.reason)},
    }));
  }

  return json_dom::stringify_indent2(object({
      {"schema_version", node(table.schema_version)},
      {"workspace_id", node(table.workspace_id)},
      {"workspace_slug", node(table.workspace_slug)},
      {"workspace_name", node(table.workspace_name)},
      {"generated_at", node(table.generated_at)},
      {"generator_version", node(table.generator_version)},
      {"projects", std::move(projects)},
      {"cross_repo", object({
                         {"plans_scoped_to_org", node(std::span{table.cross.plans_scoped_to_org})},
                         {"questions_scoped_to_org", node(std::span{table.cross.questions_scoped_to_org})},
                         {"dependency_edges", std::move(edges)},
                     })},
  }));
}

auto write_table(const std::filesystem::path& path, const routing_table& table) -> bool {
  if (path.empty()) {
    return false;
  }
  auto body = encode(table);
  body += '\n';

  // The `.tmp` sibling, then a rename — an interrupted build must not leave
  // a truncated routing table where a working one was.
  auto temp = path;
  temp += ".tmp";
  {
    std::ofstream output(temp, std::ios::binary | std::ios::trunc);
    if (!output) {
      return false;
    }
    output.write(body.data(), static_cast<std::streamsize>(body.size()));
    if (!output) {
      return false;
    }
  }
  std::error_code ec;
  std::filesystem::rename(temp, path, ec);
  if (ec) {
    std::error_code cleanup;
    std::filesystem::remove(temp, cleanup);
    return false;
  }
  return true;
}

} // namespace planar::engine::workspace::routing
