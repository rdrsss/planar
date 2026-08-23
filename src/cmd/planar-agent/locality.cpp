/// @file locality.cpp
/// @brief Implementation of `planar.cmd.planar_agent.locality`. See the
/// module interface for the never-fails contract and the three commands.

module;

#include <cstdio>
#include <sys/wait.h>

module planar.cmd.planar_agent.locality;

import std;
import planar.engine.runtime.agentactivity;

namespace planar::cmd::agent {

namespace aa = engine::runtime::agentactivity;

namespace {

/// @brief Single-quote one argument for the `/bin/sh` line below.
///
/// Every interpolated value here is a filesystem path that can contain
/// spaces and quotes, so nothing reaches the shell unquoted. The
/// subcommand words are literals in this file and are quoted for
/// uniformity rather than necessity.
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

/// @brief Run `git -C <root> <args...>` and return its stdout on exit 0.
///
/// Stderr is routed to `/dev/null`: the probe is best-effort and any noise
/// it emitted would land on the operator's terminal interleaved with the
/// verb's real output, which is worse than silence. A non-zero exit, a
/// signal, or a failure to spawn at all are all "no answer".
/// @param root The `-C` directory.
/// @param args The git subcommand and its arguments.
/// @return The raw stdout, or unset.
auto run_git(const std::filesystem::path& root, std::span<const std::string_view> args) -> std::optional<std::string> {
  std::string line = "git -C " + shell_quote(root.string());
  for (auto const& arg : args) {
    line += " " + shell_quote(arg);
  }
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
  if (status == -1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    return std::nullopt;
  }
  return output;
}

/// @brief `run_git`, trimmed, with empty output treated as no answer.
/// @param root The `-C` directory.
/// @param args The git subcommand and its arguments.
/// @return The trimmed stdout, or unset.
auto run_git_trim(const std::filesystem::path& root, std::span<const std::string_view> args) -> std::optional<std::string> {
  auto raw = run_git(root, args);
  if (!raw.has_value()) {
    return std::nullopt;
  }
  constexpr std::string_view k_space = " \t\r\n";
  auto const                 begin   = raw->find_first_not_of(k_space);
  if (begin == std::string::npos) {
    return std::nullopt;
  }
  auto const end = raw->find_last_not_of(k_space);
  return raw->substr(begin, end - begin + 1);
}

} // namespace

auto probe_locality(const std::filesystem::path& repo_root) -> engine::runtime::agentactivity::locality {
  aa::locality out;
  // Captured before any subprocess: a claim taken outside a checkout still
  // records where it was taken.
  out.repo_root = repo_root.string();

  static constexpr std::array<std::string_view, 3> k_branch_args{"symbolic-ref", "--short", "HEAD"};
  static constexpr std::array<std::string_view, 2> k_head_args{"rev-parse", "HEAD"};
  static constexpr std::array<std::string_view, 2> k_status_args{"status", "--porcelain"};

  out.branch   = run_git_trim(repo_root, k_branch_args);
  out.head_sha = run_git_trim(repo_root, k_head_args);

  if (!out.head_sha.has_value()) {
    // Not a checkout. Whatever `symbolic-ref` said cannot be trusted, so
    // discard it rather than store a branch name for a repository that
    // does not exist. See the module header.
    out.branch = std::nullopt;
    out.dirty  = aa::dirty_state::unknown;
    return out;
  }

  auto const porcelain = run_git(repo_root, k_status_args);
  if (!porcelain.has_value()) {
    out.dirty = aa::dirty_state::unknown;
    return out;
  }
  constexpr std::string_view k_space = " \t\r\n";
  auto const                 begin   = porcelain->find_first_not_of(k_space);
  out.dirty                          = begin == std::string::npos ? aa::dirty_state::clean : aa::dirty_state::dirty;
  return out;
}

auto resolve_locality(const std::optional<std::string>& repo_root_arg, const std::filesystem::path& cwd, bool skip)
    -> engine::runtime::agentactivity::locality {
  if (skip) {
    return aa::locality{};
  }
  if (repo_root_arg.has_value()) {
    // An operator-supplied `--repo-root` is passed through VERBATIM — the
    // Zig original resolves nothing here, and a claim should record the
    // path the operator named.
    return probe_locality(std::filesystem::path{*repo_root_arg});
  }
  // The cwd fallback, by contrast, IS canonicalised, and the asymmetry is
  // the oracle's: `currentDirAlloc` calls `realPathFileAlloc`, which
  // resolves symlinks. It is observable — on this platform `/tmp` is a
  // symlink to `/private/tmp`, so a claim taken in a scratch directory
  // records `/private/tmp/...` while `$PWD` says `/tmp/...`. A parity diff
  // caught this port emitting the unresolved form.
  //
  // Deliberately NOT the same choice `context::operator_cwd` makes for
  // SCOPE resolution, which stays PWD-first on purpose (see its own doc
  // comment): there the operator's mental model of where they are is the
  // right answer, while here the point is to name one checkout
  // unambiguously across processes that may have reached it by different
  // paths.
  std::error_code ec;
  auto const      resolved = std::filesystem::canonical(cwd, ec);
  return probe_locality(ec ? cwd : resolved);
}

} // namespace planar::cmd::agent
