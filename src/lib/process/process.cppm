/// @file process.cppm
/// @brief `planar.process` — the layer-1 process-spawn seam (plan 996, task
/// 6272).
///
/// ## Why this module exists, and what it is NOT
///
/// Four separate ports had grown their own subprocess runner before this
/// module existed, and the tree's own comments repeatedly said "there is no
/// process-spawn seam in this tree", which by the time task 6272 acted on it
/// was **false three times over**. What was actually missing was a runner at
/// a layer every consumer can reach, under a name that is about processes
/// rather than about the first caller that happened to need one:
///
///   * `planar.git::run` (layer 1) — `popen`, git-specific, and DELIBERATELY
///     erases the spawn-failure/non-zero-exit distinction ("no answer, never
///     a wrong answer"). It is a probe vocabulary, not a spawn vocabulary,
///     and it stays exactly as it is.
///   * `planar.cmd.planar.editor::spawn_inherit` (layer 3) — `fork`/`execv`
///     with inherited stdio, an injected `env_lookup`, and the
///     spawn-failure/exit distinction preserved. Structurally the right
///     thing, reachable only from `cmd_*`, and named for `$EDITOR`.
///   * `planar.cmd.planar.handlers.ext_adapter_factory::spawn_capture`
///     (layer 3) — `posix_spawnp` with a captured stdout.
///   * `planar.engine.execute`'s `capture_process` — internal linkage on
///     purpose, and must stay that way (that target may never grow a reach
///     into the rest of the tree).
///
/// This module is the second and third of those, moved DOWN to layer 1
/// unchanged in behavior. Both former homes are now thin delegates. Layer 1
/// is forced rather than chosen, for exactly the reason `scope_ref`,
/// `json_text`, `policy` and `git` each record: the next consumer in line is
/// `engine.runtime.sessioncommits` (layer 2), and a layer-2 -> layer-3 edge
/// FATALs at configure time via `cmake/architecture.cmake`.
///
/// ## The two shapes are not interchangeable
///
/// `capture` pipes stdout and discards stderr; `run_inherited` hands the
/// child the caller's own terminal. That is not a tuning knob — a credential
/// subprocess whose stderr reached the operator's terminal would corrupt the
/// verb's output, and a workflow whose stdout was captured would stop
/// streaming. Each consumer needs one and would be wrong with the other.
///
/// ## The spawn-failure/non-zero-exit distinction is operator-visible
///
/// Both shapes keep "the program never ran" separate from "the program ran
/// and failed", because callers render them as different refusals (`gh
/// binary not on PATH` vs `run gh auth login`; `spawning editor:
/// FileNotFound` vs `editor exited with code 1`). Collapsing them is the one
/// change that must never be made here — `/bin/sh` reports a missing program
/// as exit 127, which a real program can also return, and that is precisely
/// why neither shape goes through a shell.
module;

export module planar.process;

import std;

namespace planar::process {

/// @brief An environment lookup callable.
///
/// Structurally identical to (and freely interchangeable with) each
/// binary's `context::env_lookup` and the engine buckets' own aliases — they
/// are all the same `std::function` specialization, so no conversion or
/// adaptation is needed at any call site. It is declared here so this module
/// depends on nothing.
export using env_lookup = std::function<std::optional<std::string>(std::string_view)>;

/// @brief Resolve `program` to an executable absolute-or-relative path.
///
/// A name containing `/` is used as-is; a bare name is searched along the
/// `PATH` reported by `env`. Moved verbatim from
/// `planar.cmd.planar.editor`.
///
/// ## Why the PATH search happens here rather than in `execvp`
///
/// After `fork`, a failed `execvp` can only report itself by `_exit`ing with
/// some code, and any code it picks is a code a real program could also have
/// returned. Resolving BEFORE forking is what lets `run_inherited` report
/// "could not run this at all" as a value distinct from every exit status —
/// which a fallback chain (`$PAGER`, then `less`, then `cat`) requires, and
/// which zig's `std.process.spawn` gets for free by resolving up front.
/// @param env The environment lookup, consulted for `PATH`.
/// @param program The program name or path.
/// @return The resolved path, or unset when nothing executable matched.
export auto resolve_program(const env_lookup& env, std::string_view program) -> std::optional<std::string>;

/// @brief Run `argv` to completion with stdin, stdout and stderr INHERITED.
///
/// The child gets the caller's real terminal. `std::cout` and `std::cerr`
/// are flushed before the fork, because the child inherits this process's
/// descriptors and anything still sitting in this process's buffers would
/// otherwise be written twice.
/// @param env The environment lookup, for resolving `argv[0]`.
/// @param argv The full argument vector; `argv[0]` is the program.
/// @return The child's exit status, or unset when `argv` is empty, `argv[0]`
/// could not be resolved, or the fork failed. Death by signal reports 1,
/// matching the oracle's `else => 1` arm.
export auto run_inherited(const env_lookup& env, std::span<const std::string> argv) -> std::optional<int>;

/// @brief What running a captured subprocess produced.
export struct capture_result {
  bool        spawned   = false; ///< Whether the program was executed at all.
  int         exit_code = 0;     ///< Its exit status; meaningful only when `spawned`.
  std::string output;            ///< Everything it wrote to stdout, untrimmed.
};

/// @brief Run `program` with `args`, capturing stdout and discarding stderr.
///
/// Uses `posix_spawnp`, whose return value carries `ENOENT` directly, so a
/// missing program reports `spawned == false` rather than a shell's exit
/// 127. `program` is resolved by `posix_spawnp` against the INHERITED
/// environment — unlike `run_inherited`, this shape takes no `env_lookup`,
/// because its one consumer (`gh auth token`) must see the operator's real
/// `PATH` and credential environment.
/// @param program The program name, resolved through `PATH`.
/// @param args The arguments AFTER the program name.
/// @return What the child produced.
export auto capture(std::string_view program, std::span<const std::string_view> args) -> capture_result;

/// @brief How many threads this process has right now, or nothing when the
/// platform will not say.
///
/// A `fork` whose child goes on to run more than async-signal-safe calls is
/// safe only when no other thread can hold a lock the child needs (the
/// allocator's, `stdio`'s), so a caller that forks checks that this is 1
/// first. The read is a single small file read or one `proc_pidinfo` call and
/// allocates nothing that outlives it.
///
/// | Platform | Source |
/// |---|---|
/// | macOS | `proc_pidinfo` with `PROC_PIDTASKINFO`, `pti_threadnum` |
/// | Linux | the `Threads:` line of `/proc/self/status` |
/// @return The thread count (at least 1); `std::nullopt` when it cannot be
/// read, which a caller must not read as "one".
export auto own_thread_count() -> std::optional<std::size_t>;

/// @brief Closes every open descriptor of this process at or above 3, except
/// `keep`, however high its number.
///
/// For the child of a `fork` that must hold nothing of its caller's: a reader
/// waiting for end-of-file on a pipe the caller passed down would otherwise
/// wait for as long as this process lives. There is no cap: Linux uses
/// `close_range` over the ranges either side of `keep` (falling back to
/// `/proc/self/fd` when the kernel lacks it), and macOS lists the open
/// descriptors with `proc_pidinfo(PROC_PIDLISTFDS)` and closes each. Only when
/// the open descriptors cannot be listed at all does it close every number up
/// to `sysconf(_SC_OPEN_MAX)`. Descriptors 0 to 2 are left alone. Not
/// thread-safe against a concurrent `open`: call it in a single-threaded
/// child.
/// @param keep The one descriptor to leave open (for example a report pipe);
/// a negative or below-3 value keeps nothing extra.
export void close_descriptors_except(int keep);

} // namespace planar::process
