/// @file harvest.cpp
/// @brief Implementation of `planar.engine.runs.harvest` (plan 996, task
/// 6362). See harvest.cppm for the oracle-derived shape and the DISTINCT
/// discipline.

module planar.engine.runs.harvest;

import std;
import planar.db;
import planar.git;
import planar.engine.runs.lifecycle;

namespace planar::engine::runs::harvest {

namespace life = lifecycle;

namespace {

/// @brief ASCII whitespace trimmed from each line, matching the oracle's
/// `std.mem.trim(u8, line, " \t\r")`. Deliberately NOT `\n` — the caller
/// already split on it.
constexpr std::string_view k_trim_set = " \t\r";

auto trim_line(std::string_view line) -> std::string_view {
  auto const begin = line.find_first_not_of(k_trim_set);
  if (begin == std::string_view::npos) {
    return {};
  }
  auto const end = line.find_last_not_of(k_trim_set);
  return line.substr(begin, end - begin + 1);
}

/// @brief Split `raw` on `\n`, trim each line, drop blanks, and append the
/// first-seen occurrence of each distinct path to `out` / `seen`.
///
/// Shared by both the single-query (range mode) and two-query (working-tree
/// mode) paths so the DISTINCT rule — first occurrence wins, across calls —
/// is one piece of code rather than two copies that could drift.
/// @param raw One query's raw stdout.
/// @param seen The path set accumulated so far (mutated).
/// @param out The ordered output (mutated).
auto append_distinct_lines(std::string_view raw, std::set<std::string, std::less<>>& seen, std::vector<std::string>& out)
    -> void {
  std::size_t cursor = 0;
  while (cursor <= raw.size()) {
    auto const nl      = raw.find('\n', cursor);
    auto const segment = raw.substr(cursor, nl == std::string_view::npos ? std::string_view::npos : nl - cursor);
    auto const path    = trim_line(segment);
    if (!path.empty() && !seen.contains(path)) {
      seen.emplace(path);
      out.emplace_back(path);
    }
    if (nl == std::string_view::npos) {
      break;
    }
    cursor = nl + 1;
  }
}

} // namespace

auto parse_paths(std::string_view raw) -> std::vector<std::string> {
  std::set<std::string, std::less<>> seen;
  std::vector<std::string>           out;
  append_distinct_lines(raw, seen, out);
  return out;
}

auto diff_paths(const std::filesystem::path& worktree, const diff_spec& spec)
    -> std::expected<std::vector<std::string>, harvest_error> {
  std::vector<std::string> argv{"diff", "--name-only"};
  if (spec.has_value()) {
    argv.push_back(std::format("{}..{}", spec->base, spec->head));
  } else {
    // HEAD captures both staged and unstaged changes against the last
    // commit — the full uncommitted change set, not just unstaged.
    argv.emplace_back("HEAD");
  }
  std::vector<std::string_view> const argv_view(argv.begin(), argv.end());

  auto const raw = git::run(worktree, argv_view);
  if (!raw.has_value()) {
    return std::unexpected(harvest_error::git_failed);
  }

  std::set<std::string, std::less<>> seen;
  std::vector<std::string>           out;
  append_distinct_lines(*raw, seen, out);

  // Working-tree mode ALSO needs untracked (never-staged) new files, which
  // `git diff --name-only HEAD` cannot see — see this module's header.
  if (!spec.has_value()) {
    static constexpr std::array<std::string_view, 3> k_untracked_args{"ls-files", "--others", "--exclude-standard"};
    auto const                                       untracked_raw = git::run(worktree, k_untracked_args);
    if (!untracked_raw.has_value()) {
      return std::unexpected(harvest_error::git_failed);
    }
    append_distinct_lines(*untracked_raw, seen, out);
  }

  return out;
}

auto harvest(db::connection& conn, const harvest_args& args) -> std::expected<std::size_t, harvest_error> {
  auto const paths = diff_paths(args.worktree, args.spec);
  if (!paths.has_value()) {
    return std::unexpected(paths.error());
  }

  for (auto const& path : *paths) {
    auto const written = life::touch_idempotent(conn, args.run_id, args.task_id, path, life::touch_kind::actual);
    if (!written.has_value()) {
      return std::unexpected(harvest_error::query_failed);
    }
  }

  return paths->size();
}

} // namespace planar::engine::runs::harvest
