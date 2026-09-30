// @file parity_harness.hpp
// @brief The differential-test harness every `src/cmd/<binary>/parity.t.cpp`
// shares: run a built C++ binary and its Zig reference over identical argv
// in identical pinned scratch environments and compare stdout, stderr and
// exit code (plan 996, task 6107).
//
// ## Why this is a HEADER and not a module or a library
//
// Because a library would be illegal and a module would be pointless.
//
// D18 forbids a `cmd_* -> cmd_*` edge; cmake/architecture.cmake FATALs at
// configure time on one. So the four binaries cannot share layer-3
// vocabulary through a target — not their `context`, not their `exit`, not
// their dispatch, and not this. A shared layer-1 library would be legal by
// the layer numbers but wrong twice over: it would put test-only scaffolding
// in the shipped base-library layer, and — for THIS file specifically — it
// would drag `planar_db` (which the safety guard below needs) into the link
// closure of `planar-execute`'s test target, whose whole invariant is that
// it reaches no SQLite handle. cmake/architecture.cmake would FATAL on that
// too, via the execute-carrier check.
//
// A plain header included by each `parity.t.cpp` creates NO target edge:
// each test target compiles its own copy, exactly as if the code had been
// pasted there, which is what task 6105's `parity.t.cpp` did and what three
// more copies would otherwise do. Nothing that ships includes it.
//
// The one thing that IS deduplicated here is the part that was already
// gotten wrong once and must never be gotten wrong again — the pinned
// environment. See `run_pinned`.
//
// The SKIP-when-the-oracle-is-absent posture this header used to describe is
// GONE with its subject: `zig/` was deleted at the M10 cutover (task 6045)
// and `PLANAR_REQUIRE_ORACLE` / `parity_strict.hpp` went with it. Nothing
// here skips any more, and the expected ctest skip tally is ZERO — a nonzero
// one means a NEW skip was introduced, not an unbuilt oracle.
#pragma once

#include <sys/wait.h>

// This header is included from module-importing test translation units, so
// it must not `#include` any standard library header — `import std;` at the
// top of the includer provides everything used below.

namespace planar::cmd::parity {

/// @brief One binary's observable output for one invocation.
struct capture {
  int         code = 0; ///< The process exit status.
  std::string out;      ///< Everything written to stdout.
  std::string err;      ///< Everything written to stderr.
};

/// @brief Read a whole file as bytes, or the empty string when absent.
/// @param path The file to read.
/// @return The file's contents.
inline auto read_all(const std::filesystem::path& path) -> std::string {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return {};
  }
  std::ostringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

/// @brief Shell-quote one argument for the `/bin/sh` line built below.
/// @param value The argument.
/// @return The single-quoted form.
inline auto shell_quote(std::string_view value) -> std::string {
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

/// @brief One environment variable a pinned invocation carries.
struct pinned_var {
  std::string name;  ///< The variable name.
  std::string value; ///< Its value, an absolute path under the arena.
  /// @brief When true the child runs with `name` REMOVED from its environment
  /// (`env -u name`) and `value` is ignored. A variable merely absent from the
  /// map is inherited, not removed; only this makes it unset.
  bool unset = false;
};

/// @brief The variables every pinned invocation runs under, all rooted in
/// `work`.
///
/// EXTRACTED, NOT COPIED (plan 996, task 6547). `run_pinned` below and
/// `launch_pinned_detached` further down must pin the SAME set of roots, and
/// the whole point of this header's existence is that the set lives in one
/// place. A second spelling of it is the exact defect the `run_pinned`
/// comment above spends forty lines warning about: a redirect that misses one
/// root writes into the operator's live state while every assertion still
/// passes. So the map is built here once and both entry points consume it.
///
/// `PLANAR_AGENT_DB` (plan 1080, task 6996; decision 1181) is the agent
/// database, `src/lib/db/agentdb.cppm`'s open path, which migrates its file
/// on first use exactly as `PLANAR_DB` does. It is pinned DIRECTLY, not via
/// `HOME`, for the reason the workbench root is: the runtime's fallback is
/// `$HOME/.planar/agent.db`, so a scratch `HOME` contains it only
/// incidentally, and `agent_db_pin_error` below fails a case whose map ever
/// lets it resolve outside the arena.
/// @param work The scratch root.
/// @return The variables, in a fixed order.
inline auto pinned_env(const std::filesystem::path& work) -> std::vector<pinned_var> {
  return {
      pinned_var{.name = "PLANAR_DB", .value = (work / "planar.db").string()},
      pinned_var{.name = "PLANAR_AGENT_DB", .value = (work / "agent.db").string()},
      pinned_var{.name = "PLANAR_HOME", .value = (work / "home").string()},
      pinned_var{.name = "PLANAR_CONFIG_PATH", .value = (work / "config.toml").string()},
      pinned_var{.name = "PLANAR_LOCAL_HOME", .value = (work / "localhome").string()},
      pinned_var{.name = "PLANAR_WORKBENCH_ROOT", .value = (work / "workbench").string()},
      pinned_var{.name = "HOME", .value = (work / "fakehome").string()},
      pinned_var{.name = "PWD", .value = (work / "proj").string()},
  };
}

/// @brief Look one variable up in a pinned map.
/// @param env The map.
/// @param name The variable name.
/// @return Its value, or nullopt when absent.
inline auto pinned_lookup(std::span<const pinned_var> env, std::string_view name) -> std::optional<std::string> {
  for (auto const& var : env) {
    if (var.name == name && !var.unset) {
      return var.value;
    }
  }
  return std::nullopt;
}

/// @brief Whether a pinned map explicitly removes `name` from the child's
/// environment (an entry with `unset` set).
/// @param env The map.
/// @param name The variable name.
/// @return `true` only for an explicit removal; mere absence is inheritance.
inline auto pinned_unsets(std::span<const pinned_var> env, std::string_view name) -> bool {
  return std::ranges::any_of(env, [&](const pinned_var& var) { return var.name == name && var.unset; });
}

/// @brief Whether `path` lies under `root`, compared lexically.
/// @param root The directory.
/// @param path The candidate, which must be absolute to qualify.
/// @return `true` only for an absolute path at or below `root`.
inline auto lexically_under(const std::filesystem::path& root, const std::filesystem::path& path) -> bool {
  if (!path.is_absolute()) {
    return false;
  }
  auto const rel = path.lexically_normal().lexically_relative(root.lexically_normal());
  if (rel.empty()) {
    return false;
  }
  auto const first = rel.begin()->string();
  return first != "..";
}

/// @brief The agent-database safety check: resolve the path the runtime
/// would open from `env`, by the runtime's own rule (`PLANAR_AGENT_DB` when
/// set, refused when set but empty, else `$HOME/.planar/agent.db`, see
/// `src/lib/db/agentdb.cppm`), and report it when it is not under `work`.
///
/// This mirrors the rule rather than importing `planar.db.agentdb`, because
/// this header is included by `planar-execute`'s test target, whose
/// invariant is that it reaches no SQLite handle (see the header comment).
/// @param work The arena root.
/// @param env The variables the invocation would run under.
/// @return A diagnostic naming the offending path, or nullopt when pinned.
inline auto agent_db_pin_error(const std::filesystem::path& work, std::span<const pinned_var> env) -> std::optional<std::string> {
  std::filesystem::path resolved;
  std::string_view      source;
  if (auto const direct = pinned_lookup(env, "PLANAR_AGENT_DB"); direct.has_value() && direct->empty()) {
    // `resolve_agent_db_path` refuses a variable that is set and empty; it
    // does not fall back to HOME. A harness that fell back would call a run
    // pinned that the binary itself refuses (task 7043).
    return std::format("parity harness: PLANAR_AGENT_DB is set but empty for arena root '{}'; the runtime refuses that "
                       "rather than falling back to $HOME/.planar/agent.db",
                       work.string());
  } else if (direct.has_value()) {
    resolved = *direct;
    source   = "PLANAR_AGENT_DB";
  } else if (auto const home = pinned_lookup(env, "HOME"); home.has_value() && !home->empty()) {
    resolved = std::filesystem::path{*home} / ".planar" / "agent.db";
    source   = "the $HOME/.planar/agent.db fallback";
  } else if (pinned_unsets(env, "PLANAR_AGENT_DB") && pinned_unsets(env, "HOME")) {
    // Both variables are REMOVED from the child's environment, so the runtime
    // has nothing to resolve a path from and refuses (`resolve_agent_db_path`
    // has no passwd-database fallback). This is the one deliberately
    // unresolvable map; mere absence from the map is still refused below,
    // because an absent variable is inherited from the caller.
    return std::nullopt;
  } else {
    return std::format("parity harness: the agent database is unpinned (neither PLANAR_AGENT_DB nor HOME is in the "
                       "pinned map) for arena root '{}'; a from-source binary would open the operator's live "
                       "~/.planar/agent.db",
                       work.string());
  }
  if (lexically_under(work, resolved)) {
    return std::nullopt;
  }
  return std::format("parity harness: the agent database resolves to '{}' via {}, outside the arena root '{}'; a "
                     "from-source binary would migrate a store the arena does not own",
                     resolved.string(), source, work.string());
}

/// @brief Fail the calling case, before any binary starts, when
/// `agent_db_pin_error` reports a path outside the arena.
///
/// This throws rather than `REQUIRE`s because nine of the thirteen includers
/// include this header BEFORE `catch_test_macros.hpp`, so the Catch2 macros
/// are not visible here. Catch2 reports an uncaught `std::runtime_error` as
/// a failed case with the message, which is the same loud failure.
/// @param work The arena root.
/// @param env The variables the invocation would run under.
inline void require_agent_db_pinned(const std::filesystem::path& work, std::span<const pinned_var> env) {
  if (auto const problem = agent_db_pin_error(work, env)) {
    throw std::runtime_error(*problem);
  }
}

/// @brief The `env VAR=... ` prefix a pinned invocation runs behind.
///
/// It first removes `PLANAR_QUEUE_SLOT` (task 7061): a suite run under
/// `planar-agent queue run -- make test` inherits the marker, and every arena
/// submitter would then look like a nested run of that entry. The removal
/// comes before the assignments, so a map that names the marker still
/// delivers it.
/// @param env The variables, normally `pinned_env(work)`.
/// @return A shell fragment ending in a trailing space, ready for a binary.
inline auto pinned_env_prefix(std::span<const pinned_var> env) -> std::string {
  std::string child = "env -u PLANAR_QUEUE_SLOT";
  // Options first: `env` stops reading options at the first assignment.
  for (auto const& var : env) {
    if (var.unset) {
      child += std::format(" -u {}", shell_quote(var.name));
    }
  }
  for (auto const& var : env) {
    if (!var.unset) {
      child += std::format(" {}={}", var.name, shell_quote(var.value));
    }
  }
  child += ' ';
  return child;
}

/// @brief The `env VAR=... ` prefix for the default pinned map of `work`.
/// @param work The scratch root.
/// @return A shell fragment ending in a trailing space, ready for a binary.
inline auto pinned_env_prefix(const std::filesystem::path& work) -> std::string {
  return pinned_env_prefix(pinned_env(work));
}

/// @brief Run `bin` with `args` inside `work`, under an environment pinned
/// entirely to `work`.
///
/// DATABASE SAFETY IS THE WHOLE REASON THIS FUNCTION EXISTS IN ONE PLACE.
/// `planar`, `planar-agent` and `planar-watch` all resolve `$PLANAR_DB` and
/// otherwise fall back to `~/.planar/planar.db`; `planar` and the Zig
/// runtime's writable bootstraps apply pending migrations AUTOMATICALLY on
/// first use. Shelling any of them with the inherited environment therefore
/// opens the operator's live database — and the moment a migration lands on
/// this branch, `ctest` would migrate it past the version every installed
/// binary supports and lock every other agent on the machine out. That is
/// not hypothetical: commit 3ec6c37 fixed exactly this defect in two cli
/// parity tests.
///
/// EVERY REDIRECTABLE ROOT IS REDIRECTED DIRECTLY, NOT VIA `HOME`. The
/// workbench root resolves as `$PLANAR_WORKBENCH_ROOT` → `workbench.root`
/// in the config → `~/.planar/workbench/`. Before task 6305 this function
/// set only the first four `PLANAR_*` vars, so a workbench-touching parity
/// case landed in `$HOME/.planar/workbench` and was contained solely by
/// `HOME` also being redirected. That containment is real but INCIDENTAL:
/// it survives only as long as nobody reorders or trims the env map, and
/// the failure mode when it breaks is a parity case writing into the
/// operator's live workbench while every assertion still passes. Pin the
/// root that the runtime actually consults first, so containment does not
/// depend on a fallback chain.
///
/// `cd` FIRST, then `env` — and never the other way round. The obvious
/// spelling, `VAR=x cd dir && binary`, silently does NOT export the
/// assignments to `binary` on this platform's `/bin/sh` (verified:
/// `sh -c "FOO=bar cd /tmp && env | grep FOO"` prints nothing). Task 6105's
/// parity file was written that way first, and BOTH binaries fell back to
/// the inherited HOME and wrote ten annotation rows into the operator's live
/// `~/.planar/planar.db` before a mismatch in row ids gave it away.
///
/// That failure was also nearly missed, because the first probe for it —
/// comparing the live database file's mtime before and after — reported
/// "untouched". Under WAL, writes land in the `-wal` sibling and the main
/// `.db` file's mtime does not move. An mtime check on the main file is NOT
/// a valid liveness probe here; a content hash or a row count is.
///
/// THE ZIG RUNTIME'S FILE WRITER USES POSITIONAL WRITES, AND THAT IS A
/// PER-WRITE HAZARD, NOT A PER-INVOCATION ONE.
///
/// The original form of this function redirected each stream straight into
/// a file (`binary … > out 2> err`) and defended the hazard by giving every
/// invocation its own freshly-created capture file, on the theory that the
/// collision was between two Zig processes appending to a shared target.
/// That theory is incomplete. Two writes from ONE Zig process to ONE file
/// collide the same way: the second `pwrite` starts at offset 0 and
/// overwrites the first. Measured on `planar task add --plan <dangling>`,
/// which writes stderr twice:
///
///   truth (pipe):  "error: task.create exec failed: StepFailed\n
///                   error: task add: QueryFailed\n"          (72 bytes)
///   file redirect: "error: task add: QueryFailed\nd: StepFailed\n"
///                                                            (43 bytes)
///
/// The second line landed at offset 0 and ate the first, leaving the tail
/// of the longer message dangling as `d: StepFailed`. Every oracle stderr
/// this harness captured from a multi-write invocation was therefore
/// CORRUPT — silently, and in a way that reads as a real divergence.
/// (Found by task 6198's state differential, whose staged expectation for
/// task 6202 would otherwise have pinned the corrupted bytes as truth.)
///
/// IT IS NOT STDERR-ONLY, AND IT REACHES ACROSS INVOCATIONS. Task 6258
/// measured the same corruption on STDOUT, and — worse — through a shared
/// capture FILE rather than within one invocation: a later unpiped oracle
/// call rewound the file and ate everything written before it, so an
/// earlier probe's output vanished entirely rather than merely truncating.
/// So the rule is not "pipe stderr on multi-write verbs". It is: EVERY
/// oracle invocation goes through a pipe, both streams, always. A capture
/// that reads as empty or short is the expected symptom, and it looks
/// exactly like a verb that legitimately printed nothing.
///
/// The fix is to hand the child a PIPE rather than a seekable file: a pipe
/// has no offset to seek to, so the Zig writer falls back to sequential
/// writes. Each stream is piped through `cat`, which owns the file. The
/// child's exit status is written to a third file rather than read from
/// `std::system`, because the shell now reports the status of `cat`.
///
/// The per-invocation `tag` still keys all three files, and must keep doing
/// so — concurrent Catch2 cases share `work`.
/// THE AGENT DATABASE IS CHECKED BEFORE THE BINARY STARTS (plan 1080, task
/// 6996). `require_agent_db_pinned` resolves the agent database from the map
/// the child will run under and fails the case when it is outside `work`.
/// The `env` overload exists so the harness's own cases can hand it a map
/// that points outside and prove the refusal fires; every ordinary caller
/// uses the four-argument form, which pins `pinned_env(work)`.
/// @param bin The binary to run.
/// @param args The arguments.
/// @param work The scratch root; `work/proj` is also the working directory.
/// @param tag A discriminator so each invocation gets its own capture files.
/// @param env The variables to run under; see `pinned_env`.
/// @return The captured result.
inline auto run_pinned(const std::filesystem::path& bin, std::span<const std::string> args, const std::filesystem::path& work,
                       std::string_view tag, std::span<const pinned_var> env) -> capture {
  require_agent_db_pinned(work, env);

  auto const out_path  = work / std::format("{}.out", tag);
  auto const err_path  = work / std::format("{}.err", tag);
  auto const code_path = work / std::format("{}.code", tag);

  std::error_code discard;
  std::filesystem::remove(code_path, discard);

  std::string child = pinned_env_prefix(env);
  child += shell_quote(bin.string());
  for (auto const& arg : args) {
    child += " " + shell_quote(arg);
  }

  // `{ { child ; echo $? > code ; } 2>&1 1>&3 | cat > err ; } 3>&1 | cat > out`
  //
  // Redirections apply left to right. Inside the inner group fd1 is the
  // pipe to `cat > err`; `2>&1` points stderr at it, then `1>&3` points
  // stdout at fd3, which the outer group bound to the pipe feeding
  // `cat > out`. Neither stream ever reaches a seekable file in the child.
  std::string line = std::format("cd {} && {{ {{ {} ; echo $? > {} ; }} 2>&1 1>&3 | cat > {} ; }} 3>&1 | cat > {}",
                                 shell_quote((work / "proj").string()), child, shell_quote(code_path.string()),
                                 shell_quote(err_path.string()), shell_quote(out_path.string()));

  static_cast<void>(std::system(line.c_str()));

  int         code = -1;
  auto const  raw  = read_all(code_path);
  auto const  text = std::string_view{raw}.substr(0, raw.find('\n'));
  int         parsed{};
  auto const* first = text.data();
  if (auto const [ptr, ec] = std::from_chars(first, first + text.size(), parsed); ec == std::errc{}) {
    code = parsed;
  }
  return capture{.code = code, .out = read_all(out_path), .err = read_all(err_path)};
}

/// @brief `run_pinned` under the default pinned map of `work`.
/// @param bin The binary to run.
/// @param args The arguments.
/// @param work The scratch root; `work/proj` is also the working directory.
/// @param tag A discriminator so each invocation gets its own capture files.
/// @return The captured result.
inline auto run_pinned(const std::filesystem::path& bin, std::span<const std::string> args, const std::filesystem::path& work,
                       std::string_view tag) -> capture {
  return run_pinned(bin, args, work, tag, pinned_env(work));
}

/// @brief Start `bin` with `args` inside `work` under the same pinned
/// environment `run_pinned` uses, and return WITHOUT waiting for it.
///
/// WHY A SECOND ENTRY POINT EXISTS AT ALL (plan 996, task 6547, decision
/// 1035). `run_pinned` is synchronous, and a synchronous runner cannot
/// express the one thing the Zig integration suite covers that no in-process
/// test can reach: several real processes contending on one database while a
/// fourth watches. That scenario needs a parent that is still executing while
/// the children run — to release a barrier and to sample a clock AT the event
/// rather than after the fact.
///
/// THE PROCESS MODEL IS `/bin/sh`, NOT C++. Catch2 has none, and this header
/// is not the place to grow one. The caller writes a driver script, launches
/// it here, and the script owns backgrounding and `wait`. The C++ side owns
/// only the two things it must own: dropping the barrier file, and taking the
/// timestamps. That split is deliberate — task 6544 is the standing example
/// of a latency assertion that measured the wrong interval because the sample
/// was taken while parsing an already-complete log instead of at the event.
///
/// DETERMINISM. Nothing here waits on a sleep-and-hope. The caller polls the
/// filesystem for an explicit sentinel with a bounded deadline and fails
/// loudly when it expires, and every invariant asserted downstream
/// (exactly-once claiming, per-worker exit status, event ordering) holds for
/// ANY interleaving — the barrier only widens the contention window, it is
/// not load-bearing for correctness.
///
/// Streams go to `work/<tag>.out` / `work/<tag>.err` directly rather than
/// through the `cat` pipe `run_pinned` uses. That pipe defended the Zig
/// runtime's positional-write corruption (see above) and is now vestigial —
/// `zig/` was deleted at the M10 cutover (task 6045) and no such binary
/// exists to point this at. It is kept here only because a detached child
/// cannot be piped without leaving `cat` processes to reap, which is a
/// reason of its own.
///
/// REAPING IS THE CALLER'S SCRIPT'S JOB, AND NOTHING ENFORCES IT. This
/// returns `void`: no pid, no handle, so a caller that backgrounds a binary
/// directly instead of through a self-reaping driver has no way to stop it,
/// and `make_arena`'s destructor removes the directory out from under a
/// process that keeps running. `cross_process.t.cpp`'s driver does this
/// correctly — `kill -INT "$WPID"` then `wait` before it writes its `done`
/// sentinel — and that is the pattern to copy.
///
/// The failure mode is not hypothetical. During task 6547's development a
/// PROTOTYPE launcher (not the committed test) left a driver script and a
/// `planar-watch feed --follow` running for NINE AND A HALF HOURS, polling a
/// scratch database every 200ms and quietly loading every measurement taken
/// in that window. It was found by `ps`, not by any gate. If you are
/// prototyping against this, reap what you start.
/// The agent database is checked before the launch exactly as `run_pinned`
/// checks it, from the same map; see `require_agent_db_pinned`.
/// @param bin The binary to run.
/// @param args The arguments.
/// @param work The scratch root; `work/proj` is also the working directory.
/// @param tag A discriminator so each launch gets its own capture files.
/// @param env The variables to run under; see `pinned_env`.
inline auto launch_pinned_detached(const std::filesystem::path& bin, std::span<const std::string> args,
                                   const std::filesystem::path& work, std::string_view tag, std::span<const pinned_var> env)
    -> void {
  require_agent_db_pinned(work, env);

  std::string child = pinned_env_prefix(env);
  child += shell_quote(bin.string());
  for (auto const& arg : args) {
    child += " " + shell_quote(arg);
  }

  // The whole pipeline runs inside `( ... ) &` so the outer `sh` this
  // `std::system` call spawns exits at once and `std::system` returns.
  std::string const line = std::format("( cd {} && {} > {} 2> {} ) &", shell_quote((work / "proj").string()), child,
                                       shell_quote((work / std::format("{}.out", tag)).string()),
                                       shell_quote((work / std::format("{}.err", tag)).string()));
  static_cast<void>(std::system(line.c_str()));
}

/// @brief `launch_pinned_detached` under the default pinned map of `work`.
/// @param bin The binary to run.
/// @param args The arguments.
/// @param work The scratch root; `work/proj` is also the working directory.
/// @param tag A discriminator so each launch gets its own capture files.
inline auto launch_pinned_detached(const std::filesystem::path& bin, std::span<const std::string> args,
                                   const std::filesystem::path& work, std::string_view tag) -> void {
  launch_pinned_detached(bin, args, work, tag, pinned_env(work));
}

/// @brief Poll for `path` to exist (and, when `nonempty`, to have bytes) and
/// return the steady-clock instant at which it did.
///
/// THE RETURNED INSTANT IS SAMPLED AT THE EVENT. That is the entire reason
/// this is a function and not an inline loop: task 6544's wake-latency
/// assertion read its timestamp while parsing a log that had already been
/// fully written, so the interval it reported was the whole scenario's
/// duration and it failed only under load. A caller that wants "how long
/// after X did Y first appear" must take both samples at X and at Y, and this
/// returns the second one.
/// @param path The sentinel to wait for.
/// @param nonempty Require a non-zero size, not merely existence.
/// @param budget How long to wait before giving up.
/// @return The instant the condition first held, or nullopt on timeout.
inline auto await_sentinel(const std::filesystem::path& path, bool nonempty, std::chrono::milliseconds budget)
    -> std::optional<std::chrono::steady_clock::time_point> {
  auto const deadline = std::chrono::steady_clock::now() + budget;
  while (std::chrono::steady_clock::now() <= deadline) {
    std::error_code ec;
    if (std::filesystem::exists(path, ec) && (!nonempty || std::filesystem::file_size(path, ec) > 0)) {
      return std::chrono::steady_clock::now();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return std::nullopt;
}

/// @brief A pair of scratch roots — one per binary — seeded identically.
///
/// REMOVES ITSELF ON DESTRUCTION (plan 996, task 6311). Every case in every
/// parity file builds one of these under `$TMPDIR`, and until this destructor
/// existed none of them were ever cleaned up. Measured twice: 106,886 stale
/// `planar_*` directories holding 82 GB, and separately 25,551 regenerated in
/// roughly a day of cycles. Both times the volume filled.
///
/// The reason that mattered more than the disk is the FAILURE SIGNATURE. When
/// the volume fills mid-suite, SQLite cannot open its scratch databases and
/// ctest reports a mass `conn.has_value() == false` cascade — which reads
/// exactly like a database-layer regression. One cycle spent real time
/// debugging the DB layer before checking `df`. A leak whose symptom
/// impersonates a code defect is worth more than a leak that merely consumes
/// space.
///
/// Set `PLANAR_KEEP_ARENAS=1` to retain them. A failing differential is
/// exactly when the arena's contents are worth reading, and re-running the
/// one failing case with that variable set is cheaper than retaining
/// thousands of directories against the possibility. That trade is the whole
/// reason the original code kept everything: nobody wanted to delete the
/// evidence. This keeps the evidence available on demand instead.
struct arena {
  std::filesystem::path cpp_root; ///< Scratch root for the C++ binary.
  std::filesystem::path zig_root; ///< Scratch root for the Zig binary.

  arena()                        = default;
  arena(const arena&)            = delete; ///< Owns a directory; copying would double-remove.
  arena& operator=(const arena&) = delete;
  arena(arena&&)                 = default;
  arena& operator=(arena&&)      = default;

  /// @brief Remove the arena unless `PLANAR_KEEP_ARENAS` is set.
  ~arena() {
    if (cpp_root.empty()) {
      return; // moved-from
    }
    char const* keep = std::getenv("PLANAR_KEEP_ARENAS");
    if (keep != nullptr && *keep != '\0' && *keep != '0') {
      return;
    }
    std::error_code ec;
    std::filesystem::remove_all(cpp_root.parent_path(), ec); // best effort
  }
};

/// @brief Create a fresh arena.
/// @param tag A short discriminator so a failure names its own case.
/// @return The created arena.
inline auto make_arena(std::string_view tag) -> arena {
  auto const      base = std::filesystem::temp_directory_path() /
                         std::format("planar_cmd_parity_{}_{}", tag, std::chrono::steady_clock::now().time_since_epoch().count());
  std::error_code ec;
  // Field-assigned rather than designated-initialized: `arena` declares a
  // destructor and deleted copies (task 6311), which disqualifies it as an
  // aggregate, so `{.cpp_root = ...}` no longer compiles.
  arena result;
  result.cpp_root = base / "cpp";
  result.zig_root = base / "zig";
  for (auto const& root : {result.cpp_root, result.zig_root}) {
    std::filesystem::create_directories(root / "home", ec);
    std::filesystem::create_directories(root / "proj", ec);
    std::filesystem::create_directories(root / "fakehome", ec);
  }
  return result;
}

} // namespace planar::cmd::parity
