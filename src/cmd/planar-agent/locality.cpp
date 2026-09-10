/// @file locality.cpp
/// @brief Implementation of `planar.cmd.planar_agent.locality`. See the
/// module interface for the never-fails contract and the three commands.

module planar.cmd.planar_agent.locality;

import std;
import planar.git;
import planar.engine.runtime.agentactivity;

namespace planar::cmd::agent {

namespace aa  = engine::runtime::agentactivity;
namespace git = planar::git;

auto probe_locality(const std::filesystem::path& repo_root) -> engine::runtime::agentactivity::locality {
  aa::locality out;
  // Captured before any subprocess: a claim taken outside a checkout still
  // records where it was taken.
  out.repo_root = repo_root.string();

  static constexpr std::array<std::string_view, 3> k_branch_args{"symbolic-ref", "--short", "HEAD"};
  static constexpr std::array<std::string_view, 2> k_head_args{"rev-parse", "HEAD"};
  static constexpr std::array<std::string_view, 2> k_status_args{"status", "--porcelain"};

  out.branch   = git::run_trimmed(repo_root, k_branch_args);
  out.head_sha = git::run_trimmed(repo_root, k_head_args);

  if (!out.head_sha.has_value()) {
    // Not a checkout. Whatever `symbolic-ref` said cannot be trusted, so
    // discard it rather than store a branch name for a repository that
    // does not exist. See the module header.
    out.branch = std::nullopt;
    out.dirty  = aa::dirty_state::unknown;
    return out;
  }

  auto const porcelain = git::run(repo_root, k_status_args);
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
