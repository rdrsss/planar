/// @file git.cpp
/// @brief Implementation of `planar.git`. See the module interface for the
/// "no answer, never a wrong answer" contract and the two-level worktree
/// detection rule.

module;

#include <cstdio>
#include <sys/wait.h>

module planar.git;

import std;

namespace planar::git {

namespace {

/// @brief ASCII whitespace stripped from both ends of every probe's stdout.
/// The same set zig's `std.mem.trim(u8, ..., " \t\r\n")` uses.
constexpr std::string_view k_trim_set = " \t\r\n";

/// @brief Single-quote one argument for the `/bin/sh` line built below.
///
/// Applied unconditionally to every interpolated value so no call site has
/// to argue about whether its own arguments could contain a space or a
/// quote. A single quote inside the value is closed, escaped and reopened
/// (`'\''`), which is the only escape `sh` honours inside single quotes.
/// @param value The argument.
/// @return The single-quoted form.
auto shell_quote(std::string_view value) -> std::string {
  std::string quoted = "'";
  for (char const c : value) {
    if (c == '\'') {
      quoted += "'\\''";
    } else {
      quoted += c;
    }
  }
  quoted += "'";
  return quoted;
}

/// @brief Trim `k_trim_set` from both ends.
/// @param raw The string to trim.
/// @return The trimmed view into `raw`.
auto trim(std::string_view raw) -> std::string_view {
  auto const begin = raw.find_first_not_of(k_trim_set);
  if (begin == std::string_view::npos) {
    return {};
  }
  auto const end = raw.find_last_not_of(k_trim_set);
  return raw.substr(begin, end - begin + 1);
}

} // namespace

auto run(const std::filesystem::path& dir, std::span<const std::string_view> args) -> std::optional<std::string> {
  std::string line = "git -C " + shell_quote(dir.string());
  for (auto const& arg : args) {
    line += " " + shell_quote(arg);
  }
  // See the module header: a probe's stderr must not interleave with the
  // verb's real output on the operator's terminal.
  line += " 2>/dev/null";

  std::FILE* pipe = ::popen(line.c_str(), "r");
  if (pipe == nullptr) {
    return std::nullopt;
  }
  std::string           output;
  std::array<char, 512> buffer{};
  while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
    output.append(buffer.data());
  }
  int const status = ::pclose(pipe);
  // A failure to reap, a signal death, and a non-zero exit are the three
  // arms of the oracle's own `switch (result.term)` — all "no answer".
  if (status == -1 || WIFEXITED(status) == 0 || WEXITSTATUS(status) != 0) {
    return std::nullopt;
  }
  return output;
}

auto run_trimmed(const std::filesystem::path& dir, std::span<const std::string_view> args) -> std::optional<std::string> {
  auto raw = run(dir, args);
  if (!raw.has_value()) {
    return std::nullopt;
  }
  auto const trimmed = trim(*raw);
  if (trimmed.empty()) {
    return std::nullopt;
  }
  return std::string{trimmed};
}

auto probe_start_context(const std::filesystem::path& dir) -> std::optional<start_context> {
  static constexpr std::array<std::string_view, 2> k_toplevel{"rev-parse", "--show-toplevel"};
  static constexpr std::array<std::string_view, 2> k_head{"rev-parse", "HEAD"};

  auto repo_root = run_trimmed(dir, k_toplevel);
  if (!repo_root.has_value()) {
    return std::nullopt;
  }
  // Deliberately sequenced, not combined: the oracle bails on the FIRST
  // failure, so a repository with no commits (toplevel answers, HEAD does
  // not) stamps neither column rather than one.
  auto head_sha = run_trimmed(dir, k_head);
  if (!head_sha.has_value()) {
    return std::nullopt;
  }
  return start_context{.repo_root = std::move(*repo_root), .head_sha_at_start = std::move(*head_sha)};
}

auto probe_origin_url(const std::filesystem::path& dir) -> std::optional<std::string> {
  static constexpr std::array<std::string_view, 3> k_args{"remote", "get-url", "origin"};
  return run_trimmed(dir, k_args);
}

auto fast_path_worktree_root(std::string_view path) -> std::optional<std::string_view> {
  // BOTH slashes are required: the leading one so a directory literally
  // named `something.worktrees` does not match, the trailing one so we know
  // a segment follows.
  constexpr std::string_view k_needle = "/.worktrees/";
  auto const                 idx      = path.find(k_needle);
  if (idx == std::string_view::npos) {
    return std::nullopt;
  }
  auto const after = idx + k_needle.size();
  if (after >= path.size()) {
    return std::nullopt; // `.worktrees/` with no segment after it.
  }
  // Up to AND INCLUDING the first segment under `.worktrees/`. The
  // methodology's two-level `<plan>/<task>` convention still classifies the
  // cwd as a worktree, which is all the gate needs; the root we report is
  // the first segment, which also covers the single-segment hand-picked
  // case.
  auto const rel        = path.substr(after);
  auto const next_slash = rel.find('/');
  auto const end        = next_slash == std::string_view::npos ? path.size() : after + next_slash;
  return path.substr(0, end);
}

namespace {

/// @brief Given `<parent>/.worktrees/<seg>`, recover `<parent>`.
/// @param worktree_root The fast path's worktree root.
/// @return The parent repository root.
auto parent_of_dot_worktrees(std::string_view worktree_root) -> std::string_view {
  constexpr std::string_view k_marker = "/.worktrees/";
  auto const                 idx      = worktree_root.find(k_marker);
  if (idx == std::string_view::npos) {
    return worktree_root;
  }
  return worktree_root.substr(0, idx);
}

} // namespace

auto detect_worktree(const std::filesystem::path& dir) -> worktree_detection {
  auto const path = dir.string();

  // ---- Level 1: the `.worktrees/<seg>` convention, no subprocess. ----
  if (auto const root = fast_path_worktree_root(path)) {
    return worktree_detection{
        .is_worktree      = true,
        .worktree_root    = std::string{*root},
        .parent_repo_root = std::string{parent_of_dot_worktrees(*root)},
    };
  }

  // ---- Level 2: ask git. One invocation, three lines out. ----
  static constexpr std::array<std::string_view, 5> k_args{
      "rev-parse", "--path-format=absolute", "--show-toplevel", "--git-common-dir", "--absolute-git-dir",
  };
  auto const raw = run(dir, k_args);
  if (!raw.has_value()) {
    return worktree_detection{};
  }

  std::array<std::string_view, 3> lines{};
  std::size_t                     count  = 0;
  std::size_t                     cursor = 0;
  std::string_view const          view{*raw};
  while (count < lines.size() && cursor <= view.size()) {
    auto const nl      = view.find('\n', cursor);
    auto const segment = trim(view.substr(cursor, nl == std::string_view::npos ? std::string_view::npos : nl - cursor));
    if (!segment.empty()) {
      lines[count] = segment;
      ++count;
    }
    if (nl == std::string_view::npos) {
      break;
    }
    cursor = nl + 1;
  }
  if (count < lines.size()) {
    // Fewer than three non-empty lines: git answered something this port
    // does not understand. Not a worktree.
    return worktree_detection{};
  }
  auto const toplevel   = lines[0];
  auto const common_dir = lines[1];
  auto const git_dir    = lines[2];

  // The ONLY case where the per-worktree git dir differs from the
  // repository's common dir is a linked worktree — see the interface's note
  // on why the older `<toplevel>/.git` comparison misclassified submodules.
  if (git_dir == common_dir) {
    return worktree_detection{};
  }

  // The parent repo root is the directory CONTAINING the common dir's
  // `.git`, i.e. the common dir's parent.
  std::filesystem::path const common_path{common_dir};
  auto const                  parent = common_path.parent_path();
  if (parent.empty()) {
    return worktree_detection{};
  }

  return worktree_detection{
      .is_worktree      = true,
      .worktree_root    = std::string{toplevel},
      .parent_repo_root = parent.string(),
  };
}

} // namespace planar::git
