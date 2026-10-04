// @file cross_process.t.cpp
// @brief The part of the Zig integration suite that NO in-process test can
// replace: the binaries' ability to start at all, their promise not to touch
// a database when they need none, and several REAL processes contending on
// one database while a fourth watches (plan 996, task 6547, decision 1035).
//
// ## Why these four cases and not the other ~584
//
// Decision 1035 retires the Zig integration suite in four steps. Task 6546
// closed the eight uncovered leaves; task 6548 deletes the redundant
// per-verb blocks. The redundancy is real: 1,077 `*_leaves.t.cpp` cases
// already build a `context` over `std::ostringstream` IN PROCESS and invoke
// the handler table directly, which pins argv parsing, handler logic, JSON
// shape and exit code for every leaf.
//
// What that shape structurally cannot reach, at any count:
//
//   - THE BINARY FAILING TO START. Task 6536: a build that linked a shared
//     library produced a `planar` that died at dyld with `Library not loaded:
//     @rpath/...`, exit 134. All 3,422 in-process tests
//     passed, because an in-process test never links the shipped executable
//     — it IS the executable. Only a real exec catches this class.
//   - "THIS VERB OPENS NO DATABASE." An in-process test constructs the
//     context itself and so decides the answer it is asking about. Only an
//     exec into a pinned empty arena can observe that no file appeared.
//   - CROSS-PROCESS EXACTLY-ONCE. The claim ritual's whole thesis is that
//     several `planar-agent` processes can drain one queue without
//     double-claiming. In one process there is no second connection, no
//     SQLite lock contention, and no race to lose.
//   - A READ-ONLY OBSERVER'S TIMELINE. `planar-watch feed --follow` is a
//     separate process polling a database other processes are writing. Its
//     coherence is a property of the file, not of any one handler.
//
// The `install.sh` end-to-end lifecycle
// (`zig/integration_tests/installed_surface_test.zig`) is equally
// irreplaceable and is deliberately NOT ported here: it runs a real
// `cmake --preset debug` configure-and-build inside the test, and nesting
// that inside `ctest` — which is itself driven by a build of the same tree —
// is a reentrancy hazard, not a porting exercise. It is classified
// irreplaceable-keep for task 6548 instead. See this task's report.
//
// ## How the cross-process cases synchronize, and why that is deterministic
//
// Catch2 has no process model and `run_pinned` is synchronous, so neither
// can express "a parent that is still running while its children contend."
// The mechanism chosen is: **the process model is `/bin/sh`; the clock is
// C++.** A driver script (written into the arena below) owns backgrounding,
// the worker loop and `wait`. The test owns exactly two things — dropping
// the barrier file, and sampling `steady_clock` — because those are the two
// the Zig suite got wrong.
//
// Determinism does not rest on the barrier. Every invariant asserted here
// (exactly-once claiming, six distinct tokens, per-worker exit 0, every
// `completed` preceded by its own `claim_acquired`) holds for ANY
// interleaving; the barrier only widens the window in which contention can
// occur, so a scheduler that serializes the workers still produces a green,
// meaningful run. Every wait is a bounded poll on an explicit sentinel that
// FAILS LOUDLY on expiry — there is no sleep-and-hope anywhere.
//
// The latency assertion is the one that needs care, and task 6544 is why.
// That assertion looked like a Tier-2 wake check and actually measured total
// scenario duration, because its timestamp was sampled inside a parse loop
// walking a log that had already been fully written. It passed for months
// and failed only under load. Here both samples are taken at the events they
// name: `t_barrier` immediately after the barrier file is created, and
// `t_first` by `await_sentinel` the moment the watcher's NDJSON log first has
// a byte in it. What is asserted is therefore literally "how long after
// barrier release did the watcher surface anything," and nothing else.
//
// ## Capture discipline
//
// Nothing below was retyped. The seed sequence, the event vocabulary
// (`claim_acquired` / `action_started` / `action_ended` / `completed`), the
// `no_work` sentinel and the `version` line's shape were all captured by
// running the built binaries against a pinned scratch arena and reading the
// bytes back. The `version` line is pinned as its stable prefix plus a
// single-line/empty-stderr/exit-0 shape rather than in full: its tail
// carries the compiler version (`Clang-23.1.0`), which is a toolchain fact
// and not a contract of this binary. That is a deliberate, documented
// exclusion of one segment — not a `contains` loosening of the whole line.
//
// ## BREAK-PROBES, run against PRODUCTION code, not against expectations
//
// Seven mutations, each applied to shipped code, rebuilt, and the case
// re-run; each restored afterwards. All seven KILL.
//
//   (1) `handlers/version.cpp`: `ctx.out()` -> `ctx.err()`.
//       -> case 1 fails at `(one.on_stderr ? ran.out : ran.err).empty()`.
//   (2) `dispatch.cpp`: the `schema` lambda calls `ctx.db().ensure_db()` first
//       -- exactly the telemetry-through-the-migrating-open defect the Zig
//       original was written for.
//       -> case 2 fails at `REQUIRE_FALSE(exists(db))`.
//   (3) `engine/runtime/agentatomic.cpp`: `pick_next_eligible` drops
//       `and t.status = 'todo'`.
//       -> case 3 fails; the drain never terminates and the `done`
//       sentinel times out. A slow kill (120s) but an unambiguous one.
//   (4) same file: `set_task_status` returns success without writing.
//       -> case 3 fails at `observed.rc == "0\n"` (a worker exits 93).
//   (5) same file: `complete`'s flip targets `"todo"` instead of
//       `targs.task_to`.
//       -> case 3 fails; tasks return to the queue and the drain never
//       terminates. This is the exactly-once failure mode itself.
//   (6) `planar-watch/handlers/feed.cpp`: drop the `claim_acquired` emit.
//       -> case 4 fails at the `claim_acquired` count.
//   (7) same file: render `claim_acquired` with a constant bogus token.
//       -> case 4 fails at `acquired.contains(token)` -- the pairing
//       assertion, killed independently of the counts.
//
// HONEST GAPS. Two assertions were NOT killed independently:
//   - The per-worker spread (`per_worker.size() == k_workers`). Starving
//     one worker requires a scheduler outcome, not a code change; the
//     assertion is retained because a real regression (a worker crashing
//     out of its loop early) does produce it, but no mutation here proves
//     that.
//   - Reversing the feed's sort to descending (an eighth mutation, run and
//     recorded) killed case 4 at the COUNT assertion rather than at the
//     ordering one, because `--follow`'s incremental cursor is itself
//     order-dependent. It is a kill, but not a kill of the assertion it
//     was aimed at; (7) is what covers that one.
#include <catch2/catch_test_macros.hpp>

#include <sys/wait.h>

import std;

#include "parity_harness.hpp"

namespace {

using ::planar::cmd::parity::arena;
using ::planar::cmd::parity::await_sentinel;
using ::planar::cmd::parity::capture;
using ::planar::cmd::parity::launch_pinned_detached;
using ::planar::cmd::parity::make_arena;
using ::planar::cmd::parity::read_all;
using ::planar::cmd::parity::run_pinned;

/// @brief Path to the built `planar` binary.
/// @return The path.
auto cpp_bin() -> std::filesystem::path {
  return std::filesystem::path{PLANAR_CPP_BIN};
}

/// @brief Path to the built `planar-agent` binary.
/// @return The path.
auto agent_bin() -> std::filesystem::path {
  return std::filesystem::path{PLANAR_AGENT_CPP_BIN};
}

/// @brief Path to the built `planar-watch` binary.
/// @return The path.
auto watch_bin() -> std::filesystem::path {
  return std::filesystem::path{PLANAR_WATCH_CPP_BIN};
}

/// @brief Path to the built `planar-execute` binary.
/// @return The path.
auto execute_bin() -> std::filesystem::path {
  return std::filesystem::path{PLANAR_EXECUTE_CPP_BIN};
}

/// @brief Path to the built `planar-ext` binary.
/// @return The path.
auto ext_bin() -> std::filesystem::path {
  return std::filesystem::path{PLANAR_EXT_CPP_BIN};
}

/// @brief FNV-1a over the database file AND its `-wal` / `-shm` siblings.
///
/// THE MAIN FILE ALONE IS NOT A VALID PROBE, and `parity_harness.hpp`'s own
/// header records why: under WAL a write lands in the `-wal` sibling and the
/// `.db` file's mtime — and often its bytes — do not move. A liveness probe
/// that reads only `planar.db` reports "untouched" for a migration that
/// actually happened. Hashing all three closes that.
/// @param db The database path.
/// @return A hash of the whole on-disk database state.
auto db_fingerprint(const std::filesystem::path& db) -> std::uint64_t {
  std::uint64_t hash = 1469598103934665603ULL;
  for (auto const* suffix : {"", "-wal", "-shm"}) {
    auto const bytes = read_all(std::filesystem::path{db.string() + suffix});
    for (unsigned char const byte : bytes) {
      hash ^= byte;
      hash *= 1099511628211ULL;
    }
    hash ^= 0xffULL; // separator, so ("ab","") and ("a","b") differ
    hash *= 1099511628211ULL;
  }
  return hash;
}

/// @brief Count non-overlapping occurrences of `needle` in `text`.
/// @param text The haystack.
/// @param needle The needle.
/// @return The count.
auto count_of(std::string_view text, std::string_view needle) -> std::size_t {
  std::size_t seen = 0;
  for (std::size_t at = text.find(needle); at != std::string_view::npos; at = text.find(needle, at + needle.size())) {
    ++seen;
  }
  return seen;
}

/// @brief Split `text` on single spaces.
/// @param text The text.
/// @return The fields.
auto split_ws(std::string_view text) -> std::vector<std::string> {
  std::vector<std::string> out;
  for (auto const part : std::views::split(text, ' ')) {
    std::string_view const field{part.begin(), part.end()};
    if (!field.empty()) {
      out.emplace_back(field);
    }
  }
  return out;
}

/// @brief Split `text` on newlines, dropping the empty trailing element.
/// @param text The text.
/// @return The lines.
auto lines_of(std::string_view text) -> std::vector<std::string> {
  std::vector<std::string> out;
  for (auto const part : std::views::split(text, '\n')) {
    std::string_view const line{part.begin(), part.end()};
    if (!line.empty()) {
      out.emplace_back(line);
    }
  }
  return out;
}

/// @brief Extract the value of `"<key>":"<value>"` from a JSON line.
/// @param line The line.
/// @param key The key.
/// @return The value, or empty when absent.
auto json_string_field(std::string_view line, std::string_view key) -> std::string {
  auto const needle = std::format("\"{}\":\"", key);
  auto const at     = line.find(needle);
  if (at == std::string_view::npos) {
    return {};
  }
  auto const from = at + needle.size();
  auto const to   = line.find('"', from);
  if (to == std::string_view::npos) {
    return {};
  }
  return std::string{line.substr(from, to - from)};
}

// -------------------------------------------------------------------------
// The concurrency scenario's tunables, carried over from the Zig scenario
// they replace (`scenarios/scenario_multi_agent_session_test.zig`): three
// workers against six tasks, so every worker contends and the expected
// spread (two each) is meaningful without combinatorial runtime.
// -------------------------------------------------------------------------
constexpr int k_workers = 3;
constexpr int k_tasks   = 6;

/// @brief The driver script the concurrency cases launch.
///
/// `$1` planar-agent, `$2` planar-watch, `$3` workdir, `$4` plan id,
/// `$5` worker count. Written to the arena and run under the pinned
/// environment, so every `planar-agent` it spawns inherits `PLANAR_DB` and
/// the rest — the containment invariant is the launcher's, not the script's.
///
/// Sentinels it produces, all of which the C++ side waits on explicitly:
/// `ready.<n>` (worker parked at the barrier), `claims.log` (one
/// `<worker> <task> <token>` line per successful claim), `worked.log` (one
/// `<worker> <count>` line per worker that finished its loop), `exit.<n>`
/// (that worker's loop ran to completion), `drained` (every worker reaped),
/// `rc` (0 iff every worker exited 0), `done` (the watcher is reaped too).
constexpr std::string_view k_driver = R"SH(#!/bin/sh
AG="$1"; WA="$2"; W="$3"; PLAN="$4"; N="$5"
cd "$W" || exit 90
"$WA" feed --follow --json --interval 200ms > "$W/watch.log" 2> "$W/watch.err" &
WPID=$!
PIDS=""
i=1
while [ "$i" -le "$N" ]; do
  (
    me=$i
    : > "$W/ready.$me"
    while [ ! -f "$W/barrier" ]; do sleep 0.01; done
    n=0
    while :; do
      out=$("$AG" pull "$PLAN" --json 2>>"$W/w$me.err")
      case "$out" in
        *'"no_work":true'*) break ;;
        *'"claim_token"'*) ;;
        *) printf 'UNEXPECTED:%s\n' "$out" >> "$W/w$me.err"; exit 91 ;;
      esac
      tok=$(printf '%s' "$out" | sed -n 's/.*"claim_token":"\([0-9a-f]*\)".*/\1/p')
      [ -n "$tok" ] || { printf 'NOTOKEN:%s\n' "$out" >> "$W/w$me.err"; exit 92; }
      tid=$(printf '%s' "$out" | sed -n 's/.*"entity_kind":"task","entity_id":\([0-9]*\).*/\1/p')
      printf '%s %s %s\n' "$me" "$tid" "$tok" >> "$W/claims.log"
      "$AG" complete --claim "$tok" >> "$W/w$me.out" 2>> "$W/w$me.err" || exit 93
      n=$((n+1))
    done
    printf '%s %s\n' "$me" "$n" >> "$W/worked.log"
    printf '0\n' > "$W/exit.$me"
  ) &
  PIDS="$PIDS $!"
  i=$((i+1))
done
rc=0
for p in $PIDS; do
  wait "$p" || rc=1
done
: > "$W/drained"
sleep 1
kill -INT "$WPID" 2>/dev/null
wait "$WPID" 2>/dev/null
printf '%s\n' "$rc" > "$W/rc"
: > "$W/done"
)SH";

/// @brief Write an executable shell script.
/// @param path Where to write it.
/// @param body The script body, `#!` line included.
auto write_script(const std::filesystem::path& path, std::string_view body) -> void {
  {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    REQUIRE(file.good());
    file << body;
  }
  std::error_code ec;
  std::filesystem::permissions(path, std::filesystem::perms::owner_all, ec);
  REQUIRE(!ec);
}

/// @brief Seed one arena with an active plan carrying `k_tasks` todo tasks.
/// @param work The arena's scratch root.
auto seed_queue(const std::filesystem::path& work) -> void {
  std::vector<std::vector<std::string>> seed{
      {"init", "--name", "demo", "--slug", "demo"},
      {"assoc", "create", "project:demo", "--kind", "project"},
      {"assoc", "add", "project:demo", (work / "proj").string()},
      {"plan", "create", "Drain", "--slug", "drain", "--summary", "Queue drain."},
  };
  for (int i = 1; i <= k_tasks; ++i) {
    seed.push_back({"task", "add", std::format("T{}", i), "--plan", "1", "--slug", std::format("t-{}", i), "--editor=false"});
  }
  seed.push_back({"plan", "update", "1", "--status", "active"});
  for (std::size_t i = 0; i < seed.size(); ++i) {
    auto const tag = std::format("seed{}", i);
    auto const ran = run_pinned(cpp_bin(), seed[i], work, tag);
    INFO("seed step " << tag << " stderr: " << ran.err);
    REQUIRE(ran.code == 0);
  }
}

/// @brief What one concurrency run observed. Both cross-process cases drive
/// the same run; splitting the observations keeps each assertion's failure
/// message about one thing.
struct drain_run {
  std::string                                          claims;    ///< claims.log
  std::string                                          worked;    ///< worked.log
  std::string                                          watch_log; ///< the watcher's NDJSON
  std::string                                          rc;        ///< the script's worker rollup
  std::chrono::steady_clock::time_point                t_barrier{};
  std::optional<std::chrono::steady_clock::time_point> t_first;
};

/// @brief Seed a queue, release three real workers at a barrier, and reap.
/// @param work The arena's scratch root.
/// @return The observations.
auto run_drain(const std::filesystem::path& work) -> drain_run {
  seed_queue(work);

  auto const script = work / "drain.sh";
  write_script(script, k_driver);

  std::vector<std::string> const argv{
      script.string(), agent_bin().string(), watch_bin().string(), (work / "proj").string(), "1", std::to_string(k_workers),
  };
  launch_pinned_detached(std::filesystem::path{"/bin/sh"}, argv, work, "driver");

  auto const proj = work / "proj";

  // Wait for every worker to be parked at the barrier before releasing it.
  // This is what makes the release a real starting gun rather than a
  // staggered start, and it is a bounded wait on an explicit sentinel.
  for (int i = 1; i <= k_workers; ++i) {
    INFO("worker " << i << " never reached the barrier; driver stderr: " << read_all(work / "driver.err"));
    REQUIRE(await_sentinel(proj / std::format("ready.{}", i), false, std::chrono::seconds(60)).has_value());
  }

  drain_run observed;
  {
    std::ofstream barrier(proj / "barrier", std::ios::binary | std::ios::trunc);
    REQUIRE(barrier.good());
  }
  // Sampled HERE, immediately after the file exists — the release instant,
  // not "some time before parsing." See this file's header on task 6544.
  observed.t_barrier = std::chrono::steady_clock::now();

  // Sampled by the poller the moment the watcher's log first has a byte.
  // Started before the drain is reaped, so it overlaps the drain rather
  // than measuring after it.
  observed.t_first = await_sentinel(proj / "watch.log", true, std::chrono::seconds(30));

  INFO("driver never finished; driver stderr: " << read_all(work / "driver.err"));
  REQUIRE(await_sentinel(proj / "done", false, std::chrono::seconds(120)).has_value());

  observed.claims    = read_all(proj / "claims.log");
  observed.worked    = read_all(proj / "worked.log");
  observed.watch_log = read_all(proj / "watch.log");
  observed.rc        = read_all(proj / "rc");
  return observed;
}

} // namespace

// =========================================================================
// 1. The binaries start.
// =========================================================================

TEST_CASE("every built binary starts as a real process and reaches its own code (task 6536)", "[cmd][crossproc][smoke]") {
  auto const space = make_arena("xproc_start");

  // `version` for the four planning-state binaries; `planar-execute` has no
  // `version` verb at all (it exits 2 with usage), so its spine leaf is
  // `--help`. Captured, not assumed: `planar-execute version` and
  // `planar-execute --version` both exit 2.
  struct spine {
    std::filesystem::path bin;
    std::string           argv;
    std::string           marker;
    bool                  on_stderr; ///< Which stream the marker lands on.
  };
  // `planar-execute` prints its usage on STDERR and leaves stdout empty —
  // captured from a real run, not assumed. Pinning the stream is part of
  // the point: a binary that died at dyld also prints on stderr and nothing
  // on stdout, so the case has to say which stream carries WHAT.
  auto const spines = std::to_array<spine>({
      {cpp_bin(), "version", "planar dev dev cxx Clang-", false},
      {agent_bin(), "version", "planar-agent dev dev cxx Clang-", false},
      {watch_bin(), "version", "planar-watch dev dev cxx Clang-", false},
      {ext_bin(), "version", "planar-ext dev dev cxx Clang-", false},
      {execute_bin(), "--help", "planar-execute — deterministic, spawn-free Lua workflow engine.\n", true},
  });

  for (auto const& one : spines) {
    auto const ran = run_pinned(one.bin, std::vector<std::string>{one.argv}, space.cpp_root, one.bin.filename().string());
    INFO(one.bin.filename().string() << " " << one.argv << " -> code=" << ran.code << " out=" << ran.out << " err=" << ran.err);

    // A dyld failure (task 6536) never reaches the binary's own code: the
    // process dies at 134 with the loader's message on stderr and nothing on
    // stdout. All three of these assertions distinguish that.
    REQUIRE(ran.code == 0);
    REQUIRE((one.on_stderr ? ran.out : ran.err).empty());
    REQUIRE((one.on_stderr ? ran.err : ran.out).starts_with(one.marker));
  }

  // The `version` line's full shape, minus the toolchain segment: exactly
  // one line, newline-terminated, nothing after it.
  for (auto const& one : spines) {
    if (one.argv != "version") {
      continue;
    }
    auto const ran =
        run_pinned(one.bin, std::vector<std::string>{"version"}, space.cpp_root, std::format("{}v", one.bin.filename().string()));
    REQUIRE(ran.out.ends_with("\n"));
    REQUIRE(count_of(ran.out, "\n") == 1);
  }
}

// =========================================================================
// 2. Pure verbs open no database.
// =========================================================================

TEST_CASE("verbs that need no database neither create one nor migrate an existing one", "[cmd][crossproc][nodb]") {
  // Port of zig/integration_tests/no_db_side_effects_test.zig. That file
  // records the incident it exists for: CLI telemetry acquired its handle
  // through the MIGRATING open on every invocation, so `--help` — which
  // `scripts/coverage-check.sh` runs once per verb — silently advanced the
  // operator's live database to a dev build's schema, and every installed
  // binary then failed with SchemaVersionAhead. The telemetry path swallowed
  // its errors, so help text printed normally while the migration landed.
  struct pure {
    std::filesystem::path    bin;
    std::vector<std::string> argv;
  };
  std::vector<pure> cases;
  for (auto const& bin : {cpp_bin(), agent_bin(), watch_bin(), ext_bin()}) {
    cases.push_back({bin, {"--help"}});
    cases.push_back({bin, {"schema"}});
    cases.push_back({bin, {"version"}});
  }
  // `planar-execute` exposes neither `schema` nor `version` (it holds no
  // SQLite handle at all by design — decision 995), so `--help` is its
  // whole pure surface.
  cases.push_back({execute_bin(), {"--help"}});

  SECTION("against an empty arena, no database file appears") {
    auto const space = make_arena("xproc_nodb_fresh");
    auto const db    = space.cpp_root / "planar.db";
    for (std::size_t i = 0; i < cases.size(); ++i) {
      auto const& one = cases[i];
      auto const  ran = run_pinned(one.bin, one.argv, space.cpp_root, std::format("nodb{}", i));
      INFO(one.bin.filename().string() << " " << one.argv[0] << " -> code=" << ran.code << " err=" << ran.err);
      REQUIRE(ran.code == 0);
      // The post-condition, not the exit code: exit 0 is also what a verb
      // that quietly opened and migrated a database returns.
      REQUIRE_FALSE(std::filesystem::exists(db));
      REQUIRE_FALSE(std::filesystem::exists(std::filesystem::path{db.string() + "-wal"}));
    }
  }

  SECTION("against a seeded arena, the database is byte-identical afterwards") {
    auto const space = make_arena("xproc_nodb_seeded");
    auto const db    = space.cpp_root / "planar.db";
    seed_queue(space.cpp_root);
    REQUIRE(std::filesystem::exists(db));

    auto const before = db_fingerprint(db);
    for (std::size_t i = 0; i < cases.size(); ++i) {
      auto const& one = cases[i];
      auto const  ran = run_pinned(one.bin, one.argv, space.cpp_root, std::format("nodbs{}", i));
      INFO(one.bin.filename().string() << " " << one.argv[0] << " -> code=" << ran.code << " err=" << ran.err);
      REQUIRE(ran.code == 0);
    }
    REQUIRE(db_fingerprint(db) == before);
  }
}

// =========================================================================
// 3. Cross-process exactly-once.
// =========================================================================

TEST_CASE("three concurrent planar-agent processes drain one queue exactly once", "[cmd][crossproc][drain]") {
  auto const space    = make_arena("xproc_drain");
  auto const observed = run_drain(space.cpp_root);

  INFO("claims.log:\n" << observed.claims << "\nworked.log:\n" << observed.worked);

  // Every worker's loop ran to completion and the script's rollup says none
  // exited non-zero. A SQLITE_BUSY pile-up shows up here first.
  REQUIRE(observed.rc == "0\n");

  auto const claim_lines = lines_of(observed.claims);
  REQUIRE(claim_lines.size() == static_cast<std::size_t>(k_tasks));

  std::set<std::string>      tasks_claimed;
  std::set<std::string>      tokens;
  std::map<std::string, int> per_worker;
  for (auto const& line : claim_lines) {
    auto const parts = split_ws(line);
    REQUIRE(parts.size() == 3);
    per_worker[parts[0]] += 1;
    tasks_claimed.insert(parts[1]);
    tokens.insert(parts[2]);
  }

  // EXACTLY-ONCE: six claims over six DISTINCT tasks with six DISTINCT
  // tokens. A double-claim collapses one of the two sets.
  REQUIRE(tasks_claimed.size() == static_cast<std::size_t>(k_tasks));
  REQUIRE(tokens.size() == static_cast<std::size_t>(k_tasks));

  // No greedy starvation: every worker did some of the work.
  REQUIRE(per_worker.size() == static_cast<std::size_t>(k_workers));
  for (auto const& [worker, count] : per_worker) {
    INFO("worker " << worker << " claimed " << count);
    REQUIRE(count >= 1);
  }

  // POST-STATE, read back through a fresh process rather than inferred from
  // the workers' exit codes: all six tasks are `done`.
  auto const after = run_pinned(cpp_bin(), std::vector<std::string>{"task", "list", "--plan", "1", "--status", "done", "--json"},
                                space.cpp_root, "drainpost");
  REQUIRE(after.code == 0);
  REQUIRE(count_of(after.out, "\"status\":\"done\"") == static_cast<std::size_t>(k_tasks));
  REQUIRE(count_of(after.out, "\"status\":\"todo\"") == 0);
  REQUIRE(count_of(after.out, "\"status\":\"doing\"") == 0);
}

// =========================================================================
// 4. A fourth, read-only process observes a coherent timeline.
// =========================================================================

TEST_CASE("a read-only planar-watch process observes the drain as an ordered timeline", "[cmd][crossproc][watch]") {
  auto const space    = make_arena("xproc_watch");
  auto const observed = run_drain(space.cpp_root);

  INFO("watch.log:\n" << observed.watch_log << "\nwatch.err:\n" << read_all(space.cpp_root / "proj" / "watch.err"));

  // The full event vocabulary, one per task per kind. Captured from a real
  // run, not enumerated from the source.
  REQUIRE(count_of(observed.watch_log, "\"event\":\"claim_acquired\"") == static_cast<std::size_t>(k_tasks));
  REQUIRE(count_of(observed.watch_log, "\"event\":\"action_started\"") == static_cast<std::size_t>(k_tasks));
  REQUIRE(count_of(observed.watch_log, "\"event\":\"action_ended\"") == static_cast<std::size_t>(k_tasks));
  REQUIRE(count_of(observed.watch_log, "\"event\":\"completed\"") == static_cast<std::size_t>(k_tasks));

  // ORDERING: every `completed` is preceded, in the stream, by a
  // `claim_acquired` carrying the SAME claim token. This is the assertion
  // that a shuffled or deduplicated feed fails.
  std::set<std::string> acquired;
  std::size_t           completions = 0;
  for (auto const& line : lines_of(observed.watch_log)) {
    auto const event = json_string_field(line, "event");
    auto const token = json_string_field(line, "claim_token");
    if (event == "claim_acquired") {
      REQUIRE_FALSE(token.empty());
      acquired.insert(token);
    } else if (event == "completed") {
      INFO("completed with no prior claim_acquired for token " << token);
      REQUIRE(acquired.contains(token));
      ++completions;
    }
  }
  REQUIRE(completions == static_cast<std::size_t>(k_tasks));

  // Every NDJSON line is one whole JSON object — no interleaved partial
  // writes from a process writing while another reads.
  for (auto const& line : lines_of(observed.watch_log)) {
    REQUIRE(line.starts_with("{"));
    REQUIRE(line.ends_with("}"));
  }

  // LATENCY, measured between the two instants it names and no others: the
  // barrier release, and the moment the watcher's log first held a byte.
  // See this file's header for task 6544, the assertion this one is shaped
  // to avoid repeating. The budget is generous on purpose — this separates
  // "the watcher surfaced something while the drain was still running" from
  // "the watcher surfaced nothing until everything was over," which is the
  // only distinction a poll-interval-agnostic test can honestly make.
  REQUIRE(observed.t_first.has_value());
  auto const latency = std::chrono::duration_cast<std::chrono::milliseconds>(*observed.t_first - observed.t_barrier);
  INFO("first watcher byte " << latency.count() << "ms after barrier release");
  REQUIRE(latency <= std::chrono::seconds(10));
}
