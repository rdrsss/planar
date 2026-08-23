/// @file locality.cppm
/// @brief `planar.cmd.planar_agent.locality` — the best-effort git snapshot
/// recorded on a claim or action (plan 996, task 6038).
///
/// Port target: zig/src/engine/runtime/agentactivity/locality.zig plus
/// `zig/src/cmd/planar-agent/handlers/util.zig`'s `resolveLocality`.
///
/// ## Why this lives at layer 3 and not in the engine bucket
///
/// Because it is the only part of the agent plane that leaves the process,
/// and the engine bucket is better without a process-spawn dependency
/// wired into it. `planar.engine.runtime.agentactivity` takes a
/// `locality` VALUE; this module is what produces one. The Zig tree puts
/// the probe in the engine because the engine there already carries an
/// `std.Io` handle; nothing in this tree does, and inventing a spawn
/// abstraction inside a layer-2 bucket would have been a much larger
/// change than the three commands it serves.
///
/// ## Opportunistic, never a gate
///
/// EVERY failure maps to "unknown", never to a refusal. Not a git
/// checkout, `git` not installed, a permission error, a hung binary — all
/// of them yield NULL columns and `dirty = unknown`, and the claim still
/// succeeds. That is the tech spec's pinned contract: locality is context
/// for a human reading `planar-watch`, not a synchronisation input. A
/// probe that could fail a claim would make `planar-agent pull` depend on
/// git being installed, which it must not.
///
/// ## The three commands, and the one asymmetry between them
///
///     git -C <root> symbolic-ref --short HEAD   -> branch
///     git -C <root> rev-parse HEAD              -> head_sha
///     git -C <root> status --porcelain          -> dirty
///
/// `symbolic-ref` exits 1 on a detached HEAD, which is NOT an error — it
/// means "no branch name", and `branch` stays NULL. But a failing
/// `rev-parse` means this is not a checkout at all, and in that case the
/// branch we may have just read is meaningless: it is DISCARDED and the
/// whole snapshot degrades to unknown. Reproduced exactly; dropping that
/// second step would let a nonsense branch name reach the database.
///
/// `repo_root` is captured unconditionally whenever the probe runs at all
/// — before any subprocess — so a claim taken outside a checkout still
/// records where it was taken. That is why the oracle's claim rows in a
/// non-git scratch directory carry a `repo_root` alongside three NULLs.
module;

export module planar.cmd.planar_agent.locality;

import std;
import planar.engine.runtime.agentactivity;

namespace planar::cmd::agent {

/// @brief Run the three git probes against `repo_root`.
///
/// Never fails. See this file's header for the per-command semantics.
/// @param repo_root Absolute path of the checkout to probe.
/// @return The snapshot; `repo_root` is always populated.
export auto probe_locality(const std::filesystem::path& repo_root) -> engine::runtime::agentactivity::locality;

/// @brief Resolve the locality for one verb invocation.
///
/// The flag contract, in order:
///
///   `skip` true         -> the all-unknown snapshot; NO subprocess runs
///   `--repo-root <path>` -> probe there
///   otherwise            -> probe the operator's working directory
///
/// `skip` is the CALLER's composite decision, not just `--no-locality-probe`:
/// pull and `action start` also skip when the action kind's
/// `probe_default` is false (heartbeat, tool_call), because a heartbeat
/// every few minutes must not fork `git` three times.
/// @param repo_root_arg The `--repo-root` value, or unset.
/// @param cwd The operator's working directory, used when `--repo-root` is absent.
/// @param skip When true, return the all-unknown snapshot without probing.
/// @return The snapshot.
export auto resolve_locality(const std::optional<std::string>& repo_root_arg, const std::filesystem::path& cwd, bool skip)
    -> engine::runtime::agentactivity::locality;

} // namespace planar::cmd::agent
