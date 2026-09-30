// queue_run.t.cpp: `planar-agent queue run` as an operator sees it (plan 1080,
// task hq-queue-run-verb; tech spec 647 § Submitting, Waiting and claiming a
// turn, Running).
//
// Every case here runs the BUILT binary through `run_pinned`, in an arena
// with its own `PLANAR_AGENT_DB`, `PLANAR_DB`, `HOME` and
// `PLANAR_CONFIG_PATH`, so nothing can reach the operator's `~/.planar`. The
// store is read back through the engine's own typed reads after every step:
// an exit code alone would pass for a verb that ran the command and never
// touched the queue.
//
// SYNCHRONISATION IS BY FILES AND FIFOS, NOT BY SLEEPS. A command that has
// to stay alive blocks reading a FIFO the test holds open read-write, so the
// command's `read` blocks until the test writes and the write can never be
// lost to an ordering race. The test learns that a command started from a
// sentinel file it writes, and that a submitter is polling from the entry's
// `refreshed_mono` advancing in the store. The only clocks are the bounded
// waits that turn a hang into a failure.
//
// A gate is released on the way out of a failing case too, so a failed
// assertion cannot leave a blocked command behind.

#include <catch2/catch_test_macros.hpp>

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <libproc.h>
#include <sys/proc_info.h>
#endif

import std;
import planar.db;
import planar.db.agentdb;
import planar.process.identity;
import planar.engine.hostqueue;

#include "parity_harness.hpp"

namespace {

namespace hq     = planar::engine::hostqueue;
namespace ident  = planar::process::identity;
namespace parity = planar::cmd::parity;

using parity::capture;
using parity::pinned_var;
using parity::read_all;

constexpr auto k_budget = std::chrono::seconds(30);

auto agent_bin() -> std::filesystem::path {
  return std::filesystem::path{PLANAR_CPP_BIN};
}

/// @brief The arguments of `queue run -- <command>`.
auto queue_args(const std::vector<std::string>& command, std::vector<std::string> flags = {}) -> std::vector<std::string> {
  std::vector<std::string> args{"queue", "run"};
  for (auto& flag : flags) {
    args.push_back(std::move(flag));
  }
  args.emplace_back("--");
  for (auto const& word : command) {
    args.push_back(word);
  }
  return args;
}

/// @brief `sh -c <script> sh <args...>`, so the script reads `$1`, `$2`.
auto sh_command(std::string script, std::vector<std::string> args = {}) -> std::vector<std::string> {
  std::vector<std::string> command{"sh", "-c", std::move(script), "sh"};
  for (auto& arg : args) {
    command.push_back(std::move(arg));
  }
  return command;
}

auto run_queue(const parity::arena& arena, std::string_view tag, const std::vector<std::string>& command,
               std::vector<std::string> flags = {}) -> capture {
  auto const args = queue_args(command, std::move(flags));
  return parity::run_pinned(agent_bin(), args, arena.cpp_root, tag);
}

/// @brief Writes the arena's configuration file atomically, so a submitter
/// that reloads it mid-write never reads half of it.
void write_config(const parity::arena& arena, std::string_view text) {
  auto const path = arena.cpp_root / "config.toml";
  auto const temp = arena.cpp_root / "config.toml.new";
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    out << text;
  }
  std::filesystem::rename(temp, path);
}

constexpr std::string_view k_fast_poll = "[queue]\npoll_interval = \"100ms\"\n";

/// @brief What the store holds at one instant.
struct snapshot {
  std::vector<hq::entry>       entries;
  std::vector<hq::history_row> history;
};

/// @brief Reads the arena's store, or nothing when it does not exist or
/// cannot be read at this instant (a submitter may be creating it).
auto try_snapshot(const parity::arena& arena) -> std::optional<snapshot> {
  auto const path = arena.cpp_root / "agent.db";
  if (!std::filesystem::exists(path)) {
    return std::nullopt;
  }
  auto opened = planar::db::agent::open_agent_db_at(path);
  if (!opened) {
    return std::nullopt;
  }
  auto entries = hq::list(*opened);
  auto history = hq::list_history(*opened);
  if (!entries || !history) {
    return std::nullopt;
  }
  return snapshot{.entries = std::move(*entries), .history = std::move(*history)};
}

auto require_snapshot(const parity::arena& arena) -> snapshot {
  auto const now = try_snapshot(arena);
  REQUIRE(now.has_value());
  return *now;
}

/// @brief Polls `predicate` until it holds or the budget ends.
template <class Predicate> auto await(Predicate&& predicate, std::chrono::milliseconds budget = k_budget) -> bool {
  auto const deadline = std::chrono::steady_clock::now() + budget;
  while (std::chrono::steady_clock::now() <= deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return predicate();
}

auto entry_seq(const snapshot& snap, std::int64_t seq) -> const hq::entry* {
  auto const found = std::ranges::find_if(snap.entries, [&](const hq::entry& e) { return e.seq == seq; });
  return found == snap.entries.end() ? nullptr : &*found;
}

auto history_seq(const snapshot& snap, std::int64_t seq) -> const hq::history_row* {
  auto const found = std::ranges::find_if(snap.history, [&](const hq::history_row& r) { return r.seq == seq; });
  return found == snap.history.end() ? nullptr : &*found;
}

/// @brief Waits for entry `seq` to be in `state`, and returns it.
auto await_entry(const parity::arena& arena, std::int64_t seq, hq::entry_state state) -> hq::entry {
  std::optional<hq::entry> seen;
  REQUIRE(await([&] {
    auto const snap = try_snapshot(arena);
    if (!snap) {
      return false;
    }
    auto const* found = entry_seq(*snap, seq);
    if (found != nullptr && found->state == state) {
      seen = *found;
      return true;
    }
    return false;
  }));
  return *seen;
}

/// @brief Waits for entry `seq` to be running AND to have its child group
/// recorded, and returns it. The handler marks the entry running before it
/// starts the command and records the group afterwards, so a command can
/// signal that it started before `child_pgid` is committed; a case that needs
/// the group waits on this, not on `await_entry`.
auto await_child_recorded(const parity::arena& arena, std::int64_t seq) -> hq::entry {
  std::optional<hq::entry> seen;
  REQUIRE(await([&] {
    auto const snap = try_snapshot(arena);
    if (!snap) {
      return false;
    }
    auto const* found = entry_seq(*snap, seq);
    if (found != nullptr && found->state == hq::entry_state::running && found->child_pgid.has_value()) {
      seen = *found;
      return true;
    }
    return false;
  }));
  return *seen;
}

/// @brief Waits for entry `seq` to be refreshed at least `polls` more times.
void await_refreshes(const parity::arena& arena, std::int64_t seq, int polls) {
  auto const  start = require_snapshot(arena);
  auto const* now   = entry_seq(start, seq);
  REQUIRE(now != nullptr);
  auto previous = now->refreshed_mono;
  for (int seen = 0; seen < polls; ++seen) {
    REQUIRE(await([&] {
      auto const snap = try_snapshot(arena);
      if (!snap) {
        return false;
      }
      auto const* found = entry_seq(*snap, seq);
      if (found != nullptr && found->refreshed_mono > previous) {
        previous = found->refreshed_mono;
        return true;
      }
      return false;
    }));
  }
}

/// @brief A FIFO the test holds open read-write. A command that reads it
/// blocks until `release()`.
struct gate {
  std::filesystem::path path;
  int                   fd       = -1;
  bool                  released = false;

  explicit gate(std::filesystem::path where) : path(std::move(where)) {
    REQUIRE(::mkfifo(path.c_str(), 0600) == 0);
    fd = ::open(path.c_str(), O_RDWR);
    REQUIRE(fd >= 0);
  }
  gate(const gate&)            = delete;
  gate& operator=(const gate&) = delete;
  ~gate() {
    release();
    if (fd >= 0) {
      ::close(fd);
    }
  }
  void release() {
    if (!released && fd >= 0) {
      released = true;
      static_cast<void>(::write(fd, "x\n", 2));
    }
  }
};

/// @brief Releases the gates it names when it leaves scope, so a failed
/// assertion cannot leave a blocked command behind.
struct release_all {
  std::vector<gate*> gates;
  ~release_all() {
    for (auto* g : gates) {
      g->release();
    }
  }
};

/// @brief A `queue run` started in the background by `spawn_queue`.
///
/// It owns the submitter it started: `spawn_queue` records the submitter's pid
/// (and its start time, so a reused pid is never signalled), and the
/// destructor stops the submitter when it has not ended by then, so a case
/// that fails part-way leaves no live process behind. Stopping it is a
/// SIGTERM to that one recorded pid, which the submitter forwards to its
/// command's group; a SIGKILL follows only if it outlives a bounded wait.
/// Nothing is ever signalled by name, and never pid 0, 1 or a group.
struct spawned {
  std::filesystem::path            root;    ///< The arena root the invocation ran under.
  std::string                      tag;     ///< Its capture-file name: `<tag>.out`, `.err`, `.code`, `.pid`.
  std::int64_t                     pid = 0; ///< The submitter's pid, as recorded by the shell that started it; 0 when none.
  std::optional<ident::start_time> started; ///< The submitter's start time when it was recorded.

  spawned() = default;
  spawned(std::filesystem::path where, std::string name, std::int64_t submitter, std::optional<ident::start_time> when)
      : root(std::move(where)), tag(std::move(name)), pid(submitter), started(when) {
  }
  spawned(const spawned&)            = delete;
  spawned& operator=(const spawned&) = delete;
  spawned(spawned&& other) noexcept
      : root(std::move(other.root)), tag(std::move(other.tag)), pid(std::exchange(other.pid, 0)), started(other.started) {
  }
  spawned& operator=(spawned&& other) noexcept {
    if (this != &other) {
      stop();
      root    = std::move(other.root);
      tag     = std::move(other.tag);
      pid     = std::exchange(other.pid, 0);
      started = other.started;
    }
    return *this;
  }
  ~spawned() {
    stop();
  }

  /// @brief True once the shell that started the submitter has written its
  /// exit status: the submitter has been reaped and its pid may be reused.
  [[nodiscard]] auto ended() const -> bool {
    return read_all(root / std::format("{}.code", tag)).ends_with('\n');
  }

  /// @brief Whether `pid` is still this test's submitter: the recorded start
  /// time still matches, so the pid was not reused.
  [[nodiscard]] auto still_mine() const -> bool {
    if (pid <= 1 || !started) {
      return false;
    }
    auto const now = ident::process_start_time(pid);
    return now && now->has_value() && **now == *started;
  }

  /// @brief Stops the submitter if it is still running and waits, bounded, for
  /// it to be reaped.
  void stop() {
    if (pid <= 1 || ended()) {
      pid = 0;
      return;
    }
    auto const bound = std::chrono::seconds(10);
    if (still_mine()) {
      ::kill(static_cast<::pid_t>(pid), SIGTERM);
    }
    if (!await([&] { return ended(); }, std::chrono::duration_cast<std::chrono::milliseconds>(bound)) && still_mine()) {
      ::kill(static_cast<::pid_t>(pid), SIGKILL);
      await([&] { return ended(); }, std::chrono::duration_cast<std::chrono::milliseconds>(bound));
    }
    pid = 0;
  }
};

/// @brief Starts `planar-agent queue run -- <command>` in the background,
/// under the arena's pinned environment plus `extra`, and returns at once.
///
/// This is not `run_pinned` on a thread. `std::system` serialises concurrent
/// callers on macOS, so a second `run_pinned` would not even start until the
/// first had finished, which is exactly the overlap these cases exist to
/// exercise. The streams go straight to files, as `launch_pinned_detached`
/// does, and the exit status to `<tag>.code`; `finish` reads them back. The
/// pinned map and the agent-database check are the harness's own. The
/// background subshell closes its own streams, so a case that fails while
/// its submitter is still waiting cannot keep the test process's pipes open.
///
/// The submitter itself is the subshell's background job, so `<tag>.pid`
/// holds the submitter's own pid and the subshell's `wait` yields its exit
/// status (128 plus N when a signal killed it). A shell starts an
/// asynchronous list with SIGINT ignored, and an ignored signal stays ignored
/// through `exec`; `default_int` puts the disposition back to the default
/// first, so a case that sends the submitter SIGINT reaches a command that
/// can trap it.
auto spawn_queue(const parity::arena& arena, std::string tag, const std::vector<std::string>& command,
                 std::vector<pinned_var> extra = {}, std::vector<std::string> flags = {}, bool default_int = false) -> spawned {
  auto vars = parity::pinned_env(arena.cpp_root);
  for (auto& var : extra) {
    vars.push_back(std::move(var));
  }
  parity::require_agent_db_pinned(arena.cpp_root, vars);

  std::string child = default_int ? "perl -e '$SIG{INT} = q(DEFAULT); exec @ARGV' " : "";
  child += parity::pinned_env_prefix(vars) + parity::shell_quote(agent_bin().string());
  for (auto const& arg : queue_args(command, std::move(flags))) {
    child += " " + parity::shell_quote(arg);
  }
  auto const path = [&](std::string_view suffix) {
    return parity::shell_quote((arena.cpp_root / std::format("{}.{}", tag, suffix)).string());
  };
  std::error_code ec;
  std::filesystem::remove(arena.cpp_root / std::format("{}.code", tag), ec);
  std::filesystem::remove(arena.cpp_root / std::format("{}.pid", tag), ec);
  auto const line = std::format(
      "( cd {} && {{ {} & echo $! > {} ; wait $! ; echo $? > {} ; }} > {} 2> {} ) </dev/null >/dev/null "
      "2>&1 &",
      parity::shell_quote((arena.cpp_root / "proj").string()), child, path("pid"), path("code"), path("out"), path("err"));
  static_cast<void>(std::system(line.c_str()));

  // The pid file is written by the background subshell, not by `system`'s
  // shell, so wait (bounded) for it.
  std::int64_t                     pid = 0;
  std::optional<ident::start_time> started;
  await([&] {
    auto const text = read_all(arena.cpp_root / std::format("{}.pid", tag));
    if (!text.ends_with('\n')) {
      return false;
    }
    std::int64_t value = 0;
    if (std::from_chars(text.data(), text.data() + text.size() - 1, value).ec != std::errc{} || value <= 1) {
      return false;
    }
    pid = value;
    return true;
  });
  if (pid > 1) {
    if (auto const at = ident::process_start_time(pid); at && at->has_value()) {
      started = **at;
    }
  }
  return spawned{arena.cpp_root, std::move(tag), pid, started};
}

/// @brief Waits for a spawned invocation to end and returns what it wrote.
auto finish(const spawned& run) -> capture {
  auto const code_path = run.root / std::format("{}.code", run.tag);
  INFO("waiting for " << run.tag << " to end");
  REQUIRE(await([&] { return read_all(code_path).ends_with('\n'); }));
  auto const  raw  = read_all(code_path);
  int         code = -1;
  auto const* head = raw.data();
  REQUIRE(std::from_chars(head, head + raw.size() - 1, code).ec == std::errc{});
  return capture{.code = code,
                 .out  = read_all(run.root / std::format("{}.out", run.tag)),
                 .err  = read_all(run.root / std::format("{}.err", run.tag))};
}

auto proj(const parity::arena& arena) -> std::filesystem::path {
  return arena.cpp_root / "proj";
}

auto present(const std::filesystem::path& path) -> bool {
  std::error_code ec;
  return std::filesystem::exists(path, ec);
}

/// @brief Blocks until the file exists.
void await_file(const std::filesystem::path& path) {
  INFO("waiting for " << path.string());
  REQUIRE(await([&] { return present(path); }));
}

} // namespace

// ---------------------------------------------------------------------------
// Scenario: Happy path: a queued command returns its own output and status
// ---------------------------------------------------------------------------

TEST_CASE("queue run: a queued command returns its own output and status", "[cmd][agent][queue]") {
  auto const arena  = parity::make_arena("qr_status");
  auto const script = std::string{"echo out; echo err >&2; exit 7"};

  auto const direct = parity::run_pinned("/bin/sh", std::vector<std::string>{"-c", script}, arena.cpp_root, "direct");
  REQUIRE(direct.code == 7);

  auto const queued = run_queue(arena, "queued", sh_command(script), {"--label", "status probe"});
  INFO("stderr:\n" << queued.err);
  CHECK(queued.code == 7);
  // Byte-identical to the direct run, and nothing else on either stream.
  CHECK(queued.out == direct.out);
  CHECK(queued.err == direct.err);
  CHECK(queued.out == "out\n");
  CHECK(queued.err == "err\n");

  // The store: no entry is left, and exactly one history row says how it
  // ended.
  auto const snap = require_snapshot(arena);
  CHECK(snap.entries.empty());
  REQUIRE(snap.history.size() == 1);
  auto const& row = snap.history.front();
  CHECK(row.seq == 1);
  CHECK(row.outcome == hq::history_outcome::exited);
  CHECK(row.exit_code == 7);
  CHECK_FALSE(row.signal.has_value());
  CHECK(row.argv == sh_command(script));
  CHECK(row.label == "status probe");
  CHECK_FALSE(row.nested);
  CHECK(row.started_at.has_value());
  CHECK(row.ran_ms.has_value());
  CHECK(std::filesystem::equivalent(row.cwd, proj(arena)));
}

TEST_CASE("queue run: a command killed by a signal exits 128 plus the signal and is recorded as signaled",
          "[cmd][agent][queue]") {
  auto const arena  = parity::make_arena("qr_signal");
  auto const queued = run_queue(arena, "killed", sh_command("kill -TERM $$"));
  INFO("stderr:\n" << queued.err);
  CHECK(queued.code == 128 + SIGTERM);

  auto const snap = require_snapshot(arena);
  CHECK(snap.entries.empty());
  REQUIRE(snap.history.size() == 1);
  CHECK(snap.history.front().outcome == hq::history_outcome::signaled);
  CHECK(snap.history.front().signal == SIGTERM);
  CHECK_FALSE(snap.history.front().exit_code.has_value());
}

// ---------------------------------------------------------------------------
// Scenario: Happy path: the command runs in the caller's directory and
// environment
// ---------------------------------------------------------------------------

TEST_CASE("queue run: the command runs in the caller's directory with the caller's environment", "[cmd][agent][queue]") {
  auto const arena = parity::make_arena("qr_env");
  auto       env   = parity::pinned_env(arena.cpp_root);
  env.push_back(pinned_var{.name = "QUEUE_PROBE_VAR", .value = "caller value"});

  auto const args = queue_args(sh_command("pwd -P; printf '%s\\n' \"$QUEUE_PROBE_VAR\"; printf '%s\\n' \"$PLANAR_QUEUE_SLOT\""));
  auto const got  = parity::run_pinned(agent_bin(), args, arena.cpp_root, "env", env);
  INFO("stderr:\n" << got.err);
  REQUIRE(got.code == 0);

  auto const expected = std::format("{}\ncaller value\n1\n", std::filesystem::canonical(proj(arena)).string());
  CHECK(got.out == expected);
  CHECK(got.err.empty());
}

// ---------------------------------------------------------------------------
// Scenario: Edge: a second submitter waits for the first
// ---------------------------------------------------------------------------

TEST_CASE("queue run: a second submitter waits for the first and starts only after it ends", "[cmd][agent][queue]") {
  auto const arena = parity::make_arena("qr_wait");
  write_config(arena, k_fast_poll);
  gate       first_gate(arena.cpp_root / "first.fifo");
  auto const started_a = arena.cpp_root / "started_a";
  auto const started_b = arena.cpp_root / "started_b";

  spawned     first;
  spawned     second;
  release_all guard{.gates = {&first_gate}};

  first = spawn_queue(arena, "first",
                      sh_command("echo \"$PLANAR_QUEUE_SLOT\" > \"$1\"; read x < \"$2\"; exit 3",
                                 {started_a.string(), first_gate.path.string()}));
  await_file(started_a);

  // The first entry is running, its child group is recorded, and its
  // submitter keeps refreshing it while the command runs.
  auto const running = await_child_recorded(arena, 1);
  CHECK(running.child_pgid.has_value());
  CHECK(running.child_started.has_value());
  CHECK(running.started_at.has_value());
  CHECK(running.deadline_mono.has_value());
  await_refreshes(arena, 1, 2);

  // The second is submitted while the first runs, and waits.
  // The second records the slot marker it was given.
  second             = spawn_queue(arena, "second", sh_command("echo \"$PLANAR_QUEUE_SLOT\" > \"$1\"", {started_b.string()}));
  auto const waiting = await_entry(arena, 2, hq::entry_state::waiting);
  CHECK_FALSE(waiting.child_pgid.has_value());
  await_refreshes(arena, 2, 3);
  CHECK_FALSE(present(started_b));
  {
    auto const snap = require_snapshot(arena);
    REQUIRE(snap.entries.size() == 2);
    CHECK(snap.entries[0].state == hq::entry_state::running);
    CHECK(snap.entries[1].state == hq::entry_state::waiting);
    CHECK(snap.history.empty());
  }

  first_gate.release();
  auto const a = finish(first);
  auto const b = finish(second);
  INFO("first stderr:\n" << a.err << "second stderr:\n" << b.err);
  CHECK(a.code == 3);
  CHECK(b.code == 0);
  CHECK(present(started_b));

  auto const snap = require_snapshot(arena);
  CHECK(snap.entries.empty());
  REQUIRE(snap.history.size() == 2);
  auto const* one = history_seq(snap, 1);
  auto const* two = history_seq(snap, 2);
  REQUIRE(one != nullptr);
  REQUIRE(two != nullptr);
  CHECK(one->outcome == hq::history_outcome::exited);
  CHECK(one->exit_code == 3);
  CHECK(two->outcome == hq::history_outcome::exited);
  CHECK(two->exit_code == 0);
  // The second's start is not earlier than the first's end.
  REQUIRE(two->started_at.has_value());
  CHECK(*two->started_at >= one->ended_at);

  // PLANAR_QUEUE_SLOT is each entry's OWN sequence number, read from the
  // store rather than assumed: the first command saw its seq, the second
  // (which is not entry 1) saw a different one.
  REQUIRE(two->seq != one->seq);
  CHECK(read_all(started_a) == std::format("{}\n", one->seq));
  CHECK(read_all(started_b) == std::format("{}\n", two->seq));
}

// ---------------------------------------------------------------------------
// Scenario: Happy path: the queue table takes effect (slots = 2)
// ---------------------------------------------------------------------------

TEST_CASE("queue run: [queue] slots = 2 lets two commands run at once and a third waits", "[cmd][agent][queue]") {
  auto const arena = parity::make_arena("qr_slots");
  write_config(arena, "[queue]\nslots = 2\npoll_interval = \"100ms\"\n");
  gate       gate_a(arena.cpp_root / "a.fifo");
  gate       gate_b(arena.cpp_root / "b.fifo");
  auto const started_a = arena.cpp_root / "started_a";
  auto const started_b = arena.cpp_root / "started_b";
  auto const started_c = arena.cpp_root / "started_c";

  spawned     a;
  spawned     b;
  spawned     c;
  release_all guard{.gates = {&gate_a, &gate_b}};

  auto const blocked = [](const std::filesystem::path& started, const gate& g) {
    return sh_command("echo x > \"$1\"; read x < \"$2\"", {started.string(), g.path.string()});
  };
  a = spawn_queue(arena, "a", blocked(started_a, gate_a));
  await_file(started_a);
  b = spawn_queue(arena, "b", blocked(started_b, gate_b));
  await_file(started_b);
  await_entry(arena, 1, hq::entry_state::running);
  await_entry(arena, 2, hq::entry_state::running);

  c = spawn_queue(arena, "c", sh_command("echo c > \"$1\"", {started_c.string()}));
  await_entry(arena, 3, hq::entry_state::waiting);
  await_refreshes(arena, 3, 3);
  CHECK_FALSE(present(started_c));
  {
    auto const snap = require_snapshot(arena);
    REQUIRE(snap.entries.size() == 3);
    CHECK(snap.entries[0].state == hq::entry_state::running);
    CHECK(snap.entries[1].state == hq::entry_state::running);
    CHECK(snap.entries[2].state == hq::entry_state::waiting);
  }

  gate_a.release();
  CHECK(finish(a).code == 0);
  CHECK(finish(c).code == 0);
  CHECK(present(started_c));
  gate_b.release();
  CHECK(finish(b).code == 0);
  auto const snap = require_snapshot(arena);
  CHECK(snap.entries.empty());
  CHECK(snap.history.size() == 3);
}

TEST_CASE("queue run: a slot count changed while a submitter waits is read at its next poll", "[cmd][agent][queue]") {
  auto const arena = parity::make_arena("qr_reload");
  write_config(arena, "[queue]\nslots = 1\npoll_interval = \"100ms\"\n");
  gate       gate_a(arena.cpp_root / "a.fifo");
  auto const started_a = arena.cpp_root / "started_a";
  auto const started_b = arena.cpp_root / "started_b";

  spawned     a;
  spawned     b;
  release_all guard{.gates = {&gate_a}};

  a = spawn_queue(arena, "a", sh_command("echo x > \"$1\"; read x < \"$2\"", {started_a.string(), gate_a.path.string()}));
  await_file(started_a);
  b = spawn_queue(arena, "b", sh_command("echo b > \"$1\"", {started_b.string()}));
  await_entry(arena, 2, hq::entry_state::waiting);
  await_refreshes(arena, 2, 3);
  CHECK_FALSE(present(started_b));

  // The first command is still blocked. Raising the slot count lets the
  // waiting submitter start on its next poll.
  write_config(arena, "[queue]\nslots = 2\npoll_interval = \"100ms\"\n");
  await_file(started_b);
  CHECK(finish(b).code == 0);
  CHECK(present(started_b));
  CHECK(entry_seq(require_snapshot(arena), 1) != nullptr);

  gate_a.release();
  CHECK(finish(a).code == 0);
  CHECK(require_snapshot(arena).history.size() == 2);
}

TEST_CASE("queue run: an unreadable configuration mid-wait is reported once and the previous settings stay",
          "[cmd][agent][queue]") {
  auto const arena = parity::make_arena("qr_badconf");
  write_config(arena, k_fast_poll);
  gate       gate_a(arena.cpp_root / "a.fifo");
  auto const started_a = arena.cpp_root / "started_a";
  auto const started_b = arena.cpp_root / "started_b";

  spawned     a;
  spawned     b;
  release_all guard{.gates = {&gate_a}};

  a = spawn_queue(arena, "a", sh_command("echo x > \"$1\"; read x < \"$2\"", {started_a.string(), gate_a.path.string()}));
  await_file(started_a);
  b = spawn_queue(arena, "b", sh_command("echo b > \"$1\"", {started_b.string()}));
  await_entry(arena, 2, hq::entry_state::waiting);

  write_config(arena, "[queue]\nslots = 0\n");
  // The notice reaches the waiting submitter's standard error.
  REQUIRE(await([&] { return read_all(arena.cpp_root / "b.err").contains("slots"); }));
  await_refreshes(arena, 2, 3);
  // The submitter neither aborted nor changed its behaviour: it still waits.
  CHECK(entry_seq(require_snapshot(arena), 2) != nullptr);
  CHECK_FALSE(present(started_b));

  gate_a.release();
  CHECK(finish(a).code == 0);
  auto const done = finish(b);
  CHECK(done.code == 0);
  CHECK(present(started_b));
  // Once per failure streak, not once per poll.
  auto const notice = std::string_view{"keeping the previous settings"};
  auto       count  = std::size_t{0};
  for (auto at = done.err.find(notice); at != std::string::npos; at = done.err.find(notice, at + 1)) {
    ++count;
  }
  INFO("stderr:\n" << done.err);
  CHECK(count == 1);
  CHECK(done.out.empty());
}

TEST_CASE("queue run: an invalid configuration before the enqueue refuses at 125 and runs nothing", "[cmd][agent][queue]") {
  auto const arena  = parity::make_arena("qr_badstart");
  auto const marker = arena.cpp_root / "marker";
  write_config(arena, "[queue]\nslots = 0\n");

  auto const got = run_queue(arena, "bad", sh_command("touch \"$1\"", {marker.string()}));
  INFO("stderr:\n" << got.err);
  CHECK(got.code == 125);
  CHECK(got.err.contains("slots"));
  CHECK(got.out.empty());
  CHECK_FALSE(present(marker));
  // No entry and no history row: nothing was enqueued.
  if (auto const snap = try_snapshot(arena)) {
    CHECK(snap->entries.empty());
    CHECK(snap->history.empty());
  }
}

// ---------------------------------------------------------------------------
// Scenario: Edge: a killed submitter does not free the slot while its command
// runs
// ---------------------------------------------------------------------------

TEST_CASE("queue run: a killed submitter does not free the slot while its command runs", "[cmd][agent][queue]") {
  auto const arena = parity::make_arena("qr_orphan");
  write_config(arena, k_fast_poll);
  gate       gate_a(arena.cpp_root / "a.fifo");
  auto const started_a = arena.cpp_root / "started_a";
  auto const started_b = arena.cpp_root / "started_b";

  spawned     a;
  spawned     b;
  release_all guard{.gates = {&gate_a}};

  // The command closes its streams (and fd 3, `run_pinned`'s capture pipe)
  // once started: after its submitter is killed, nothing may keep the
  // capture open.
  a = spawn_queue(arena, "a",
                  sh_command("echo x > \"$1\"; exec >/dev/null 2>&1 </dev/null 3>&-; read x < \"$2\"",
                             {started_a.string(), gate_a.path.string()}));
  await_file(started_a);
  auto const running = await_child_recorded(arena, 1);
  REQUIRE(running.child_pgid.has_value());
  REQUIRE(running.pid > 1);
  REQUIRE(running.pid != static_cast<std::int64_t>(::getpid()));
  // The pid is the submitter this case started: the entry records it.
  REQUIRE(::kill(static_cast<pid_t>(running.pid), SIGKILL) == 0);
  CHECK(finish(a).code == 128 + SIGKILL);

  // The submitter is gone, its command is not: a second submitter must not
  // start on top of it.
  b = spawn_queue(arena, "b", sh_command("echo b > \"$1\"", {started_b.string()}));
  await_entry(arena, 2, hq::entry_state::waiting);
  await_refreshes(arena, 2, 4);
  CHECK_FALSE(present(started_b));
  CHECK(entry_seq(require_snapshot(arena), 1) != nullptr);

  // Once the command ends, the second submitter reaps the entry and starts.
  gate_a.release();
  auto const done = finish(b);
  INFO("stderr:\n" << done.err);
  CHECK(done.code == 0);
  CHECK(present(started_b));
  auto const snap = require_snapshot(arena);
  CHECK(snap.entries.empty());
  auto const* one = history_seq(snap, 1);
  REQUIRE(one != nullptr);
  CHECK(one->outcome == hq::history_outcome::abandoned);
  auto const* two = history_seq(snap, 2);
  REQUIRE(two != nullptr);
  CHECK(two->outcome == hq::history_outcome::exited);
}

// ---------------------------------------------------------------------------
// Scenario: Edge: a leftover helper does not hold the slot when the submitter
// is alive
// ---------------------------------------------------------------------------

TEST_CASE("queue run: a helper left in the command's group does not hold the slot while the submitter lives",
          "[cmd][agent][queue]") {
  auto const  arena = parity::make_arena("qr_helper");
  gate        helper_gate(arena.cpp_root / "helper.fifo");
  auto const  pid_file = arena.cpp_root / "helper.pid";
  release_all guard{.gates = {&helper_gate}};

  // The helper closes its inherited streams (and fd 3, the capture pipe
  // `run_pinned` gives the shell) so the capture is not held open, then blocks on the gate in the command's group.
  auto const first = run_queue(arena, "helper",
                               sh_command("( exec >/dev/null 2>&1 </dev/null 3>&-; read x < \"$2\" ) & echo $! > \"$1\"; exit 0",
                                          {pid_file.string(), helper_gate.path.string()}));
  INFO("stderr:\n" << first.err);
  CHECK(first.code == 0);
  {
    auto const snap = require_snapshot(arena);
    CHECK(snap.entries.empty());
    REQUIRE(snap.history.size() == 1);
    CHECK(snap.history.front().outcome == hq::history_outcome::exited);
  }
  // The helper is still running, and the next entry starts anyway.
  auto const helper_pid = std::stoll(read_all(pid_file));
  REQUIRE(helper_pid > 1);
  CHECK(::kill(static_cast<pid_t>(helper_pid), 0) == 0);
  auto const second = run_queue(arena, "after", sh_command("exit 0"));
  CHECK(second.code == 0);
  CHECK(require_snapshot(arena).history.size() == 2);

  helper_gate.release();
}

// ---------------------------------------------------------------------------
// Scenario: Error: no command is a parse failure
// ---------------------------------------------------------------------------

TEST_CASE("queue run: no command is a parse failure and creates no entry", "[cmd][agent][queue]") {
  auto const arena = parity::make_arena("qr_nocmd");

  SECTION("nothing after the verb") {
    auto const got = parity::run_pinned(agent_bin(), std::vector<std::string>{"queue", "run"}, arena.cpp_root, "bare");
    CHECK(got.code == 1);
    CHECK(got.out.empty());
    CHECK_FALSE(got.err.empty());
  }
  SECTION("nothing after the terminator") {
    auto const got = parity::run_pinned(agent_bin(), std::vector<std::string>{"queue", "run", "--"}, arena.cpp_root, "dashes");
    CHECK(got.code == 1);
    CHECK(got.out.empty());
    CHECK_FALSE(got.err.empty());
  }
  // A parse failure never opens the store.
  CHECK_FALSE(present(arena.cpp_root / "agent.db"));
}

// ---------------------------------------------------------------------------
// Scenarios: run limit and wait limit (task hq-timeouts, 7014, with 7053 and
// 7060). `--timeout` bounds how long the command may run (default thirty
// minutes) and `--wait-timeout` bounds how long it may wait (default none).
// ---------------------------------------------------------------------------

namespace {

/// @brief The monotonic clock the entries' deadlines are measured against.
auto mono_now() -> std::int64_t {
  ident::system_clock clock;
  auto const          now = clock.monotonic_ms();
  REQUIRE(now.has_value());
  return *now;
}

/// @brief True when the process group `pgid` has no member. Signal 0 only
/// asks; `pgid` must be a real group this test recorded.
auto group_empty(std::int64_t pgid) -> bool {
  REQUIRE(pgid > 1);
  return ::kill(-static_cast<::pid_t>(pgid), 0) != 0 && errno == ESRCH;
}

/// @brief The script of a command that records it started and then blocks on
/// the FIFO `$2`. With `ignore_term` it ignores SIGTERM first, and records
/// the start only after the trap is in place.
auto blocked_script(bool ignore_term) -> std::string {
  return std::string{ignore_term ? "trap '' TERM; " : ""} + "echo x > \"$1\"; read x < \"$2\"";
}

/// @brief The rows of the store after a run that ended: no entry is left and
/// exactly the given history row exists.
auto only_history(const parity::arena& arena, std::int64_t seq) -> hq::history_row {
  auto const snap = require_snapshot(arena);
  CHECK(snap.entries.empty());
  REQUIRE(snap.history.size() == 1);
  auto const* row = history_seq(snap, seq);
  REQUIRE(row != nullptr);
  return *row;
}

} // namespace

TEST_CASE("queue run: --timeout stops a command that honours SIGTERM, exits 124 and records timeout",
          "[cmd][agent][queue][hq-timeouts]") {
  auto const arena = parity::make_arena("qr_timeout_term");
  write_config(arena, k_fast_poll);
  gate        hold(arena.cpp_root / "hold.fifo");
  auto const  started = arena.cpp_root / "started";
  release_all guard{.gates = {&hold}};

  auto const run = spawn_queue(arena, "limited", sh_command(blocked_script(false), {started.string(), hold.path.string()}), {},
                               {"--timeout", "1s"});
  await_file(started);
  auto const running = await_child_recorded(arena, 1);
  REQUIRE(running.child_pgid.has_value());

  auto const got = finish(run);
  INFO("stderr:\n" << got.err);
  CHECK(got.code == 124);
  CHECK(group_empty(*running.child_pgid));

  auto const row = only_history(arena, 1);
  CHECK(row.outcome == hq::history_outcome::timeout);
  CHECK_FALSE(row.exit_code.has_value());
  // It ran for its limit, not less: a limit that fired at once would end it
  // in a few tens of milliseconds.
  REQUIRE(row.ran_ms.has_value());
  CHECK(*row.ran_ms >= 900);
}

TEST_CASE("queue run: --timeout kills a command that ignores SIGTERM only after the grace period",
          "[cmd][agent][queue][hq-timeouts]") {
  auto const arena = parity::make_arena("qr_timeout_kill");
  // A long poll interval, so only the submitter's own per-tick advance of its
  // stop can send the SIGKILL in time: the next regular poll is twenty
  // seconds away.
  write_config(arena, "[queue]\npoll_interval = \"20s\"\ngrace = \"1s\"\n");
  gate        hold(arena.cpp_root / "hold.fifo");
  auto const  started = arena.cpp_root / "started";
  release_all guard{.gates = {&hold}};

  auto const run = spawn_queue(arena, "stubborn", sh_command(blocked_script(true), {started.string(), hold.path.string()}), {},
                               {"--timeout", "1s"});
  await_file(started);
  auto const running = await_child_recorded(arena, 1);
  REQUIRE(running.child_pgid.has_value());

  auto const got = finish(run);
  INFO("stderr:\n" << got.err);
  CHECK(got.code == 124);
  CHECK(group_empty(*running.child_pgid));

  auto const row = only_history(arena, 1);
  CHECK(row.outcome == hq::history_outcome::timeout);
  // SIGTERM alone cannot have ended it: the limit (1s) plus the grace (1s)
  // had to pass before the SIGKILL.
  REQUIRE(row.ran_ms.has_value());
  CHECK(*row.ran_ms >= 1900);
  CHECK(*row.ran_ms < 10000);
}

namespace {

/// @brief Kills, when the case ends, the one process whose pid the command
/// recorded in `file`: a straggler that a failing case would otherwise leave
/// running for its whole sleep. Signals only that recorded pid.
struct straggler_guard {
  std::filesystem::path file;
  ~straggler_guard() {
    std::ifstream in(file);
    long          pid = 0;
    if (in >> pid && pid > 1) {
      ::kill(static_cast<::pid_t>(pid), SIGKILL);
    }
  }
};

} // namespace

TEST_CASE("queue run: a stopped command keeps its slot until every member of its group is gone",
          "[cmd][agent][queue][hq-timeouts]") {
  auto const arena = parity::make_arena("qr_timeout_group");
  write_config(arena, "[queue]\npoll_interval = \"100ms\"\ngrace = \"1s\"\n");
  gate            hold(arena.cpp_root / "hold.fifo");
  auto const      straggler = arena.cpp_root / "straggler.pid";
  release_all     guard{.gates = {&hold}};
  straggler_guard reap{.file = straggler};

  // The leader honours SIGTERM and dies of it at the limit. It leaves in its
  // group a member that ignores SIGTERM and sleeps far longer than the case,
  // which only the SIGKILL after the grace period can end.
  auto const script = "( trap '' TERM; exec sleep 25 ) & echo \"$!\" > \"$1\"; read x < \"$2\"";
  auto const run =
      spawn_queue(arena, "grouped", sh_command(script, {straggler.string(), hold.path.string()}), {}, {"--timeout", "1s"});
  await_file(straggler);
  auto const running = await_child_recorded(arena, 1);
  REQUIRE(running.child_pgid.has_value());

  auto const got = finish(run);
  INFO("stderr:\n" << got.err);
  CHECK(got.code == 124);

  // The entry was removed, and its row written, only once the group was
  // empty: the straggler was killed, not outlived.
  auto const row = only_history(arena, 1);
  CHECK(group_empty(*running.child_pgid));
  CHECK(row.outcome == hq::history_outcome::timeout);
  REQUIRE(row.ran_ms.has_value());
  CHECK(*row.ran_ms >= 1900);
  CHECK(*row.ran_ms < 20000);
}

TEST_CASE("queue run: --timeout is recorded as the entry's deadline and the default is thirty minutes",
          "[cmd][agent][queue][hq-timeouts]") {
  auto const arena = parity::make_arena("qr_deadline");
  write_config(arena, "[queue]\nslots = 2\npoll_interval = \"100ms\"\n");
  gate        limited_gate(arena.cpp_root / "limited.fifo");
  gate        default_gate(arena.cpp_root / "default.fifo");
  auto const  started_limited = arena.cpp_root / "started_limited";
  auto const  started_default = arena.cpp_root / "started_default";
  release_all guard{.gates = {&limited_gate, &default_gate}};

  constexpr std::int64_t k_five_minutes   = 5LL * 60 * 1000;
  constexpr std::int64_t k_thirty_minutes = 30LL * 60 * 1000;

  auto const before_limited = mono_now();
  auto const limited        = spawn_queue(
      arena, "limited", sh_command("echo x > \"$1\"; read x < \"$2\"", {started_limited.string(), limited_gate.path.string()}),
      {}, {"--timeout", "5m"});
  await_file(started_limited);
  auto const limited_entry = await_entry(arena, 1, hq::entry_state::running);
  auto const after_limited = mono_now();
  REQUIRE(limited_entry.deadline_mono.has_value());
  CHECK(*limited_entry.deadline_mono >= before_limited + k_five_minutes);
  CHECK(*limited_entry.deadline_mono <= after_limited + k_five_minutes);
  CHECK_FALSE(limited_entry.wait_deadline_mono.has_value());

  auto const before_default = mono_now();
  auto const defaulted      = spawn_queue(
      arena, "default", sh_command("echo x > \"$1\"; read x < \"$2\"", {started_default.string(), default_gate.path.string()}));
  await_file(started_default);
  auto const default_entry = await_entry(arena, 2, hq::entry_state::running);
  auto const after_default = mono_now();
  REQUIRE(default_entry.deadline_mono.has_value());
  CHECK(*default_entry.deadline_mono >= before_default + k_thirty_minutes);
  CHECK(*default_entry.deadline_mono <= after_default + k_thirty_minutes);
  CHECK_FALSE(default_entry.wait_deadline_mono.has_value());

  limited_gate.release();
  default_gate.release();
  CHECK(finish(limited).code == 0);
  CHECK(finish(defaulted).code == 0);
}

TEST_CASE("queue run: --wait-timeout removes a waiting entry at its limit, exits 125 and never runs the command",
          "[cmd][agent][queue][hq-timeouts]") {
  auto const arena = parity::make_arena("qr_wait_timeout");
  write_config(arena, k_fast_poll);
  gate        hold(arena.cpp_root / "hold.fifo");
  auto const  started = arena.cpp_root / "started";
  auto const  marker  = arena.cpp_root / "marker";
  release_all guard{.gates = {&hold}};

  auto const holder = spawn_queue(arena, "holder", sh_command(blocked_script(false), {started.string(), hold.path.string()}));
  await_file(started);
  await_entry(arena, 1, hq::entry_state::running);

  auto const before  = mono_now();
  auto const waiter  = spawn_queue(arena, "waiter", sh_command("touch \"$1\"", {marker.string()}), {}, {"--wait-timeout", "1s"});
  auto const waiting = await_entry(arena, 2, hq::entry_state::waiting);
  auto const after   = mono_now();
  // The wait deadline the flag asked for is what the store holds.
  REQUIRE(waiting.wait_deadline_mono.has_value());
  CHECK(*waiting.wait_deadline_mono >= before + 1000);
  CHECK(*waiting.wait_deadline_mono <= after + 1000);
  CHECK_FALSE(waiting.deadline_mono.has_value());

  auto const got = finish(waiter);
  INFO("stderr:\n" << got.err);
  CHECK(got.code == 125);
  CHECK(got.out.empty());
  CHECK_FALSE(present(marker));

  // The waiter is gone from the queue with its one history row; the holder
  // still holds the slot and has no row yet.
  {
    auto const snap = require_snapshot(arena);
    REQUIRE(snap.entries.size() == 1);
    CHECK(snap.entries.front().seq == 1);
    CHECK(snap.entries.front().state == hq::entry_state::running);
    REQUIRE(snap.history.size() == 1);
    auto const* row = history_seq(snap, 2);
    REQUIRE(row != nullptr);
    CHECK(row->outcome == hq::history_outcome::wait_timeout);
    CHECK_FALSE(row->started_at.has_value());
  }

  hold.release();
  CHECK(finish(holder).code == 0);
  CHECK_FALSE(present(marker));
}

TEST_CASE("queue run: a wait limit never cuts short a run that has started", "[cmd][agent][queue][hq-timeouts]") {
  auto const arena = parity::make_arena("qr_wait_started");
  write_config(arena, k_fast_poll);
  gate        hold(arena.cpp_root / "hold.fifo");
  auto const  started = arena.cpp_root / "started";
  release_all guard{.gates = {&hold}};

  // The turn is immediate, so the wait limit is over before it matters; the
  // command outlives it and still exits with its own status.
  auto const run =
      spawn_queue(arena, "run", sh_command("echo x > \"$1\"; read x < \"$2\"; exit 4", {started.string(), hold.path.string()}),
                  {}, {"--wait-timeout", "1s"});
  await_file(started);
  await_entry(arena, 1, hq::entry_state::running);
  await_refreshes(arena, 1, 3);
  std::this_thread::sleep_for(std::chrono::milliseconds(1200));
  CHECK(entry_seq(require_snapshot(arena), 1) != nullptr);
  hold.release();
  auto const got = finish(run);
  CHECK(got.code == 4);
  CHECK(only_history(arena, 1).outcome == hq::history_outcome::exited);
}

TEST_CASE("queue run: an invalid --timeout or --wait-timeout is refused at 2 and runs nothing",
          "[cmd][agent][queue][hq-timeouts]") {
  auto const arena  = parity::make_arena("qr_bad_duration");
  auto const marker = arena.cpp_root / "marker";
  struct bad_case {
    std::string flag;
    std::string value;
  };
  // A bare integer (unit unknown), zero, a negative, an unknown unit, no
  // number, and one past the one-day cap the [queue] durations share.
  std::vector<bad_case> const bad{
      {"--timeout", "5"},   {"--timeout", "0s"},     {"--timeout", "-1s"},      {"--timeout", "5x"},      {"--timeout", "abc"},
      {"--timeout", "25h"}, {"--wait-timeout", "5"}, {"--wait-timeout", "0ms"}, {"--wait-timeout", "1d"}, {"--wait-timeout", ""},
  };
  for (auto const& one : bad) {
    INFO("flag " << one.flag << " value '" << one.value << "'");
    // A value that starts with `-` must be attached, or the parser reads it
    // as a flag of its own and this is a parse failure instead.
    auto const flags = one.value.starts_with('-') ? std::vector<std::string>{one.flag + "=" + one.value}
                                                  : std::vector<std::string>{one.flag, one.value};
    auto const got   = run_queue(arena, "bad", sh_command("touch \"$1\"", {marker.string()}), flags);
    INFO("stderr:\n" << got.err);
    CHECK(got.code == 2);
    CHECK(got.out.empty());
    CHECK(got.err.contains(one.flag));
    CHECK_FALSE(present(marker));
    // Refused before the store is touched: nothing was enqueued or recorded.
    auto const snap = try_snapshot(arena);
    CHECK((!snap.has_value() || (snap->entries.empty() && snap->history.empty())));
  }

  // The shared grammar: every unit, and the cap itself, is accepted.
  for (auto const* good : {"250ms", "30s", "1m", "1h", "24h"}) {
    INFO("value " << good);
    auto const got = run_queue(arena, "good", sh_command("exit 0"), {"--timeout", good, "--wait-timeout", good});
    INFO("stderr:\n" << got.err);
    CHECK(got.code == 0);
  }
}

TEST_CASE("queue run: a command whose own entry carries a stop reason ends with that reason, never signaled",
          "[cmd][agent][queue][hq-timeouts]") {
  // The submitter's own entry is marked by another process (a `queue cancel`
  // of milestone 3, or any poller enforcing a limit), which then SIGTERMs the
  // group. The submitter observes its child killed by a signal, and must
  // still end the entry with the reason the entry carries.
  struct reason_case {
    hq::stop_reason     reason;
    hq::history_outcome outcome;
    int                 code;
    std::string_view    tag;
  };
  std::vector<reason_case> const cases{
      {hq::stop_reason::timeout, hq::history_outcome::timeout, 124, "reason_timeout"},
      {hq::stop_reason::cancelled, hq::history_outcome::cancelled, 125, "reason_cancelled"},
  };
  for (auto const& one : cases) {
    INFO("reason " << hq::to_string(one.reason));
    auto const arena = parity::make_arena(std::string{one.tag});
    write_config(arena, k_fast_poll);
    gate        hold(arena.cpp_root / "hold.fifo");
    auto const  started = arena.cpp_root / "started";
    release_all guard{.gates = {&hold}};

    auto const run = spawn_queue(arena, "marked", sh_command(blocked_script(false), {started.string(), hold.path.string()}));
    await_file(started);
    auto const running = await_child_recorded(arena, 1);
    REQUIRE(running.child_pgid.has_value());

    // The marker and the SIGTERM come from this process, through the same
    // engine step `queue cancel` will use.
    auto opened = planar::db::agent::open_agent_db_at(arena.cpp_root / "agent.db");
    REQUIRE(opened.has_value());
    ident::system_clock clock;
    hq::canceller const who{.vendor = "claude", .role = "operator", .pid = static_cast<std::int64_t>(::getpid())};
    auto const          begun = hq::begin_terminate(
        *opened,
        hq::begin_terminate_request{.seq          = 1,
                                    .reason       = one.reason,
                                    .cancelled_by = one.reason == hq::stop_reason::cancelled ? std::optional{who} : std::nullopt,
                                    .host_id      = ident::host_identity(ident::native_identity_source())},
        clock, hq::system_process_probe(), hq::system_group_signaller());
    REQUIRE(begun.has_value());
    REQUIRE(begun->status == hq::begin_status::marked);

    auto const got = finish(run);
    INFO("stderr:\n" << got.err);
    CHECK(got.code == one.code);
    auto const row = only_history(arena, 1);
    CHECK(row.outcome == one.outcome);
    CHECK_FALSE(row.signal.has_value());
    if (one.reason == hq::stop_reason::cancelled) {
      CHECK(row.cancelled_by == who);
    }
  }
}

// ---------------------------------------------------------------------------
// Scenario: Edge: the queue works when the main database is unusable
// ---------------------------------------------------------------------------

TEST_CASE("queue run: works while the main database's schema is ahead of the binary", "[cmd][agent][queue]") {
  auto const arena   = parity::make_arena("qr_maindb");
  auto const main_db = arena.cpp_root / "planar.db";
  {
    auto opened = planar::db::connection::open(main_db.string());
    REQUIRE(opened.has_value());
    REQUIRE(
        opened->execute("create table schema_migrations (version integer primary key, description text not null);").has_value());
    REQUIRE(
        opened->execute("insert into schema_migrations (version, description) values (99999, 'from the future');").has_value());
  }
  auto const before = read_all(main_db);
  REQUIRE_FALSE(before.empty());

  auto const got = run_queue(arena, "maindb", sh_command("exit 0"));
  INFO("stderr:\n" << got.err);
  CHECK(got.code == 0);
  CHECK(got.err.empty());
  CHECK(read_all(main_db) == before);
  CHECK(require_snapshot(arena).history.size() == 1);
}

TEST_CASE("queue run: never creates the main database", "[cmd][agent][queue]") {
  auto const arena = parity::make_arena("qr_nomaindb");
  REQUIRE_FALSE(present(arena.cpp_root / "planar.db"));
  auto const got = run_queue(arena, "nomain", sh_command("exit 0"));
  INFO("stderr:\n" << got.err);
  CHECK(got.code == 0);
  CHECK(got.err.empty());
  CHECK_FALSE(present(arena.cpp_root / "planar.db"));
  CHECK(require_snapshot(arena).history.size() == 1);
}

// ---------------------------------------------------------------------------
// Scenario: Edge: old history and its log files are pruned at enqueue (7046)
// ---------------------------------------------------------------------------

TEST_CASE("queue run: old history rows and their log files are pruned when a command is enqueued", "[cmd][agent][queue]") {
  auto const arena = parity::make_arena("qr_prune");
  write_config(arena, "[queue]\nhistory_days = 1\n");

  REQUIRE(run_queue(arena, "old", sh_command("exit 0")).code == 0);
  REQUIRE(run_queue(arena, "recent", sh_command("exit 0")).code == 0);
  auto const old_log    = arena.cpp_root / "old.log";
  auto const recent_log = arena.cpp_root / "recent.log";
  {
    std::ofstream(old_log) << "old\n";
    std::ofstream(recent_log) << "recent\n";
    auto opened = planar::db::agent::open_agent_db_at(arena.cpp_root / "agent.db");
    REQUIRE(opened.has_value());
    // Row 1 ended three days ago; row 2 just now. Both name a log file.
    REQUIRE(opened
                ->execute(std::format("update queue_history set ended_at = ended_at - {}, log_path = '{}' where seq = 1;",
                                      3 * hq::k_ms_per_day, old_log.string()))
                .has_value());
    REQUIRE(
        opened->execute(std::format("update queue_history set log_path = '{}' where seq = 2;", recent_log.string())).has_value());
  }

  REQUIRE(run_queue(arena, "third", sh_command("exit 0")).code == 0);
  auto const snap = require_snapshot(arena);
  CHECK(history_seq(snap, 1) == nullptr);
  CHECK(history_seq(snap, 2) != nullptr);
  CHECK(history_seq(snap, 3) != nullptr);
  CHECK_FALSE(present(old_log));
  CHECK(present(recent_log));
}

// ---------------------------------------------------------------------------
// Task 7055: the handler surfaces what the stopping steps could not do
// ---------------------------------------------------------------------------

namespace {

/// @brief Seeds a running, terminating entry of this process on this host
/// whose child group is empty, and a trigger that makes its history insert
/// fail, so ending it fails every time it is tried.
auto seed_unendable_entry(const parity::arena& arena) -> std::int64_t {
  auto opened = planar::db::agent::open_agent_db_at(arena.cpp_root / "agent.db");
  REQUIRE(opened.has_value());
  auto& conn = *opened;

  ident::system_clock clock;
  auto const          me      = static_cast<std::int64_t>(::getpid());
  auto const          started = ident::process_start_time(me);
  REQUIRE(started.has_value());
  REQUIRE(started->has_value());
  auto const now = clock.monotonic_ms();
  REQUIRE(now.has_value());

  auto const seq = hq::enqueue(conn, hq::enqueue_request{
                                         .host_id        = ident::host_identity(ident::native_identity_source()),
                                         .pid            = me,
                                         .pid_started    = static_cast<std::int64_t>(**started),
                                         .cwd            = "/seeded",
                                         .argv           = {"seeded"},
                                         .enqueued_at    = clock.wall_ms(),
                                         .refreshed_mono = *now,
                                     });
  REQUIRE(seq.has_value());
  REQUIRE(conn.execute(std::format("update queue_entries set state = 'running', started_at = {}, child_pgid = 2000000000, "
                                   "child_started = 1, terminating_since_mono = 1, terminate_reason = 'timeout' "
                                   "where seq = {};",
                                   clock.wall_ms(), *seq))
              .has_value());
  REQUIRE(conn.execute(std::format("create trigger refuse_history before insert on queue_history when new.seq = {} "
                                   "begin select raise(abort, 'history refused'); end;",
                                   *seq))
              .has_value());
  return *seq;
}

} // namespace

TEST_CASE("queue run: a terminating entry that cannot be ended is reported on stderr and the command still runs",
          "[cmd][agent][queue]") {
  auto const arena = parity::make_arena("qr_advance");
  // A second slot, so the seeded entry (which keeps its slot while it is
  // terminating) does not hold this submitter back; a long staleness window
  // so the seeded entry stays live.
  write_config(arena, "[queue]\nslots = 2\npoll_interval = \"100ms\"\nstale_after = \"1h\"\n");
  // Create and migrate the store first, with a command that ends at once.
  REQUIRE(run_queue(arena, "prime", sh_command("exit 0")).code == 0);
  auto const stuck = seed_unendable_entry(arena);

  // Bounded: a regression in slot counting must fail this case, not hang it.
  auto const run = spawn_queue(arena, "advance", sh_command("echo done"));
  auto const got = finish(run);
  INFO("stderr:\n" << got.err);
  CHECK(got.code == 0);
  CHECK(got.out == "done\n");
  CHECK(got.err.contains("history refused"));
  CHECK(got.err.contains(std::format("{}", stuck)));
  // The seeded entry is still there, and this run's own entry is not.
  auto const snap = require_snapshot(arena);
  REQUIRE(snap.entries.size() == 1);
  CHECK(snap.entries.front().seq == stuck);
  CHECK(history_seq(snap, stuck + 1) != nullptr);
}

// ---------------------------------------------------------------------------
// Scenarios: signals are forwarded and the entry is always removed (task
// hq-signal-forwarding; tech spec 647 § Signals are forwarded and the entry is
// always removed).
//
// A signal to a WAITING submitter removes its entry and runs nothing; a signal
// to a RUNNING submitter reaches the command's group and the submitter then
// reports what the command did. The submitter's pid is the one the case
// recorded when it started it (`spawned::pid`), never a name lookup.
// ---------------------------------------------------------------------------

namespace {

/// @brief The signal number's name as a test label.
auto signal_label(int sig) -> std::string {
  return sig == SIGINT ? "int" : sig == SIGTERM ? "term" : "hup";
}

/// @brief The shell's name for `sig`, for a `trap` line.
auto trap_name(int sig) -> std::string {
  return sig == SIGINT ? "INT" : sig == SIGTERM ? "TERM" : "HUP";
}

/// @brief The script of a command that traps `sig`, appends `got` to `$2`, and
/// either exits 42 (`exit_on_signal`) or carries on. It records that it is
/// ready in `$1` only once the trap is in place, and otherwise waits, in short
/// naps so a trapped signal is taken promptly by any shell, for the file `$3`
/// to appear, at most sixty seconds, so a command a failed case leaves behind
/// ends by itself.
auto trapping_script(int sig, bool exit_on_signal) -> std::string {
  return std::format("trap 'echo got >> \"$2\"{}' {}; echo ready > \"$1\"; n=0; while [ ! -e \"$3\" ] && [ $n -lt 1200 ]; do "
                     "sleep 0.05; n=$((n+1)); done; exit 0",
                     exit_on_signal ? "; exit 42" : "", trap_name(sig));
}

/// @brief Creates `file` when it goes out of scope, so a command that waits for
/// that file ends even when the case failed before it got there. Declare it
/// after the `spawned` it serves, so it runs first.
struct touch_on_exit {
  std::filesystem::path file;
  ~touch_on_exit() {
    std::ofstream out(file);
  }
};

/// @brief The CPU time the process `pid` has used, in seconds, from `ps`, or
/// nothing when it cannot be read (the process is gone). Whole-second
/// resolution on Linux, hundredths on macOS.
auto cpu_seconds(std::int64_t pid) -> std::optional<double> {
  std::string text;
  auto* const pipe = ::popen(std::format("ps -o time= -p {} 2>/dev/null", pid).c_str(), "r");
  if (pipe == nullptr) {
    return std::nullopt;
  }
  char buffer[128];
  while (std::fgets(buffer, sizeof buffer, pipe) != nullptr) {
    text += buffer;
  }
  ::pclose(pipe);
  std::erase_if(text, [](char c) { return c == ' ' || c == '\n'; });
  if (text.empty()) {
    return std::nullopt;
  }
  if (auto const dash = text.find('-'); dash != std::string::npos) {
    text.erase(0, dash + 1);
  }
  double      seconds = 0;
  std::size_t begin   = 0;
  while (begin <= text.size()) {
    auto const colon = text.find(':', begin);
    auto const part  = text.substr(begin, colon == std::string::npos ? std::string::npos : colon - begin);
    double     value = 0;
    if (std::from_chars(part.data(), part.data() + part.size(), value).ec != std::errc{}) {
      return std::nullopt;
    }
    seconds = seconds * 60 + value;
    if (colon == std::string::npos) {
      break;
    }
    begin = colon + 1;
  }
  return seconds;
}

/// @brief The pid of the submitter of `run`, checked to be the process the
/// store recorded for entry `seq` and not this test.
auto submitter_pid(const spawned& run, const parity::arena& arena, std::int64_t seq) -> ::pid_t {
  auto const  snap = require_snapshot(arena);
  auto const* live = entry_seq(snap, seq);
  REQUIRE(live != nullptr);
  REQUIRE(run.pid > 1);
  REQUIRE(live->pid == run.pid);
  REQUIRE(run.pid != static_cast<std::int64_t>(::getpid()));
  return static_cast<::pid_t>(run.pid);
}

/// @brief A waiting submitter that gets `sig` removes its entry, runs nothing,
/// records why (naming the sending process as the canceller), exits 125, and
/// leaves the queue free for the next entry.
void waiter_is_signalled(int sig) {
  auto const arena = parity::make_arena(std::format("qr_sigwait_{}", signal_label(sig)));
  write_config(arena, k_fast_poll);
  gate       hold(arena.cpp_root / "hold.fifo");
  auto const started_a = arena.cpp_root / "started_a";
  auto const ran_b     = arena.cpp_root / "ran_b";
  auto const ran_c     = arena.cpp_root / "ran_c";

  spawned     a;
  spawned     b;
  spawned     c;
  release_all guard{.gates = {&hold}};

  a = spawn_queue(arena, "a", sh_command(blocked_script(false), {started_a.string(), hold.path.string()}), {}, {}, true);
  await_file(started_a);
  await_child_recorded(arena, 1);
  b = spawn_queue(arena, "b", sh_command("touch \"$1\"", {ran_b.string()}), {}, {}, true);
  await_entry(arena, 2, hq::entry_state::waiting);
  await_refreshes(arena, 2, 2);
  c = spawn_queue(arena, "c", sh_command("touch \"$1\"", {ran_c.string()}), {}, {}, true);
  await_entry(arena, 3, hq::entry_state::waiting);

  auto const victim = submitter_pid(b, arena, 2);
  REQUIRE(::kill(victim, sig) == 0);

  auto const got = finish(b);
  INFO("stderr:\n" << got.err);
  // The command never ran: the queue's cancelled exit, not 128 plus the signal.
  CHECK(got.code == 125);
  CHECK_FALSE(present(ran_b));
  CHECK(got.err.contains("not run"));
  {
    // The entry is gone and its row says why; the others are untouched.
    auto const snap = require_snapshot(arena);
    CHECK(entry_seq(snap, 2) == nullptr);
    REQUIRE(entry_seq(snap, 1) != nullptr);
    CHECK(entry_seq(snap, 1)->state == hq::entry_state::running);
    REQUIRE(entry_seq(snap, 3) != nullptr);
    CHECK(entry_seq(snap, 3)->state == hq::entry_state::waiting);
    auto const* row = history_seq(snap, 2);
    REQUIRE(row != nullptr);
    CHECK(row->outcome == hq::history_outcome::cancelled);
    REQUIRE(row->cancelled_by.has_value());
    // The canceller is whoever sent the signal (task 7071): this test process,
    // read from the signal's `si_pid`, not the submitter that recorded it.
    CHECK(row->cancelled_by->pid == static_cast<std::int64_t>(::getpid()));
    CHECK(row->cancelled_by->pid != static_cast<std::int64_t>(victim));
    CHECK_FALSE(row->started_at.has_value());
    CHECK_FALSE(row->exit_code.has_value());
  }

  // The queue is free for the entry behind it: once the running one ends, the
  // third starts (it does not wait for the removed one).
  hold.release();
  CHECK(finish(a).code == 0);
  auto const third = finish(c);
  INFO("third stderr:\n" << third.err);
  CHECK(third.code == 0);
  CHECK(present(ran_c));
  CHECK_FALSE(present(ran_b));
  auto const snap = require_snapshot(arena);
  CHECK(snap.entries.empty());
  REQUIRE(snap.history.size() == 3);
  CHECK(history_seq(snap, 1)->outcome == hq::history_outcome::exited);
  CHECK(history_seq(snap, 3)->outcome == hq::history_outcome::exited);
}

/// @brief A running submitter that gets `sig` forwards it to the command, which
/// traps it and exits 42; the submitter exits 42 and records `exited`.
void running_command_traps_forwarded(int sig) {
  auto const arena = parity::make_arena(std::format("qr_sigfwd_{}", signal_label(sig)));
  write_config(arena, k_fast_poll);
  auto const ready  = arena.cpp_root / "ready";
  auto const marker = arena.cpp_root / "marker";
  auto const stop   = arena.cpp_root / "stop";

  auto run = spawn_queue(arena, "run", sh_command(trapping_script(sig, true), {ready.string(), marker.string(), stop.string()}),
                         {}, {}, true);
  touch_on_exit stopper{stop};
  await_file(ready);
  auto const running = await_child_recorded(arena, 1);
  REQUIRE(running.child_pgid.has_value());
  auto const victim = submitter_pid(run, arena, 1);
  REQUIRE(::kill(victim, sig) == 0);

  auto const got = finish(run);
  INFO("stderr:\n" << got.err);
  // The command saw the signal, and the submitter reports what the command did
  // with it.
  CHECK(got.code == 42);
  CHECK(read_all(marker) == "got\n");
  auto const row = only_history(arena, 1);
  CHECK(row.outcome == hq::history_outcome::exited);
  CHECK(row.exit_code == 42);
  CHECK_FALSE(row.signal.has_value());
  CHECK(group_empty(*running.child_pgid));
}

} // namespace

TEST_CASE("queue run: SIGTERM to a waiting submitter removes its entry and runs nothing", "[cmd][agent][queue][hq-signals]") {
  waiter_is_signalled(SIGTERM);
}

TEST_CASE("queue run: SIGINT to a waiting submitter removes its entry and runs nothing", "[cmd][agent][queue][hq-signals]") {
  waiter_is_signalled(SIGINT);
}

TEST_CASE("queue run: SIGHUP to a waiting submitter removes its entry and runs nothing", "[cmd][agent][queue][hq-signals]") {
  waiter_is_signalled(SIGHUP);
}

TEST_CASE("queue run: SIGINT to a running submitter reaches the command, which traps it", "[cmd][agent][queue][hq-signals]") {
  running_command_traps_forwarded(SIGINT);
}

TEST_CASE("queue run: SIGTERM to a running submitter reaches the command, which traps it", "[cmd][agent][queue][hq-signals]") {
  running_command_traps_forwarded(SIGTERM);
}

TEST_CASE("queue run: SIGHUP to a running submitter reaches the command, which traps it", "[cmd][agent][queue][hq-signals]") {
  running_command_traps_forwarded(SIGHUP);
}

TEST_CASE("queue run: SIGTERM to a running submitter empties the group, removes the entry and starts the next",
          "[cmd][agent][queue][hq-signals]") {
  auto const arena = parity::make_arena("qr_sigfwd_next");
  write_config(arena, k_fast_poll);
  gate       hold(arena.cpp_root / "hold.fifo");
  auto const started_a = arena.cpp_root / "started_a";
  auto const ran_b     = arena.cpp_root / "ran_b";

  spawned     a;
  spawned     b;
  release_all guard{.gates = {&hold}};

  a                  = spawn_queue(arena, "a", sh_command(blocked_script(false), {started_a.string(), hold.path.string()}));
  auto const running = [&] {
    await_file(started_a);
    return await_child_recorded(arena, 1);
  }();
  REQUIRE(running.child_pgid.has_value());
  b = spawn_queue(arena, "b", sh_command("touch \"$1\"", {ran_b.string()}));
  await_entry(arena, 2, hq::entry_state::waiting);
  CHECK_FALSE(present(ran_b));

  REQUIRE(::kill(submitter_pid(a, arena, 1), SIGTERM) == 0);

  // The command was blocked on a read and has no trap: the forwarded SIGTERM
  // killed it, so the submitter exits 128 + 15 and records `signaled`.
  auto const first = finish(a);
  INFO("stderr:\n" << first.err);
  CHECK(first.code == 128 + SIGTERM);
  CHECK(group_empty(*running.child_pgid));
  auto const second = finish(b);
  INFO("second stderr:\n" << second.err);
  CHECK(second.code == 0);
  CHECK(present(ran_b));

  auto const snap = require_snapshot(arena);
  CHECK(snap.entries.empty());
  REQUIRE(snap.history.size() == 2);
  auto const* one = history_seq(snap, 1);
  auto const* two = history_seq(snap, 2);
  REQUIRE(one != nullptr);
  REQUIRE(two != nullptr);
  CHECK(one->outcome == hq::history_outcome::signaled);
  CHECK(one->signal == SIGTERM);
  CHECK(two->outcome == hq::history_outcome::exited);
  // The next entry started only after the first was removed.
  REQUIRE(two->started_at.has_value());
  CHECK(*two->started_at >= one->ended_at);
}

TEST_CASE("queue run: a command that carries on after the forwarded signal keeps its slot and its own exit status",
          "[cmd][agent][queue][hq-signals]") {
  auto const arena = parity::make_arena("qr_sigfwd_ignored");
  write_config(arena, k_fast_poll);
  auto const ready  = arena.cpp_root / "ready";
  auto const marker = arena.cpp_root / "marker";
  auto const stop   = arena.cpp_root / "stop";
  auto const ran_b  = arena.cpp_root / "ran_b";

  spawned b;
  spawned a =
      spawn_queue(arena, "a", sh_command(trapping_script(SIGTERM, false), {ready.string(), marker.string(), stop.string()}));
  touch_on_exit stopper{stop};
  await_file(ready);
  auto const running = await_child_recorded(arena, 1);
  REQUIRE(running.child_pgid.has_value());
  b = spawn_queue(arena, "b", sh_command("touch \"$1\"", {ran_b.string()}));
  await_entry(arena, 2, hq::entry_state::waiting);

  REQUIRE(::kill(submitter_pid(a, arena, 1), SIGTERM) == 0);
  // The signal reached the command, which took it and went on.
  await_file(marker);

  // Nothing escalates: the spec forwards the signal and stops there. The
  // submitter still supervises, the entry still holds the slot and is not
  // marked terminating, and the next entry is still waiting.
  await_refreshes(arena, 1, 3);
  {
    auto const snap = require_snapshot(arena);
    REQUIRE(entry_seq(snap, 1) != nullptr);
    CHECK(entry_seq(snap, 1)->state == hq::entry_state::running);
    CHECK_FALSE(entry_seq(snap, 1)->terminating_since_mono.has_value());
    CHECK_FALSE(entry_seq(snap, 1)->terminate_reason.has_value());
    CHECK(snap.history.empty());
    CHECK_FALSE(present(ran_b));
    CHECK_FALSE(a.ended());
  }

  // It ends by itself, and the submitter passes its status through.
  {
    std::ofstream out(stop);
  }
  auto const first = finish(a);
  INFO("stderr:\n" << first.err);
  CHECK(first.code == 0);
  CHECK(finish(b).code == 0);
  auto const snap = require_snapshot(arena);
  CHECK(snap.entries.empty());
  auto const* one = history_seq(snap, 1);
  REQUIRE(one != nullptr);
  CHECK(one->outcome == hq::history_outcome::exited);
  CHECK(one->exit_code == 0);
}

TEST_CASE("queue run: a submitter started with SIGINT ignored leaves it ignored and forwards nothing",
          "[cmd][agent][queue][hq-signals]") {
  // A shell starts a background job with SIGINT ignored (no `default_int`
  // here). The submitter must leave that alone, as `nohup` leaves SIGHUP: it
  // installs no handler, so the SIGINT is ignored and nothing reaches the
  // command. The command puts SIGINT back to the default itself and traps it,
  // so a forwarded SIGINT would leave the marker.
  auto const arena = parity::make_arena("qr_sigign");
  write_config(arena, k_fast_poll);
  auto const ready  = arena.cpp_root / "ready";
  auto const marker = arena.cpp_root / "marker";
  auto const stop   = arena.cpp_root / "stop";

  std::vector<std::string> command{"perl",       "-e",           "$SIG{INT} = q(DEFAULT); exec @ARGV",
                                   "sh",         "-c",           trapping_script(SIGINT, true),
                                   "sh",         ready.string(), marker.string(),
                                   stop.string()};
  spawned                  run = spawn_queue(arena, "run", command);
  touch_on_exit            stopper{stop};
  await_file(ready);
  await_child_recorded(arena, 1);
  auto const victim = submitter_pid(run, arena, 1);
  REQUIRE(::kill(victim, SIGINT) == 0);

  // The submitter kept supervising for several more polls, so it took the
  // signal (had it a handler, it would have forwarded it within one tick).
  await_refreshes(arena, 1, 3);
  CHECK_FALSE(run.ended());
  CHECK_FALSE(present(marker));
  {
    auto const snap = require_snapshot(arena);
    REQUIRE(entry_seq(snap, 1) != nullptr);
    CHECK(entry_seq(snap, 1)->state == hq::entry_state::running);
    CHECK(snap.history.empty());
  }

  {
    std::ofstream out(stop);
  }
  auto const got = finish(run);
  INFO("stderr:\n" << got.err);
  CHECK(got.code == 0);
  CHECK_FALSE(present(marker));
  CHECK(only_history(arena, 1).outcome == hq::history_outcome::exited);
}

TEST_CASE("queue run: a signal during the drain of a stopped command does not make the submitter spin",
          "[cmd][agent][queue][hq-signals]") {
  // The leader dies of the run limit's SIGTERM and is reaped; a member that
  // ignores SIGTERM keeps the group, so the submitter drains until the
  // SIGKILL after the grace period. A signal that arrives then must be taken
  // off the relay's pipe: left readable, the wait returns at once on every
  // pass and the drain burns a core for the whole grace period. CPU time, not
  // wall time, is the bound, so load does not disturb it.
  auto const arena = parity::make_arena("qr_sigspin");
  write_config(arena, "[queue]\npoll_interval = \"100ms\"\ngrace = \"4s\"\n");
  gate            hold(arena.cpp_root / "hold.fifo");
  auto const      straggler = arena.cpp_root / "straggler.pid";
  release_all     guard{.gates = {&hold}};
  straggler_guard reap{.file = straggler};

  auto const script = "( trap '' TERM; exec sleep 25 ) & echo \"$!\" > \"$1\"; read x < \"$2\"";
  auto run = spawn_queue(arena, "run", sh_command(script, {straggler.string(), hold.path.string()}), {}, {"--timeout", "1s"});
  await_file(straggler);
  auto const running = await_child_recorded(arena, 1);
  REQUIRE(running.child_pgid.has_value());
  auto const victim = submitter_pid(run, arena, 1);
  auto const leader = static_cast<::pid_t>(*running.child_pgid);

  // Stopping has begun and the leader has been reaped: the submitter is now in
  // the drain, waiting for the straggler.
  REQUIRE(await([&] {
    auto const  snap = try_snapshot(arena);
    auto const* live = snap ? entry_seq(*snap, 1) : nullptr;
    return live != nullptr && live->terminating_since_mono.has_value() && ::kill(leader, 0) != 0 && errno == ESRCH;
  }));
  REQUIRE(::kill(victim, SIGTERM) == 0);

  // Let the drain run for a while (a measurement window, not a
  // synchronisation), then read the submitter's CPU time.
  auto const window = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < window) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  REQUIRE_FALSE(run.ended());
  auto const cpu = cpu_seconds(run.pid);
  REQUIRE(cpu.has_value());
  INFO("submitter CPU seconds: " << *cpu);
  CHECK(*cpu < 1.0);

  auto const got = finish(run);
  INFO("stderr:\n" << got.err);
  CHECK(got.code == 124);
  CHECK(only_history(arena, 1).outcome == hq::history_outcome::timeout);
}

TEST_CASE("queue run: a failing case leaves no live submitter behind", "[cmd][agent][queue][hq-signals]") {
  // 7064: the harness itself. A `spawned` that goes out of scope while its
  // submitter still runs stops it, so an assertion that fails part-way cannot
  // leave a process behind.
  auto const arena = parity::make_arena("qr_reap");
  write_config(arena, k_fast_poll);
  gate         hold(arena.cpp_root / "hold.fifo");
  auto const   started = arena.cpp_root / "started";
  std::int64_t pid     = 0;
  {
    release_all guard{.gates = {}};
    auto const  run = spawn_queue(arena, "run", sh_command(blocked_script(false), {started.string(), hold.path.string()}));
    await_file(started);
    pid = run.pid;
    REQUIRE(pid > 1);
    REQUIRE(::kill(static_cast<::pid_t>(pid), 0) == 0);
  }
  // Out of scope with the command still blocked: the submitter is gone.
  CHECK(::kill(static_cast<::pid_t>(pid), 0) != 0);
  CHECK(errno == ESRCH);
}

// ---------------------------------------------------------------------------
// Scenarios: nested runs and the slot marker (task hq-nested-run, tech spec
// 647 § Nested runs and § The slot marker is advisory).
//
// A queued command reaches `planar-agent queue run` through the built
// binary's absolute path, passed as `$1`, and inherits the submitter's
// pinned environment, so the inner run opens the arena's own store.
// ---------------------------------------------------------------------------

namespace {

/// @brief The arena's pinned map with `PLANAR_QUEUE_SLOT` set to `value`, as a
/// command that inherited a marker would carry it.
auto env_with_slot(const parity::arena& arena, std::string value) -> std::vector<pinned_var> {
  auto vars = parity::pinned_env(arena.cpp_root);
  vars.push_back(pinned_var{.name = "PLANAR_QUEUE_SLOT", .value = std::move(value)});
  return vars;
}

/// @brief The files an outer command that runs one inner command under the
/// queue reports through.
struct nested_files {
  std::filesystem::path outer_started; ///< Written by the outer command: its own `PLANAR_QUEUE_SLOT`.
  std::filesystem::path inner_started; ///< Written by the inner command: its own `PLANAR_QUEUE_SLOT`.
  std::filesystem::path inner_status;  ///< Written by the outer command: the inner run's exit status.
  std::filesystem::path inner_gate;    ///< FIFO the inner command blocks on.
  std::filesystem::path outer_gate;    ///< FIFO the outer command blocks on after the inner run ends.
};

/// @brief The outer command: it records its marker, runs an inner command
/// through `queue run` (which records its own marker, blocks on the inner
/// gate and exits 7), records the inner run's status, blocks on the outer gate
/// and exits with the inner status.
auto nested_outer_command(const nested_files& files) -> std::vector<std::string> {
  return sh_command("echo \"$PLANAR_QUEUE_SLOT\" > \"$6\"; "
                    "\"$1\" queue run -- sh -c 'echo \"$PLANAR_QUEUE_SLOT\" > \"$1\"; read x < \"$2\"; exit 7' sh \"$3\" \"$4\"; "
                    "rc=$?; echo $rc > \"$2\"; read y < \"$5\"; exit $rc",
                    {agent_bin().string(), files.inner_status.string(), files.inner_started.string(), files.inner_gate.string(),
                     files.outer_gate.string(), files.outer_started.string()});
}

auto make_nested_files(const parity::arena& arena) -> nested_files {
  return nested_files{.outer_started = arena.cpp_root / "outer_started",
                      .inner_started = arena.cpp_root / "inner_started",
                      .inner_status  = arena.cpp_root / "inner_status",
                      .inner_gate    = arena.cpp_root / "inner.fifo",
                      .outer_gate    = arena.cpp_root / "outer.fifo"};
}

} // namespace

TEST_CASE("queue run: a queued command's own queue run starts at once and is recorded as nested",
          "[cmd][agent][queue][hq-nested-run]") {
  auto const arena = parity::make_arena("qr_nested");
  write_config(arena, k_fast_poll); // one slot
  auto const  files = make_nested_files(arena);
  gate        inner_gate(files.inner_gate);
  gate        outer_gate(files.outer_gate);
  spawned     outer;
  spawned     third;
  release_all guard{.gates = {&inner_gate, &outer_gate}};

  outer = spawn_queue(arena, "outer", nested_outer_command(files));
  await_file(files.outer_started);
  // The outer command holds the only slot. The inner run starts although the
  // slot is taken; were it queued behind the outer command, nothing would
  // ever write this file.
  await_file(files.inner_started);
  auto const nested = await_child_recorded(arena, 2);
  {
    auto const snap = require_snapshot(arena);
    REQUIRE(snap.entries.size() == 2);
    auto const* parent = entry_seq(snap, 1);
    auto const* inner  = entry_seq(snap, 2);
    REQUIRE(parent != nullptr);
    REQUIRE(inner != nullptr);
    CHECK(parent->state == hq::entry_state::running);
    CHECK_FALSE(parent->parent_seq.has_value());
    CHECK(inner->state == hq::entry_state::running);
    REQUIRE(inner->parent_seq.has_value());
    CHECK(*inner->parent_seq == 1);
    CHECK(inner->started_at.has_value());
    CHECK(inner->deadline_mono.has_value());
    REQUIRE_FALSE(inner->argv.empty());
    CHECK(inner->argv.front() == "sh");
    CHECK(snap.history.empty());
  }
  // The marker each command sees is its own entry's sequence number.
  CHECK(read_all(files.outer_started) == "1\n");
  CHECK(read_all(files.inner_started) == "2\n");
  static_cast<void>(nested);

  // The slot count is unaffected: an ordinary submitter still waits behind
  // the outer command, for as long as it runs.
  third = spawn_queue(arena, "third", sh_command("exit 0"));
  static_cast<void>(await_entry(arena, 3, hq::entry_state::waiting));
  await_refreshes(arena, 3, 3);
  {
    auto const  snap    = require_snapshot(arena);
    auto const* waiting = entry_seq(snap, 3);
    REQUIRE(waiting != nullptr);
    CHECK(waiting->state == hq::entry_state::waiting);
    CHECK_FALSE(waiting->parent_seq.has_value());
  }

  // The inner command ends with 7: the run passes that status through and
  // leaves one history row marked nested under the outer entry.
  inner_gate.release();
  REQUIRE(await([&] { return read_all(files.inner_status).ends_with('\n'); }));
  CHECK(read_all(files.inner_status) == "7\n");
  {
    auto const snap = require_snapshot(arena);
    REQUIRE(snap.history.size() == 1);
    auto const* row = history_seq(snap, 2);
    REQUIRE(row != nullptr);
    CHECK(row->nested);
    REQUIRE(row->parent_seq.has_value());
    CHECK(*row->parent_seq == 1);
    CHECK(row->outcome == hq::history_outcome::exited);
    REQUIRE(row->exit_code.has_value());
    CHECK(*row->exit_code == 7);
    CHECK(entry_seq(snap, 1) != nullptr);
  }

  outer_gate.release();
  auto const a = finish(outer);
  auto const c = finish(third);
  INFO("outer stderr:\n" << a.err << "third stderr:\n" << c.err);
  CHECK(a.code == 7);
  CHECK(c.code == 0);

  auto const snap = require_snapshot(arena);
  CHECK(snap.entries.empty());
  REQUIRE(snap.history.size() == 3);
  auto const* one   = history_seq(snap, 1);
  auto const* two   = history_seq(snap, 2);
  auto const* three = history_seq(snap, 3);
  REQUIRE(one != nullptr);
  REQUIRE(two != nullptr);
  REQUIRE(three != nullptr);
  CHECK_FALSE(one->nested);
  CHECK_FALSE(one->parent_seq.has_value());
  CHECK(one->exit_code == 7);
  CHECK(two->nested);
  CHECK(two->parent_seq == 1);
  CHECK_FALSE(three->nested);
  CHECK_FALSE(three->parent_seq.has_value());
  CHECK(three->exit_code == 0);
  // The ordinary entry started only once the outer one ended.
  REQUIRE(three->started_at.has_value());
  CHECK(*three->started_at >= one->ended_at);
}

TEST_CASE("queue run: a nested entry does not take a slot from an ordinary submitter", "[cmd][agent][queue][hq-nested-run]") {
  auto const arena = parity::make_arena("qr_nested_slots");
  write_config(arena, "[queue]\npoll_interval = \"100ms\"\nslots = 2\n");
  auto const  files = make_nested_files(arena);
  gate        inner_gate(files.inner_gate);
  gate        outer_gate(files.outer_gate);
  gate        third_gate(arena.cpp_root / "third.fifo");
  spawned     outer;
  spawned     third;
  release_all guard{.gates = {&inner_gate, &outer_gate, &third_gate}};

  outer = spawn_queue(arena, "outer", nested_outer_command(files));
  await_file(files.inner_started);
  static_cast<void>(await_child_recorded(arena, 2));

  // Two slots, and the outer entry and its nested entry are both running. An
  // ordinary submitter takes the second slot at once: were the nested entry
  // counted, both slots would be full and it would wait.
  third = spawn_queue(
      arena, "third",
      sh_command("echo x > \"$1\"; read y < \"$2\"", {(arena.cpp_root / "third_started").string(), third_gate.path.string()}));
  await_file(arena.cpp_root / "third_started");
  auto const running = await_child_recorded(arena, 3);
  CHECK_FALSE(running.parent_seq.has_value());
  {
    auto const snap = require_snapshot(arena);
    CHECK(snap.entries.size() == 3);
    CHECK(snap.history.empty());
  }

  inner_gate.release();
  outer_gate.release();
  third_gate.release();
  CHECK(finish(outer).code == 7);
  CHECK(finish(third).code == 0);
  auto const snap = require_snapshot(arena);
  CHECK(snap.entries.empty());
  REQUIRE(snap.history.size() == 3);
  auto const* inner = history_seq(snap, 2);
  REQUIRE(inner != nullptr);
  CHECK(inner->nested);
  CHECK(inner->parent_seq == 1);
}

TEST_CASE("queue run: a nested run inside a nested run completes and each row names its parent",
          "[cmd][agent][queue][hq-nested-run]") {
  auto const arena = parity::make_arena("qr_nested_deep");
  write_config(arena, k_fast_poll);
  // outer -> inner -> innermost, each through `queue run`; the innermost
  // exits 5 and both levels pass it through.
  auto const got = [&] {
    auto run = spawn_queue(arena, "deep",
                           sh_command("\"$1\" queue run -- \"$1\" queue run -- sh -c 'exit 5'; exit $?", {agent_bin().string()}));
    return finish(run);
  }();
  INFO("stderr:\n" << got.err);
  CHECK(got.code == 5);

  auto const snap = require_snapshot(arena);
  CHECK(snap.entries.empty());
  REQUIRE(snap.history.size() == 3);
  auto const* one   = history_seq(snap, 1);
  auto const* two   = history_seq(snap, 2);
  auto const* three = history_seq(snap, 3);
  REQUIRE(one != nullptr);
  REQUIRE(two != nullptr);
  REQUIRE(three != nullptr);
  CHECK_FALSE(one->nested);
  CHECK(two->nested);
  CHECK(two->parent_seq == 1);
  CHECK(three->nested);
  CHECK(three->parent_seq == 2);
  for (auto const* row : {one, two, three}) {
    CHECK(row->outcome == hq::history_outcome::exited);
    CHECK(row->exit_code == 5);
  }
}

TEST_CASE("queue run: a marker that names no live running entry queues normally and is not nested",
          "[cmd][agent][queue][hq-nested-run]") {
  auto const arena = parity::make_arena("qr_forged_marker");
  write_config(arena, k_fast_poll);
  gate        hold(arena.cpp_root / "hold.fifo");
  auto const  held = arena.cpp_root / "held";
  spawned     holder;
  release_all guard{.gates = {&hold}};
  holder = spawn_queue(arena, "holder", sh_command(blocked_script(false), {held.string(), hold.path.string()}));
  await_file(held);
  static_cast<void>(await_child_recorded(arena, 1));

  // With the only slot held, each of these waits: a nonexistent sequence
  // number, garbage, an empty string, a negative number, trailing text, a
  // number too large for the store, zero, and (last) the sequence number of
  // an entry that is itself still waiting.
  std::vector<std::string> markers{"999", "abc", "", "-1", "1x", "99999999999999999999", "0"};
  std::vector<spawned>     runs;
  std::int64_t             seq = 1;
  for (std::size_t i = 0; i <= markers.size(); ++i) {
    auto const value = i < markers.size() ? markers[i] : std::string{"2"};
    INFO("marker '" << value << "'");
    ++seq;
    auto const started = arena.cpp_root / std::format("ran_{}", seq);
    runs.push_back(spawn_queue(arena, std::format("forged{}", seq), sh_command("echo x > \"$1\"", {started.string()}),
                               {pinned_var{.name = "PLANAR_QUEUE_SLOT", .value = value}}));
    auto const waiting = await_entry(arena, seq, hq::entry_state::waiting);
    CHECK_FALSE(waiting.parent_seq.has_value());
    CHECK_FALSE(present(started));
  }
  {
    auto const snap = require_snapshot(arena);
    CHECK(snap.entries.size() == static_cast<std::size_t>(seq));
    CHECK(snap.history.empty());
    for (auto const& e : snap.entries) {
      CHECK_FALSE(e.parent_seq.has_value());
      CHECK(e.state == (e.seq == 1 ? hq::entry_state::running : hq::entry_state::waiting));
    }
  }

  hold.release();
  CHECK(finish(holder).code == 0);
  for (auto& run : runs) {
    auto const got = finish(run);
    INFO("stderr:\n" << got.err);
    CHECK(got.code == 0);
  }
  auto const snap = require_snapshot(arena);
  CHECK(snap.entries.empty());
  REQUIRE(snap.history.size() == static_cast<std::size_t>(seq));
  for (auto const& row : snap.history) {
    CHECK_FALSE(row.nested);
    CHECK_FALSE(row.parent_seq.has_value());
    CHECK(row.outcome == hq::history_outcome::exited);
  }
}

TEST_CASE("queue run: a marker that outlives its parent queues normally", "[cmd][agent][queue][hq-nested-run]") {
  auto const arena = parity::make_arena("qr_stale_marker");
  write_config(arena, k_fast_poll);
  auto const first = run_queue(arena, "first", {"true"});
  REQUIRE(first.code == 0);
  REQUIRE(require_snapshot(arena).history.size() == 1);

  // Entry 1 has ended. A run that still carries its number is an ordinary
  // run: it gets its own entry, is not nested, and passes its status through.
  auto const args = queue_args(sh_command("exit 4"));
  auto const got  = parity::run_pinned(agent_bin(), args, arena.cpp_root, "stale", env_with_slot(arena, "1"));
  INFO("stderr:\n" << got.err);
  CHECK(got.code == 4);
  auto const snap = require_snapshot(arena);
  CHECK(snap.entries.empty());
  REQUIRE(snap.history.size() == 2);
  auto const* row = history_seq(snap, 2);
  REQUIRE(row != nullptr);
  CHECK_FALSE(row->nested);
  CHECK_FALSE(row->parent_seq.has_value());
  CHECK(row->exit_code == 4);
}

TEST_CASE("queue run: a marker naming a fresh running entry on another host queues normally",
          "[cmd][agent][queue][hq-nested-run][hq-nested-host]") {
  // Decision 1209: a marker nests only under an entry on this host identity.
  auto const arena = parity::make_arena("qr_cross_host_marker");
  write_config(arena, "[queue]\nslots = 2\npoll_interval = \"100ms\"\nstale_after = \"1h\"\n");
  REQUIRE(run_queue(arena, "prime", sh_command("exit 0")).code == 0);

  std::int64_t foreign = 0;
  {
    auto opened = planar::db::agent::open_agent_db_at(arena.cpp_root / "agent.db");
    REQUIRE(opened.has_value());
    ident::system_clock clock;
    auto const          now = clock.monotonic_ms();
    REQUIRE(now.has_value());
    auto const seq = hq::enqueue(*opened, hq::enqueue_request{
                                              .host_id        = "another-host-identity",
                                              .pid            = 4'000'000,
                                              .pid_started    = 1,
                                              .cwd            = "/foreign",
                                              .argv           = {"foreign"},
                                              .enqueued_at    = clock.wall_ms(),
                                              .refreshed_mono = *now,
                                          });
    REQUIRE(seq.has_value());
    REQUIRE(opened
                ->execute(std::format("update queue_entries set state = 'running', started_at = {} where seq = {};",
                                      clock.wall_ms(), *seq))
                .has_value());
    foreign = *seq;
  }

  auto const args = queue_args(sh_command("exit 4"));
  auto const got =
      parity::run_pinned(agent_bin(), args, arena.cpp_root, "cross_host", env_with_slot(arena, std::to_string(foreign)));
  INFO("stderr:\n" << got.err);
  CHECK(got.code == 4);
  auto const snap = require_snapshot(arena);
  // The foreign entry is untouched; the run got its own ordinary entry.
  REQUIRE(snap.entries.size() == 1);
  CHECK(snap.entries[0].seq == foreign);
  CHECK(snap.entries[0].state == hq::entry_state::running);
  REQUIRE(snap.history.size() == 2);
  auto const* row = history_seq(snap, foreign + 1);
  REQUIRE(row != nullptr);
  CHECK_FALSE(row->nested);
  CHECK_FALSE(row->parent_seq.has_value());
  CHECK(row->exit_code == 4);
}

namespace {

/// @brief A nested run under a holder that has the only slot: the holder
/// (entry 1) blocks on a FIFO and the nested submitter (entry 2) carries the
/// holder's marker. Kept in one place so the nested timeout and signal cases
/// below start from the same state (task 7078).
struct nested_under_holder {
  parity::arena arena;
  gate          hold;
  spawned       holder;
  release_all   guard;

  explicit nested_under_holder(std::string_view tag)
      : arena(parity::make_arena(tag)), hold((write_config(arena, k_fast_poll), arena.cpp_root / "hold.fifo")),
        guard{.gates = {&hold}} {
    auto const held = arena.cpp_root / "held";
    holder          = spawn_queue(arena, "holder", sh_command(blocked_script(false), {held.string(), hold.path.string()}));
    await_file(held);
    static_cast<void>(await_child_recorded(arena, 1));
  }
};

} // namespace

TEST_CASE("queue run: a nested entry that reaches its own --timeout is stopped, drained and recorded as timeout",
          "[cmd][agent][queue][hq-nested-run][hq-timeouts]") {
  // Task 7078. The nested entry has its own deadline (its own --timeout, never
  // the parent's), and ending at it must empty the nested command's group and
  // leave the parent running and untouched.
  nested_under_holder fx{"qr_nested_timeout"};
  auto const          started = fx.arena.cpp_root / "nested_started";
  auto const          gate_fd = fx.arena.cpp_root / "nested.fifo";
  gate                nested_hold(gate_fd);
  release_all         nested_guard{.gates = {&nested_hold}};

  auto const run = spawn_queue(fx.arena, "nested", sh_command(blocked_script(false), {started.string(), gate_fd.string()}),
                               {pinned_var{.name = "PLANAR_QUEUE_SLOT", .value = "1"}}, {"--timeout", "1s"});
  await_file(started);
  auto const running = await_child_recorded(fx.arena, 2);
  REQUIRE(running.child_pgid.has_value());
  REQUIRE(running.parent_seq.has_value());
  CHECK(*running.parent_seq == 1);

  auto const got = finish(run);
  INFO("stderr:\n" << got.err);
  CHECK(got.code == 124);
  CHECK(group_empty(*running.child_pgid));

  auto const snap = require_snapshot(fx.arena);
  // The nested entry is gone and left one row; the parent still runs.
  CHECK(entry_seq(snap, 2) == nullptr);
  REQUIRE(entry_seq(snap, 1) != nullptr);
  CHECK(entry_seq(snap, 1)->state == hq::entry_state::running);
  CHECK_FALSE(entry_seq(snap, 1)->terminate_reason.has_value());
  REQUIRE(snap.history.size() == 1);
  auto const* row = history_seq(snap, 2);
  REQUIRE(row != nullptr);
  CHECK(row->nested);
  CHECK(row->parent_seq == 1);
  CHECK(row->outcome == hq::history_outcome::timeout);
  CHECK_FALSE(row->exit_code.has_value());
  REQUIRE(row->ran_ms.has_value());
  CHECK(*row->ran_ms >= 900); // its own limit, not an immediate stop

  fx.hold.release();
  CHECK(finish(fx.holder).code == 0);
}

namespace {

/// @brief A signal sent to the submitter of a NESTED run reaches the nested
/// command, which traps it and exits 42; the nested submitter exits 42 and
/// records `exited`, and the holder it runs under is not disturbed.
void nested_command_traps_forwarded(int sig) {
  nested_under_holder fx{std::format("qr_nested_sig_{}", signal_label(sig))};
  auto const          ready  = fx.arena.cpp_root / "ready";
  auto const          marker = fx.arena.cpp_root / "marker";
  auto const          stop   = fx.arena.cpp_root / "stop";

  auto run =
      spawn_queue(fx.arena, "nested", sh_command(trapping_script(sig, true), {ready.string(), marker.string(), stop.string()}),
                  {pinned_var{.name = "PLANAR_QUEUE_SLOT", .value = "1"}}, {}, true);
  touch_on_exit stopper{stop};
  await_file(ready);
  auto const running = await_child_recorded(fx.arena, 2);
  REQUIRE(running.child_pgid.has_value());
  REQUIRE(running.parent_seq.has_value());
  auto const victim = submitter_pid(run, fx.arena, 2);
  REQUIRE(::kill(victim, sig) == 0);

  auto const got = finish(run);
  INFO("stderr:\n" << got.err);
  CHECK(got.code == 42);
  CHECK(read_all(marker) == "got\n");
  CHECK(group_empty(*running.child_pgid));

  auto const snap = require_snapshot(fx.arena);
  CHECK(entry_seq(snap, 2) == nullptr);
  REQUIRE(entry_seq(snap, 1) != nullptr);
  CHECK(entry_seq(snap, 1)->state == hq::entry_state::running); // the holder was not signalled
  REQUIRE(snap.history.size() == 1);
  auto const* row = history_seq(snap, 2);
  REQUIRE(row != nullptr);
  CHECK(row->nested);
  CHECK(row->outcome == hq::history_outcome::exited);
  CHECK(row->exit_code == 42);
  CHECK_FALSE(row->signal.has_value());

  fx.hold.release();
  CHECK(finish(fx.holder).code == 0);
}

} // namespace

TEST_CASE("queue run: SIGTERM to a nested submitter reaches its command, which traps it",
          "[cmd][agent][queue][hq-nested-run][hq-signals]") {
  nested_command_traps_forwarded(SIGTERM);
}

TEST_CASE("queue run: SIGINT to a nested submitter reaches its command, which traps it",
          "[cmd][agent][queue][hq-nested-run][hq-signals]") {
  nested_command_traps_forwarded(SIGINT);
}

// ---------------------------------------------------------------------------
// Task 7017 (hq-missing-entry): a submitter that finds its own entry missing
// reads the history row. Scenarios "a reaped waiter rejoins at the back" and
// "a cancelled waiter does not rejoin" (test spec 649).
// ---------------------------------------------------------------------------

namespace {

/// @brief Ends entry `seq` through the engine from this test process, the way
/// another process (a reaper, or `queue cancel`) would.
void end_from_outside(const parity::arena& arena, std::int64_t seq, hq::history_outcome outcome) {
  auto opened = planar::db::agent::open_agent_db_at(arena.cpp_root / "agent.db");
  REQUIRE(opened.has_value());
  ident::system_clock clock;
  hq::end_request     request{.outcome = outcome, .ended_at = clock.wall_ms()};
  if (outcome == hq::history_outcome::cancelled) {
    request.cancelled_by = hq::canceller{.vendor = "claude", .role = "operator", .pid = 4321};
  }
  auto const ended = hq::end_entry(*opened, seq, request);
  REQUIRE(ended.has_value());
  REQUIRE(*ended == hq::end_result::ended);
}

/// @brief Continues a stopped submitter when it leaves scope, so a failed
/// assertion cannot leave a stopped process behind. Signals only the pid the
/// test recorded, and only while its start time still matches.
struct continue_on_exit {
  const spawned* run = nullptr;
  ~continue_on_exit() {
    if (run != nullptr && run->still_mine() && !run->ended()) {
      ::kill(static_cast<::pid_t>(run->pid), SIGCONT);
    }
  }
};

/// @brief Stops `run` (SIGSTOP) at an instant when it is not inside a write
/// transaction. A process stopped mid-transaction keeps the store's write lock
/// for as long as it is stopped, so no other submitter could ever reap it and
/// the case would time out under load. After each stop the test tries the write
/// lock itself; when that is busy it continues the submitter and tries again.
void stop_outside_a_transaction(const parity::arena& arena, const spawned& run) {
  for (int attempt = 0; attempt < 200; ++attempt) {
    REQUIRE(::kill(static_cast<::pid_t>(run.pid), SIGSTOP) == 0);
    auto opened = planar::db::agent::open_agent_db_at(arena.cpp_root / "agent.db");
    REQUIRE(opened.has_value());
    static_cast<void>(opened->execute("pragma busy_timeout = 100;"));
    auto const locked = opened->execute("begin immediate;");
    if (locked) {
      static_cast<void>(opened->execute("rollback;"));
      return;
    }
    REQUIRE(::kill(static_cast<::pid_t>(run.pid), SIGCONT) == 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  FAIL("the waiter was always inside a write transaction when it was stopped");
}

} // namespace

TEST_CASE("queue run: a cancelled waiter exits 125 without running and does not rejoin",
          "[cmd][agent][queue][hq-missing-entry]") {
  auto const arena = parity::make_arena("qr_missing_cancelled");
  write_config(arena, k_fast_poll);
  gate       hold(arena.cpp_root / "hold.fifo");
  auto const started = arena.cpp_root / "started";
  auto const marker  = arena.cpp_root / "marker";

  spawned     holder;
  spawned     waiter;
  release_all guard{.gates = {&hold}};

  holder = spawn_queue(arena, "holder", sh_command("echo x > \"$1\"; read x < \"$2\"", {started.string(), hold.path.string()}));
  await_file(started);
  await_child_recorded(arena, 1);
  waiter = spawn_queue(arena, "waiter", sh_command("echo x > \"$1\"", {marker.string()}));
  await_entry(arena, 2, hq::entry_state::waiting);

  end_from_outside(arena, 2, hq::history_outcome::cancelled);
  auto const got = finish(waiter);
  INFO("stderr:\n" << got.err);
  CHECK(got.code == 125);
  CHECK_FALSE(present(marker));

  {
    // Nothing with a higher number appeared while the holder still runs.
    auto const snap = require_snapshot(arena);
    REQUIRE(snap.entries.size() == 1);
    CHECK(snap.entries.front().seq == 1);
    REQUIRE(snap.history.size() == 1);
    CHECK(snap.history.front().seq == 2);
    CHECK(snap.history.front().outcome == hq::history_outcome::cancelled);
    CHECK_FALSE(snap.history.front().successor_seq.has_value());
  }
  hold.release();
  CHECK(finish(holder).code == 0);
  CHECK(require_snapshot(arena).history.size() == 2);
}

TEST_CASE("queue run: a waiter stopped until it is reaped rejoins behind the entries that arrived meanwhile",
          "[cmd][agent][queue][hq-missing-entry]") {
  auto const arena = parity::make_arena("qr_missing_rejoin");
  write_config(arena, "[queue]\npoll_interval = \"100ms\"\nstale_after = \"2s\"\n");
  gate       hold(arena.cpp_root / "hold.fifo");
  auto const started = arena.cpp_root / "started";
  auto const order   = arena.cpp_root / "order";

  spawned          holder;
  spawned          waiter;
  spawned          later;
  release_all      guard{.gates = {&hold}};
  continue_on_exit resume;

  holder = spawn_queue(arena, "holder", sh_command("echo x > \"$1\"; read x < \"$2\"", {started.string(), hold.path.string()}));
  await_file(started);
  await_child_recorded(arena, 1);
  waiter = spawn_queue(arena, "waiter", sh_command("echo waiter >> \"$1\"", {order.string()}));
  await_entry(arena, 2, hq::entry_state::waiting);

  // Stop the waiter until its entry is stale and another submitter's poll
  // reaps it. This process recorded the pid, and checks its start time first.
  REQUIRE(waiter.still_mine());
  resume.run = &waiter;
  stop_outside_a_transaction(arena, waiter);
  later = spawn_queue(arena, "later", sh_command("echo later >> \"$1\"", {order.string()}));
  REQUIRE(await([&] {
    auto const snap = try_snapshot(arena);
    return snap && history_seq(*snap, 2) != nullptr;
  }));
  {
    auto const snap = require_snapshot(arena);
    CHECK(history_seq(snap, 2)->outcome == hq::history_outcome::abandoned);
    CHECK_FALSE(history_seq(snap, 2)->successor_seq.has_value()); // nobody rejoined yet
  }

  REQUIRE(::kill(static_cast<::pid_t>(waiter.pid), SIGCONT) == 0);
  std::int64_t successor = 0;
  REQUIRE(await([&] {
    auto const snap = try_snapshot(arena);
    if (!snap || history_seq(*snap, 2) == nullptr || !history_seq(*snap, 2)->successor_seq) {
      return false;
    }
    successor = *history_seq(*snap, 2)->successor_seq;
    return true;
  }));
  CHECK(successor > 2);
  await_entry(arena, successor, hq::entry_state::waiting);

  hold.release();
  auto const a = finish(holder);
  auto const b = finish(waiter);
  auto const c = finish(later);
  INFO("waiter stderr:\n" << b.err << "later stderr:\n" << c.err);
  CHECK(a.code == 0);
  CHECK(b.code == 0);
  CHECK(c.code == 0);
  // The waiter arrived first and was reaped, so it comes back BEHIND the entry
  // that arrived while it was stopped, and its command ran once.
  CHECK(read_all(order) == "later\nwaiter\n");

  auto const snap = require_snapshot(arena);
  CHECK(snap.entries.empty());
  auto const* old_row = history_seq(snap, 2);
  auto const* new_row = history_seq(snap, successor);
  REQUIRE(old_row != nullptr);
  REQUIRE(new_row != nullptr);
  CHECK(old_row->outcome == hq::history_outcome::abandoned);
  CHECK(old_row->successor_seq == successor);
  CHECK(new_row->outcome == hq::history_outcome::exited);
  CHECK(new_row->exit_code == 0);
}

// ---------------------------------------------------------------------------
// Scenarios: notices and the submitter's vendor and role (tasks hq-notices
// and hq-vendor-role, tech spec 647 § CLI surface).
// ---------------------------------------------------------------------------

namespace {

/// @brief The lines of `text`, without their newlines.
auto split_lines(std::string_view text) -> std::vector<std::string> {
  std::vector<std::string> lines;
  while (!text.empty()) {
    auto const end = text.find('\n');
    lines.emplace_back(text.substr(0, end));
    text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
  }
  return lines;
}

/// @brief The lines of `text` that are queue notices, in order.
auto notice_lines(std::string_view text) -> std::vector<std::string> {
  std::vector<std::string> notices;
  for (auto& line : split_lines(text)) {
    if (line.starts_with("queue: ")) {
      notices.push_back(std::move(line));
    }
  }
  return notices;
}

/// @brief The lines of `text` that are not queue notices, in order.
auto other_lines(std::string_view text) -> std::vector<std::string> {
  std::vector<std::string> others;
  for (auto& line : split_lines(text)) {
    if (!line.starts_with("queue: ")) {
      others.push_back(std::move(line));
    }
  }
  return others;
}

/// @brief What a spawned submitter has written to standard error so far.
auto err_so_far(const spawned& run) -> std::string {
  return read_all(run.root / std::format("{}.err", run.tag));
}

/// @brief Waits until a spawned submitter's standard error holds `text`.
void await_err_contains(const spawned& run, std::string_view text) {
  INFO("waiting for " << run.tag << " to write: " << text);
  REQUIRE(await([&] { return err_so_far(run).find(text) != std::string::npos; }));
}

/// @brief `queue run` under the arena's pinned map plus `extra`, which wins
/// over the map for a name it repeats.
auto run_with_env(const parity::arena& arena, std::string_view tag, const std::vector<std::string>& command,
                  std::vector<std::string> flags, std::vector<pinned_var> extra) -> capture {
  auto env = parity::pinned_env(arena.cpp_root);
  for (auto& var : extra) {
    env.push_back(std::move(var));
  }
  auto const args = queue_args(command, std::move(flags));
  return parity::run_pinned(agent_bin(), args, arena.cpp_root, tag, env);
}

/// @brief The vendor and role variables, set to `vendor` and `role`; an empty
/// value is how a case says "not set" without inheriting the operator's own.
auto identity_env(std::string vendor, std::string role) -> std::vector<pinned_var> {
  return {pinned_var{.name = "PLANAR_VENDOR", .value = std::move(vendor)},
          pinned_var{.name = "PLANAR_ROLE", .value = std::move(role)}};
}

} // namespace

TEST_CASE("queue run: --notices tells a waiting submitter its position, then its start and its outcome",
          "[cmd][agent][queue][hq-notices]") {
  auto const arena = parity::make_arena("qr_notices");
  write_config(arena, k_fast_poll);
  gate        hold_a(arena.cpp_root / "hold_a.fifo");
  gate        hold_b(arena.cpp_root / "hold_b.fifo");
  auto const  started_a = arena.cpp_root / "started_a";
  auto const  started_b = arena.cpp_root / "started_b";
  auto const  blocked   = std::string{"echo x > \"$1\"; read x < \"$2\""};
  spawned     a;
  spawned     b;
  spawned     c;
  release_all guard{.gates = {&hold_a, &hold_b}};

  a = spawn_queue(arena, "a", sh_command(blocked, {started_a.string(), hold_a.path.string()}));
  await_file(started_a);
  b = spawn_queue(arena, "b", sh_command(blocked, {started_b.string(), hold_b.path.string()}), {}, {"--notices"});
  await_err_contains(b, "queue: entry 2 waiting at position 1\n");
  c = spawn_queue(arena, "c", sh_command("echo out; echo err >&2; exit 3"), {}, {"--notices"});
  await_err_contains(c, "queue: entry 3 waiting at position 2\n");

  // The first entry ends: B starts and C moves up, and says so while it waits.
  hold_a.release();
  await_file(started_b);
  await_err_contains(c, "queue: entry 3 waiting at position 1\n");
  hold_b.release();

  auto const got_a = finish(a);
  auto const got_b = finish(b);
  auto const got_c = finish(c);
  CHECK(got_a.code == 0);
  CHECK(got_a.err.empty());
  CHECK(got_b.code == 0);
  CHECK(got_b.err == "queue: entry 2 waiting at position 1\nqueue: entry 2 started\nqueue: entry 2 exited with code 0\n");

  INFO("c stderr:\n" << got_c.err);
  CHECK(got_c.code == 3);
  CHECK(notice_lines(got_c.err) == std::vector<std::string>{"queue: entry 3 waiting at position 2",
                                                            "queue: entry 3 waiting at position 1", "queue: entry 3 started",
                                                            "queue: entry 3 exited with code 3"});
  // The command's own line is untouched, and the outcome is the last line.
  CHECK(other_lines(got_c.err) == std::vector<std::string>{"err"});
  CHECK(split_lines(got_c.err).back() == "queue: entry 3 exited with code 3");
  CHECK(got_c.out == "out\n");
}

TEST_CASE("queue run: --notices names a command a signal ended and a command stopped at its run limit",
          "[cmd][agent][queue][hq-notices]") {
  auto const arena = parity::make_arena("qr_notices_ends");
  write_config(arena, k_fast_poll);
  auto const killed = run_queue(arena, "killed", sh_command("kill -TERM $$"), {"--notices"});
  CHECK(killed.code == 128 + SIGTERM);
  CHECK(notice_lines(killed.err) == std::vector<std::string>{"queue: entry 1 started", "queue: entry 1 terminated by signal 15"});
  CHECK(killed.out.empty());

  auto const limited = run_queue(arena, "limited", sh_command("sleep 30"), {"--notices", "--timeout", "300ms"});
  CHECK(limited.code == 124);
  auto const notices = notice_lines(limited.err);
  REQUIRE(notices.size() == 2);
  CHECK(notices.front() == "queue: entry 2 started");
  CHECK(notices.back() == "queue: entry 2 stopped at its run limit");
  CHECK(split_lines(limited.err).back() == notices.back());
}

TEST_CASE("queue run: standard output is the command's output byte for byte, with and without --notices",
          "[cmd][agent][queue][hq-notices]") {
  auto const arena  = parity::make_arena("qr_notices_out");
  auto const script = std::string{"printf 'one\\ntwo'; echo e >&2; printf 'three\\n'"};

  auto const direct = parity::run_pinned("/bin/sh", std::vector<std::string>{"-c", script}, arena.cpp_root, "direct");
  REQUIRE(direct.code == 0);
  auto const plain    = run_queue(arena, "plain", sh_command(script));
  auto const noticing = run_queue(arena, "noticing", sh_command(script), {"--notices"});
  INFO("stderr with notices:\n" << noticing.err);

  CHECK(plain.code == 0);
  CHECK(noticing.code == 0);
  CHECK(plain.out == direct.out);
  CHECK(noticing.out == direct.out);
  CHECK(direct.out == "one\ntwothree\n");

  // Without --notices the happy path's standard error is the command's own.
  CHECK(plain.err == direct.err);
  CHECK(direct.err == "e\n");
  // With it, the command's own lines are unchanged and the notices are added.
  CHECK(other_lines(noticing.err) == split_lines(direct.err));
  CHECK(notice_lines(noticing.err) == std::vector<std::string>{"queue: entry 2 started", "queue: entry 2 exited with code 0"});
  CHECK(split_lines(noticing.err).back() == "queue: entry 2 exited with code 0");
}

TEST_CASE("queue run: without --notices a waiting submitter writes nothing to standard error",
          "[cmd][agent][queue][hq-notices]") {
  auto const arena = parity::make_arena("qr_notices_off");
  write_config(arena, k_fast_poll);
  gate        hold(arena.cpp_root / "hold.fifo");
  auto const  started = arena.cpp_root / "started";
  spawned     a;
  spawned     b;
  release_all guard{.gates = {&hold}};

  a = spawn_queue(arena, "a", sh_command("echo x > \"$1\"; read x < \"$2\"", {started.string(), hold.path.string()}));
  await_file(started);
  b = spawn_queue(arena, "b", sh_command("echo out; echo err >&2"));
  static_cast<void>(await_entry(arena, 2, hq::entry_state::waiting));
  await_refreshes(arena, 2, 3);
  CHECK(err_so_far(b).empty());
  hold.release();
  static_cast<void>(finish(a));
  auto const got = finish(b);
  CHECK(got.code == 0);
  CHECK(got.out == "out\n");
  CHECK(got.err == "err\n");
}

TEST_CASE("queue run: the vendor and role come from the flags, then the environment, and are stored empty otherwise",
          "[cmd][agent][queue][hq-vendor-role]") {
  auto const arena = parity::make_arena("qr_identity");

  // 1: flags only. 2: environment only. 3: both, and the flags win. 4: neither.
  // 5: empty flags with nothing in the environment. 6: empty flags fall back
  // to the environment.
  REQUIRE(
      run_with_env(arena, "flags", sh_command("exit 0"), {"--vendor", "flagv", "--role", "flagr"}, identity_env("", "")).code ==
      0);
  REQUIRE(run_with_env(arena, "env", sh_command("exit 0"), {}, identity_env("envv", "envr")).code == 0);
  REQUIRE(
      run_with_env(arena, "both", sh_command("exit 0"), {"--vendor", "flagv", "--role", "flagr"}, identity_env("envv", "envr"))
          .code == 0);
  REQUIRE(run_with_env(arena, "neither", sh_command("exit 0"), {}, identity_env("", "")).code == 0);
  REQUIRE(run_with_env(arena, "emptyflags", sh_command("exit 0"), {"--vendor", "", "--role", ""}, identity_env("", "")).code ==
          0);
  REQUIRE(
      run_with_env(arena, "emptyflags_env", sh_command("exit 0"), {"--vendor", "", "--role", ""}, identity_env("envv", "envr"))
          .code == 0);

  auto const snap = require_snapshot(arena);
  REQUIRE(snap.history.size() == 6);
  auto const check_row = [&](std::int64_t seq, std::optional<std::string> vendor, std::optional<std::string> role) {
    INFO("history row " << seq);
    auto const* row = history_seq(snap, seq);
    REQUIRE(row != nullptr);
    CHECK(row->vendor == vendor);
    CHECK(row->role == role);
  };
  check_row(1, "flagv", "flagr");
  check_row(2, "envv", "envr");
  check_row(3, "flagv", "flagr");
  check_row(4, std::nullopt, std::nullopt);
  check_row(5, std::nullopt, std::nullopt);
  check_row(6, "envv", "envr");
}

TEST_CASE("queue run: a live entry carries the vendor and role it was submitted with", "[cmd][agent][queue][hq-vendor-role]") {
  auto const arena = parity::make_arena("qr_identity_live");
  write_config(arena, k_fast_poll);
  gate        hold(arena.cpp_root / "hold.fifo");
  auto const  started = arena.cpp_root / "started";
  spawned     run;
  release_all guard{.gates = {&hold}};

  // The flag names the vendor; the role is left to the environment.
  auto env = identity_env("envv", "envr");
  run      = spawn_queue(arena, "run", sh_command("echo x > \"$1\"; read x < \"$2\"", {started.string(), hold.path.string()}),
                         std::move(env), {"--vendor", "flagv"});
  await_file(started);
  auto const live = await_child_recorded(arena, 1);
  CHECK(live.vendor == "flagv");
  CHECK(live.role == "envr");
  hold.release();
  CHECK(finish(run).code == 0);

  auto const  snap = require_snapshot(arena);
  auto const* row  = history_seq(snap, 1);
  REQUIRE(row != nullptr);
  CHECK(row->vendor == "flagv");
  CHECK(row->role == "envr");
}

TEST_CASE("queue run: a nested run records its own vendor and role, from its flags or the inherited environment",
          "[cmd][agent][queue][hq-vendor-role]") {
  auto const arena  = parity::make_arena("qr_identity_nested");
  auto const script = std::string{"\"$1\" queue run --vendor nestedv --role nestedr -- true; "
                                  "\"$1\" queue run -- true"};
  auto const got    = run_with_env(arena, "outer", sh_command(script, {agent_bin().string()}),
                                   {"--vendor", "outerv", "--role", "outerr"}, identity_env("envv", "envr"));
  INFO("stderr:\n" << got.err);
  REQUIRE(got.code == 0);

  auto const snap = require_snapshot(arena);
  REQUIRE(snap.history.size() == 3);
  auto const* outer  = history_seq(snap, 1);
  auto const* first  = history_seq(snap, 2);
  auto const* second = history_seq(snap, 3);
  REQUIRE(outer != nullptr);
  REQUIRE(first != nullptr);
  REQUIRE(second != nullptr);
  CHECK_FALSE(outer->nested);
  CHECK(outer->vendor == "outerv");
  CHECK(outer->role == "outerr");
  CHECK(first->nested);
  CHECK(first->vendor == "nestedv");
  CHECK(first->role == "nestedr");
  CHECK(second->nested);
  CHECK(second->vendor == "envv");
  CHECK(second->role == "envr");
}

TEST_CASE("queue run: a rejoined entry keeps the vendor and role of the one that was reaped",
          "[cmd][agent][queue][hq-vendor-role]") {
  auto const arena = parity::make_arena("qr_identity_rejoin");
  write_config(arena, "[queue]\npoll_interval = \"100ms\"\nstale_after = \"2s\"\n");
  gate       hold(arena.cpp_root / "hold.fifo");
  auto const started = arena.cpp_root / "started";

  spawned          holder;
  spawned          waiter;
  spawned          later;
  release_all      guard{.gates = {&hold}};
  continue_on_exit resume;

  holder = spawn_queue(arena, "holder", sh_command("echo x > \"$1\"; read x < \"$2\"", {started.string(), hold.path.string()}));
  await_file(started);
  static_cast<void>(await_child_recorded(arena, 1));
  waiter =
      spawn_queue(arena, "waiter", sh_command("exit 0"), identity_env("envv", ""), {"--vendor", "rejoinv", "--role", "rejoinr"});
  static_cast<void>(await_entry(arena, 2, hq::entry_state::waiting));

  REQUIRE(waiter.still_mine());
  resume.run = &waiter;
  stop_outside_a_transaction(arena, waiter);
  later = spawn_queue(arena, "later", sh_command("exit 0"));
  REQUIRE(await([&] {
    auto const snap = try_snapshot(arena);
    return snap && history_seq(*snap, 2) != nullptr;
  }));
  REQUIRE(::kill(static_cast<::pid_t>(waiter.pid), SIGCONT) == 0);

  std::int64_t successor = 0;
  REQUIRE(await([&] {
    auto const snap = try_snapshot(arena);
    if (!snap || history_seq(*snap, 2) == nullptr || !history_seq(*snap, 2)->successor_seq) {
      return false;
    }
    successor = *history_seq(*snap, 2)->successor_seq;
    return true;
  }));
  auto const rejoined = await_entry(arena, successor, hq::entry_state::waiting);
  CHECK(rejoined.vendor == "rejoinv");
  CHECK(rejoined.role == "rejoinr");

  hold.release();
  CHECK(finish(holder).code == 0);
  CHECK(finish(waiter).code == 0);
  CHECK(finish(later).code == 0);

  auto const  snap    = require_snapshot(arena);
  auto const* old_row = history_seq(snap, 2);
  auto const* new_row = history_seq(snap, successor);
  REQUIRE(old_row != nullptr);
  REQUIRE(new_row != nullptr);
  CHECK(old_row->vendor == "rejoinv");
  CHECK(old_row->role == "rejoinr");
  CHECK(new_row->vendor == "rejoinv");
  CHECK(new_row->role == "rejoinr");
}

// ---------------------------------------------------------------------------
// Scenarios: with --notices the last line on standard error names the entry
// and how it ended, on every path that leaves after the entry exists (tech
// spec 647 § Exit codes), and a self-interrupt records its vendor and role.
// ---------------------------------------------------------------------------

TEST_CASE("queue run: --notices ends a submitter interrupted before its turn with that outcome, and records who it was",
          "[cmd][agent][queue][hq-notices]") {
  auto const arena = parity::make_arena("qr_notice_interrupt");
  write_config(arena, k_fast_poll);
  gate        hold(arena.cpp_root / "hold.fifo");
  auto const  started = arena.cpp_root / "started";
  spawned     a;
  spawned     b;
  release_all guard{.gates = {&hold}};

  a = spawn_queue(arena, "a", sh_command("echo x > \"$1\"; read x < \"$2\"", {started.string(), hold.path.string()}));
  await_file(started);
  b = spawn_queue(arena, "b", sh_command("exit 0"), identity_env("", ""), {"--notices", "--vendor", "iv", "--role", "ir"});
  await_err_contains(b, "queue: entry 2 waiting at position 1\n");
  REQUIRE(b.still_mine());
  auto const b_pid = b.pid;
  REQUIRE(::kill(static_cast<::pid_t>(b_pid), SIGTERM) == 0);

  auto const got = finish(b);
  INFO("stderr:\n" << got.err);
  CHECK(got.code == 125);
  CHECK(split_lines(got.err).back() == "queue: entry 2 cancelled before its turn");
  hold.release();
  CHECK(finish(a).code == 0);

  auto const  snap = require_snapshot(arena);
  auto const* row  = history_seq(snap, 2);
  REQUIRE(row != nullptr);
  CHECK(row->outcome == hq::history_outcome::cancelled);
  // Vendor and role are the submitter's (a signal cannot carry them); the pid
  // is the sender's, this test process (task 7071).
  CHECK(row->cancelled_by == hq::canceller{.vendor = "iv", .role = "ir", .pid = static_cast<std::int64_t>(::getpid())});
  CHECK(row->cancelled_by->pid != b_pid);
}

TEST_CASE("queue run: --notices ends a submitter whose wait limit passed with that outcome", "[cmd][agent][queue][hq-notices]") {
  auto const arena = parity::make_arena("qr_notice_waitlimit");
  write_config(arena, k_fast_poll);
  gate        hold(arena.cpp_root / "hold.fifo");
  auto const  started = arena.cpp_root / "started";
  spawned     a;
  release_all guard{.gates = {&hold}};

  a = spawn_queue(arena, "a", sh_command("echo x > \"$1\"; read x < \"$2\"", {started.string(), hold.path.string()}));
  await_file(started);
  auto const got = run_queue(arena, "b", sh_command("exit 0"), {"--notices", "--wait-timeout", "300ms"});
  INFO("stderr:\n" << got.err);
  CHECK(got.code == 125);
  CHECK(split_lines(got.err).back() == "queue: entry 2 removed at its wait limit");
  hold.release();
  CHECK(finish(a).code == 0);
}

TEST_CASE("queue run: --notices ends a command that could not be started with that outcome", "[cmd][agent][queue][hq-notices]") {
  auto const arena   = parity::make_arena("qr_notice_notstarted");
  auto const program = arena.cpp_root / "no-shebang";
  {
    std::ofstream out(program, std::ios::binary | std::ios::trunc);
    out << "echo this is not a binary\n";
  }
  std::filesystem::permissions(program, std::filesystem::perms::owner_all);

  auto const got = run_queue(arena, "ns", {program.string()}, {"--notices"});
  INFO("stderr:\n" << got.err);
  CHECK(got.code == 126);
  CHECK(split_lines(got.err).back() == "queue: entry 1 not started");
}

TEST_CASE("queue run: --notices ends a running command another process cancelled with that outcome",
          "[cmd][agent][queue][hq-notices]") {
  auto const arena = parity::make_arena("qr_notice_cancelled");
  write_config(arena, k_fast_poll);
  gate        hold(arena.cpp_root / "hold.fifo");
  auto const  started = arena.cpp_root / "started";
  release_all guard{.gates = {&hold}};

  auto const run =
      spawn_queue(arena, "marked", sh_command(blocked_script(false), {started.string(), hold.path.string()}), {}, {"--notices"});
  await_file(started);
  REQUIRE(await_child_recorded(arena, 1).child_pgid.has_value());
  auto opened = planar::db::agent::open_agent_db_at(arena.cpp_root / "agent.db");
  REQUIRE(opened.has_value());
  ident::system_clock clock;
  hq::canceller const who{.vendor = "claude", .role = "operator", .pid = static_cast<std::int64_t>(::getpid())};
  auto const          begun =
      hq::begin_terminate(*opened,
                          hq::begin_terminate_request{.seq          = 1,
                                                      .reason       = hq::stop_reason::cancelled,
                                                      .cancelled_by = who,
                                                      .host_id      = ident::host_identity(ident::native_identity_source())},
                          clock, hq::system_process_probe(), hq::system_group_signaller());
  REQUIRE(begun.has_value());

  auto const got = finish(run);
  INFO("stderr:\n" << got.err);
  CHECK(got.code == 125);
  CHECK(split_lines(got.err).back() == "queue: entry 1 cancelled");
}

TEST_CASE("queue run: --notices ends a waiting submitter whose entry another process cancelled with that outcome",
          "[cmd][agent][queue][hq-notices]") {
  auto const arena = parity::make_arena("qr_notice_waitcancel");
  write_config(arena, k_fast_poll);
  gate        hold(arena.cpp_root / "hold.fifo");
  auto const  started = arena.cpp_root / "started";
  spawned     a;
  spawned     b;
  release_all guard{.gates = {&hold}};

  a = spawn_queue(arena, "a", sh_command("echo x > \"$1\"; read x < \"$2\"", {started.string(), hold.path.string()}));
  await_file(started);
  b = spawn_queue(arena, "b", sh_command("exit 0"), {}, {"--notices"});
  await_err_contains(b, "queue: entry 2 waiting at position 1\n");

  // Another process removes the waiting entry, as `queue cancel` will.
  auto opened = planar::db::agent::open_agent_db_at(arena.cpp_root / "agent.db");
  REQUIRE(opened.has_value());
  ident::system_clock clock;
  auto const          ended = hq::end_entry(
      *opened, 2,
      hq::end_request{.outcome = hq::history_outcome::cancelled,
                      .cancelled_by =
                          hq::canceller{.vendor = "claude", .role = "operator", .pid = static_cast<std::int64_t>(::getpid())},
                      .ended_at = clock.wall_ms()});
  REQUIRE(ended.has_value());

  auto const got = finish(b);
  INFO("stderr:\n" << got.err);
  CHECK(got.code == 125);
  CHECK(split_lines(got.err).back() == "queue: entry 2 cancelled");
  hold.release();
  CHECK(finish(a).code == 0);
}

TEST_CASE("queue run: --notices tells a rejoined submitter its position again under its new entry number",
          "[cmd][agent][queue][hq-notices]") {
  auto const arena = parity::make_arena("qr_notice_rejoin");
  write_config(arena, "[queue]\npoll_interval = \"100ms\"\nstale_after = \"2s\"\n");
  gate       hold(arena.cpp_root / "hold.fifo");
  auto const started = arena.cpp_root / "started";

  spawned          holder;
  spawned          ahead;
  spawned          waiter;
  spawned          later;
  release_all      guard{.gates = {&hold}};
  continue_on_exit resume;

  holder = spawn_queue(arena, "holder", sh_command("echo x > \"$1\"; read x < \"$2\"", {started.string(), hold.path.string()}));
  await_file(started);
  static_cast<void>(await_child_recorded(arena, 1));
  // One entry waits ahead of the waiter, so the waiter's place is 2, and the
  // same place after it rejoins: the re-notice must not be swallowed as "no
  // change" just because the number is the same.
  ahead = spawn_queue(arena, "ahead", sh_command("exit 0"));
  static_cast<void>(await_entry(arena, 2, hq::entry_state::waiting));
  waiter = spawn_queue(arena, "waiter", sh_command("exit 0"), {}, {"--notices"});
  await_err_contains(waiter, "queue: entry 3 waiting at position 2\n");

  REQUIRE(waiter.still_mine());
  resume.run = &waiter;
  stop_outside_a_transaction(arena, waiter);
  {
    // The entry ahead leaves while the waiter is stopped, and is replaced by
    // a later arrival.
    auto opened = planar::db::agent::open_agent_db_at(arena.cpp_root / "agent.db");
    REQUIRE(opened.has_value());
    ident::system_clock clock;
    REQUIRE(hq::end_entry(*opened, 2,
                          hq::end_request{.outcome      = hq::history_outcome::cancelled,
                                          .cancelled_by = hq::canceller{.vendor = std::nullopt, .role = std::nullopt, .pid = 1},
                                          .ended_at     = clock.wall_ms()})
                .has_value());
  }
  later = spawn_queue(arena, "later", sh_command("exit 0"));
  REQUIRE(await([&] {
    auto const snap = try_snapshot(arena);
    return snap && history_seq(*snap, 3) != nullptr;
  }));
  REQUIRE(::kill(static_cast<::pid_t>(waiter.pid), SIGCONT) == 0);

  std::int64_t successor = 0;
  REQUIRE(await([&] {
    auto const snap = try_snapshot(arena);
    if (!snap || history_seq(*snap, 3) == nullptr || !history_seq(*snap, 3)->successor_seq) {
      return false;
    }
    successor = *history_seq(*snap, 3)->successor_seq;
    return true;
  }));
  // The entry that arrived meanwhile is ahead of the rejoined one: place 2 again.
  await_err_contains(waiter, std::format("queue: entry {} waiting at position 2\n", successor));

  hold.release();
  CHECK(finish(holder).code == 0);
  CHECK(finish(ahead).code == 125);
  auto const got = finish(waiter);
  CHECK(finish(later).code == 0);
  INFO("stderr:\n" << got.err);
  CHECK(got.code == 0);
  CHECK(split_lines(got.err).back() == std::format("queue: entry {} exited with code 0", successor));
}

// ---------------------------------------------------------------------------
// Scenarios: `queue run --detach` (plan 1080, task hq-detach; tech spec 647
// § Submitting, With `--detach`).
//
// A detached submitter is a process the test did not start directly: the
// invoked `queue run` forks it and exits. Every case that makes one records
// its pid and start time from the entry the child inserted, and the
// `detached_submitter` guard stops it on the way out, so a failed assertion
// leaves no submitter behind. Nothing is signalled by name, and never pid 0,
// 1 or a group.
// ---------------------------------------------------------------------------

namespace {

/// @brief The ticket `queue run --detach` prints: the sequence number, then
/// the log path, one per line.
struct ticket {
  std::int64_t seq = 0;
  std::string  path;
};

auto parse_ticket(const std::string& out) -> ticket {
  INFO("ticket output:\n" << out);
  auto const first = out.find('\n');
  REQUIRE(first != std::string::npos);
  auto const second = out.find('\n', first + 1);
  REQUIRE(second != std::string::npos);
  REQUIRE(second + 1 == out.size());
  ticket      result;
  auto const* head = out.data();
  REQUIRE(std::from_chars(head, head + first, result.seq).ec == std::errc{});
  result.path = out.substr(first + 1, second - first - 1);
  return result;
}

/// @brief Owns one detached submitter: stops it, bounded, when it is still
/// the process the entry named.
struct detached_submitter {
  std::int64_t pid     = 0;
  std::int64_t started = 0;

  detached_submitter()                                     = default;
  detached_submitter(const detached_submitter&)            = delete;
  detached_submitter& operator=(const detached_submitter&) = delete;
  ~detached_submitter() {
    stop();
  }

  void record(const hq::entry& entry) {
    pid     = entry.pid;
    started = entry.pid_started;
  }

  [[nodiscard]] auto still_mine() const -> bool {
    if (pid <= 1) {
      return false;
    }
    auto const now = ident::process_start_time(pid);
    return now && now->has_value() && static_cast<std::int64_t>(**now) == started;
  }

  void stop() {
    if (!still_mine()) {
      return;
    }
    ::kill(static_cast<::pid_t>(pid), SIGTERM);
    if (!await([&] { return !still_mine(); }, std::chrono::seconds(10)) && still_mine()) {
      ::kill(static_cast<::pid_t>(pid), SIGKILL);
      await([&] { return !still_mine(); }, std::chrono::seconds(10));
    }
  }
};

/// @brief The process group of `pid`, from `ps`.
auto process_group_of(std::int64_t pid) -> std::optional<std::int64_t> {
  auto* pipe = ::popen(std::format("ps -o pgid= -p {} 2>/dev/null", pid).c_str(), "r");
  if (pipe == nullptr) {
    return std::nullopt;
  }
  std::string text;
  char        buffer[64];
  while (auto const n = std::fread(buffer, 1, sizeof buffer, pipe)) {
    text.append(buffer, n);
  }
  ::pclose(pipe);
  std::int64_t value = 0;
  auto const   start = text.find_first_not_of(" \t");
  if (start == std::string::npos || std::from_chars(text.data() + start, text.data() + text.size(), value).ec != std::errc{}) {
    return std::nullopt;
  }
  return value;
}

auto file_mode(const std::filesystem::path& path) -> unsigned {
  struct stat info{};
  REQUIRE(::stat(path.c_str(), &info) == 0);
  return static_cast<unsigned>(info.st_mode & 07777);
}

auto log_of(const parity::arena& arena, std::int64_t seq) -> std::filesystem::path {
  return arena.cpp_root / "queue-logs" / std::format("{}.log", seq);
}

/// @brief Runs `queue run --detach ...` from a helper that first makes itself
/// a process-group leader; with `kill_group`, the helper then kills its whole
/// group. Returns the invoked process's exit status and streams, read back
/// from files, since the helper cannot report its own.
auto run_detached_from_leader(const parity::arena& arena, std::string tag, const std::vector<std::string>& command,
                              bool kill_group, std::vector<std::string> flags = {"--detach"}) -> capture {
  auto vars = parity::pinned_env(arena.cpp_root);
  vars.push_back(parity::pinned_var{.name = "QD_RC", .value = (arena.cpp_root / (tag + ".rc")).string()});
  parity::require_agent_db_pinned(arena.cpp_root, vars);
  auto const  script = kill_group ? std::string{"setpgrp(0,0); system(@ARGV); open(F, '>'.$ENV{QD_RC}); print F $?>>8, qq(\\n); "
                                                "close F; kill 'KILL', -$$;"}
                                  : std::string{"setpgrp(0,0); exec @ARGV;"};
  std::string line   = "cd " + parity::shell_quote((arena.cpp_root / "proj").string()) + " && ";
  line += parity::pinned_env_prefix(vars) + "perl -e " + parity::shell_quote(script) + " " +
          parity::shell_quote(agent_bin().string());
  for (auto const& arg : queue_args(command, std::move(flags))) {
    line += " " + parity::shell_quote(arg);
  }
  auto const out = arena.cpp_root / (tag + ".out");
  auto const err = arena.cpp_root / (tag + ".err");
  line += " > " + parity::shell_quote(out.string()) + " 2> " + parity::shell_quote(err.string()) + " ; echo $? > " +
          parity::shell_quote((arena.cpp_root / (tag + ".code")).string());
  // The subshell reports a killed job on its own stderr; that is the point of
  // the case, not output for the test log.
  line = "( " + line + " ) 2>/dev/null";
  static_cast<void>(std::system(line.c_str()));
  int code = -1;
  if (kill_group) {
    auto const rc = read_all(arena.cpp_root / (tag + ".rc"));
    std::from_chars(rc.data(), rc.data() + rc.size() - (rc.ends_with('\n') ? 1 : 0), code);
  } else {
    auto const raw = read_all(arena.cpp_root / (tag + ".code"));
    std::from_chars(raw.data(), raw.data() + raw.size() - (raw.ends_with('\n') ? 1 : 0), code);
  }
  return capture{.code = code, .out = read_all(out), .err = read_all(err)};
}

auto split_out_lines(std::string_view text) -> std::vector<std::string> {
  std::vector<std::string> lines;
  while (!text.empty()) {
    auto const end = text.find('\n');
    lines.emplace_back(text.substr(0, end));
    text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
  }
  return lines;
}

} // namespace

TEST_CASE("queue run: --detach returns a ticket at once and the command's output lands in the log",
          "[cmd][agent][queue][hq-detach]") {
  auto const arena = parity::make_arena("qd_happy");
  write_config(arena, k_fast_poll);
  gate               hold(arena.cpp_root / "hold.fifo");
  gate               release(arena.cpp_root / "release.fifo");
  auto const         holder_started = arena.cpp_root / "holder.started";
  release_all        guard{.gates = {&hold, &release}};
  spawned            holder;
  detached_submitter submitter;

  holder =
      spawn_queue(arena, "holder", sh_command("echo x > \"$1\"; read x < \"$2\"", {holder_started.string(), hold.path.string()}));
  await_file(holder_started);
  static_cast<void>(await_child_recorded(arena, 1));

  auto const script    = std::string{"echo out; echo err >&2; [ -z \"$(cat)\" ] && echo stdin-empty; read x < \"$1\"; exit 3"};
  auto const submitted = run_queue(arena, "detached", sh_command(script, {release.path.string()}), {"--detach"});
  INFO("stderr:\n" << submitted.err);
  REQUIRE(submitted.code == 0);
  CHECK(submitted.err.empty());
  auto const ticket = parse_ticket(submitted.out);

  // It returned while another entry still holds the only slot, and the entry
  // it named exists and is waiting, with the log path it printed.
  auto const snap = require_snapshot(arena);
  REQUIRE(entry_seq(snap, 1) != nullptr);
  CHECK(entry_seq(snap, 1)->state == hq::entry_state::running);
  auto const* entry = entry_seq(snap, ticket.seq);
  REQUIRE(entry != nullptr);
  submitter.record(*entry);
  CHECK(entry->state == hq::entry_state::waiting);
  CHECK(entry->log_path == ticket.path);
  CHECK(ticket.path == log_of(arena, ticket.seq).string());

  // The log exists already, is private to its owner, and the submitter that
  // outlived the invoked process is a session leader of its own.
  CHECK(present(ticket.path));
  CHECK(file_mode(ticket.path) == 0600);
  CHECK(submitter.still_mine());
  CHECK(process_group_of(entry->pid) == entry->pid);
  CHECK(std::filesystem::equivalent(entry->cwd, proj(arena)));

  hold.release();
  static_cast<void>(await_entry(arena, ticket.seq, hq::entry_state::running));
  REQUIRE(await([&] { return read_all(ticket.path).ends_with("stdin-empty\n"); }));
  release.release();
  REQUIRE(await([&] {
    auto const now = try_snapshot(arena);
    return now && history_seq(*now, ticket.seq) != nullptr;
  }));
  auto const  done = require_snapshot(arena);
  auto const* row  = history_seq(done, ticket.seq);
  REQUIRE(row != nullptr);
  CHECK(row->outcome == hq::history_outcome::exited);
  CHECK(row->exit_code == 3);
  CHECK(row->log_path == ticket.path);
  CHECK(read_all(ticket.path) == "out\nerr\nstdin-empty\n");
  CHECK(finish(holder).code == 0);
}

TEST_CASE("queue run: --detach hands its ticket back through a pipe that nothing else holds open",
          "[cmd][agent][queue][hq-detach]") {
  // A submitter that kept the invoked process's standard streams (or any
  // descriptor it inherited) would keep a reader waiting for end-of-file
  // until the command ended. The command here runs for a long time, so a
  // reader that returns while the entry still exists proves nothing holds it.
  auto const arena = parity::make_arena("qd_pipe");
  write_config(arena, k_fast_poll);
  detached_submitter submitter;
  auto const         vars = parity::pinned_env(arena.cpp_root);
  parity::require_agent_db_pinned(arena.cpp_root, vars);
  std::string line = "cd " + parity::shell_quote((arena.cpp_root / "proj").string()) + " && " + parity::pinned_env_prefix(vars) +
                     parity::shell_quote(agent_bin().string());
  for (auto const& arg : queue_args(sh_command("sleep 8; echo late"), {"--detach"})) {
    line += " " + parity::shell_quote(arg);
  }
  line += " 2>&1";
  auto* pipe = ::popen(line.c_str(), "r");
  REQUIRE(pipe != nullptr);
  std::string text;
  char        buffer[256];
  while (auto const n = std::fread(buffer, 1, sizeof buffer, pipe)) {
    text.append(buffer, n);
  }
  auto const status = ::pclose(pipe);
  auto const got    = parse_ticket(text);
  CHECK(status == 0);
  auto const  snap  = require_snapshot(arena);
  auto const* entry = entry_seq(snap, got.seq);
  REQUIRE(entry != nullptr); // still there: the reader did not wait for the command
  submitter.record(*entry);
}

TEST_CASE("queue run: a detached submitter survives its caller's process group being killed", "[cmd][agent][queue][hq-detach]") {
  auto const arena = parity::make_arena("qd_group");
  write_config(arena, k_fast_poll);
  gate               release(arena.cpp_root / "release.fifo");
  release_all        guard{.gates = {&release}};
  detached_submitter submitter;

  auto const submitted =
      run_detached_from_leader(arena, "leader", sh_command("read x < \"$1\"; exit 4", {release.path.string()}), true);
  INFO("stderr:\n" << submitted.err);
  REQUIRE(submitted.code == 0);
  auto const ticket = parse_ticket(submitted.out);

  // The whole group of the invoking helper has been killed by now.
  auto const  snap  = require_snapshot(arena);
  auto const* entry = entry_seq(snap, ticket.seq);
  REQUIRE(entry != nullptr);
  submitter.record(*entry);
  CHECK(submitter.still_mine());
  static_cast<void>(await_child_recorded(arena, ticket.seq));

  release.release();
  REQUIRE(await([&] {
    auto const now = try_snapshot(arena);
    return now && history_seq(*now, ticket.seq) != nullptr;
  }));
  auto const done = require_snapshot(arena);
  CHECK(history_seq(done, ticket.seq)->outcome == hq::history_outcome::exited);
  CHECK(history_seq(done, ticket.seq)->exit_code == 4);
}

TEST_CASE("queue run: --detach from a process-group leader returns a ticket and a poll by another entry does not reap it",
          "[cmd][agent][queue][hq-detach]") {
  auto const arena = parity::make_arena("qd_leader");
  write_config(arena, k_fast_poll);
  gate               hold(arena.cpp_root / "hold.fifo");
  release_all        guard{.gates = {&hold}};
  auto const         holder_started = arena.cpp_root / "holder.started";
  spawned            holder;
  spawned            follower;
  detached_submitter submitter;

  holder =
      spawn_queue(arena, "holder", sh_command("echo x > \"$1\"; read x < \"$2\"", {holder_started.string(), hold.path.string()}));
  await_file(holder_started);
  static_cast<void>(await_child_recorded(arena, 1));

  auto const submitted = run_detached_from_leader(arena, "leader", sh_command("exit 0"), false);
  INFO("stderr:\n" << submitted.err);
  REQUIRE(submitted.code == 0);
  auto const  ticket = parse_ticket(submitted.out);
  auto const  snap   = require_snapshot(arena);
  auto const* entry  = entry_seq(snap, ticket.seq);
  REQUIRE(entry != nullptr);
  submitter.record(*entry);
  CHECK(submitter.still_mine());
  CHECK(entry->state == hq::entry_state::waiting);

  // Another submitter queues behind it and polls; the detached entry is live
  // by the recorded pid and start time, so those polls leave it in place.
  follower = spawn_queue(arena, "follower", sh_command("exit 0"));
  static_cast<void>(await_entry(arena, ticket.seq + 1, hq::entry_state::waiting));
  await_refreshes(arena, ticket.seq + 1, 4);
  auto const later = require_snapshot(arena);
  REQUIRE(entry_seq(later, ticket.seq) != nullptr);
  CHECK(history_seq(later, ticket.seq) == nullptr);

  hold.release();
  CHECK(finish(follower).code == 0);
  CHECK(finish(holder).code == 0);
  REQUIRE(await([&] {
    auto const now = try_snapshot(arena);
    return now && history_seq(*now, ticket.seq) != nullptr;
  }));
  CHECK(history_seq(require_snapshot(arena), ticket.seq)->outcome == hq::history_outcome::exited);
}

TEST_CASE("queue run: a refused detached command detaches nothing and prints no ticket", "[cmd][agent][queue][hq-detach]") {
  auto const arena = parity::make_arena("qd_refused");
  {
    std::ofstream plain(proj(arena) / "notexec");
    plain << "#!/bin/sh\n";
  }
  ::chmod((proj(arena) / "notexec").c_str(), 0644);

  struct refusal {
    std::string              tag;
    std::vector<std::string> command;
    std::vector<std::string> flags;
    int                      code;
  };
  std::vector<refusal> const cases{
      {"guard", {"claude"}, {"--detach"}, 2},
      {"missing", {"planar-no-such-program-7021"}, {"--detach"}, 127},
      {"notexec", {"./notexec"}, {"--detach"}, 126},
      {"duration", {"true"}, {"--detach", "--timeout", "0s"}, 2},
      {"waitduration", {"true"}, {"--detach", "--wait-timeout", "12"}, 2},
  };
  for (auto const& c : cases) {
    INFO("case " << c.tag);
    auto const got = run_queue(arena, c.tag, c.command, c.flags);
    INFO("stderr:\n" << got.err);
    CHECK(got.code == c.code);
    CHECK(got.out.empty());
    CHECK_FALSE(got.err.empty());
    // Refused in the invoked process: the store was never opened and no log
    // directory was made, so nothing was forked to do either.
    CHECK_FALSE(present(arena.cpp_root / "agent.db"));
    CHECK_FALSE(present(arena.cpp_root / "queue-logs"));
  }
}

TEST_CASE("queue run: --detach with an unreachable store exits 125 and issues no ticket", "[cmd][agent][queue][hq-detach]") {
  auto const arena = parity::make_arena("qd_store");
  {
    std::ofstream blocker(arena.cpp_root / "blocker");
    blocker << "not a directory\n";
  }
  auto vars = parity::pinned_env(arena.cpp_root);
  for (auto& var : vars) {
    if (var.name == "PLANAR_AGENT_DB") {
      var.value = (arena.cpp_root / "blocker" / "agent.db").string();
    }
  }
  auto const args = queue_args({"true"}, {"--detach"});
  auto const got  = parity::run_pinned(agent_bin(), args, arena.cpp_root, "unreachable", vars);
  INFO("stderr:\n" << got.err);
  CHECK(got.code == 125);
  CHECK(got.out.empty());
  CHECK(got.err.starts_with("error: queue: "));
  CHECK_FALSE(present(arena.cpp_root / "queue-logs"));
}

TEST_CASE("queue run: a detached run that cannot create its log file is refused and leaves no entry",
          "[cmd][agent][queue][hq-detach]") {
  auto const arena = parity::make_arena("qd_nolog");
  // A plain file where the log directory belongs: the directory cannot be made.
  {
    std::ofstream blocker(arena.cpp_root / "queue-logs");
    blocker << "not a directory\n";
  }
  auto const ran = arena.cpp_root / "ran";
  auto const got = run_queue(arena, "nolog", sh_command("echo x > \"$1\"", {ran.string()}), {"--detach"});
  INFO("stderr:\n" << got.err);
  CHECK(got.code == 125);
  CHECK(got.out.empty());
  CHECK(got.err.starts_with("error: queue: "));
  CHECK(got.err.find("log") != std::string::npos);

  // The child opened the store, inserted the entry, failed, and took the
  // entry back out before it reported: no entry, no history row, no command.
  auto const snap = require_snapshot(arena);
  CHECK(snap.entries.empty());
  CHECK(snap.history.empty());
  CHECK_FALSE(present(ran));
}

TEST_CASE("queue run: --detach writes notices and the wait-limit refusal to the log, not to the invoked process",
          "[cmd][agent][queue][hq-detach]") {
  auto const arena = parity::make_arena("qd_notices");
  write_config(arena, k_fast_poll);
  gate               hold(arena.cpp_root / "hold.fifo");
  release_all        guard{.gates = {&hold}};
  auto const         holder_started = arena.cpp_root / "holder.started";
  spawned            holder;
  detached_submitter submitter;

  holder =
      spawn_queue(arena, "holder", sh_command("echo x > \"$1\"; read x < \"$2\"", {holder_started.string(), hold.path.string()}));
  await_file(holder_started);
  static_cast<void>(await_child_recorded(arena, 1));

  auto const submitted = run_queue(arena, "detached", sh_command("exit 0"), {"--detach", "--notices", "--wait-timeout", "300ms"});
  INFO("stderr:\n" << submitted.err);
  REQUIRE(submitted.code == 0);
  CHECK(submitted.err.empty());
  auto const ticket = parse_ticket(submitted.out);
  auto const snap   = require_snapshot(arena);
  REQUIRE(entry_seq(snap, ticket.seq) != nullptr);
  submitter.record(*entry_seq(snap, ticket.seq));

  REQUIRE(await([&] {
    auto const now = try_snapshot(arena);
    return now && history_seq(*now, ticket.seq) != nullptr;
  }));
  CHECK(history_seq(require_snapshot(arena), ticket.seq)->outcome == hq::history_outcome::wait_timeout);
  REQUIRE(await([&] { return read_all(ticket.path).find("removed at its wait limit") != std::string::npos; }));
  auto const lines = split_out_lines(read_all(ticket.path));
  REQUIRE_FALSE(lines.empty());
  CHECK(lines.front() == std::format("queue: entry {} waiting at position 1", ticket.seq));
  CHECK(lines.back() == std::format("queue: entry {} removed at its wait limit", ticket.seq));
  hold.release();
  CHECK(finish(holder).code == 0);
}

namespace {

/// @brief The pids whose parent is `parent`, from `ps`.
auto children_of(std::int64_t parent) -> std::vector<std::int64_t> {
  std::vector<std::int64_t> found;
  auto*                     pipe = ::popen("ps -axo pid=,ppid= 2>/dev/null", "r");
  if (pipe == nullptr) {
    return found;
  }
  std::string text;
  char        buffer[4096];
  while (auto const n = std::fread(buffer, 1, sizeof buffer, pipe)) {
    text.append(buffer, n);
  }
  ::pclose(pipe);
  std::istringstream in(text);
  std::int64_t       pid  = 0;
  std::int64_t       ppid = 0;
  while (in >> pid >> ppid) {
    if (ppid == parent) {
      found.push_back(pid);
    }
  }
  return found;
}

} // namespace

TEST_CASE("queue run: a detached submitter whose caller died before the ticket removes its entry and its log",
          "[cmd][agent][queue][hq-detach]") {
  auto const arena = parity::make_arena("qd_gone");
  write_config(arena, k_fast_poll);
  // Create and migrate the store with an ordinary run, so the lock below
  // stalls the child at its insert and not at the migration.
  REQUIRE(run_queue(arena, "seed", sh_command("exit 0")).code == 0);

  auto opened = planar::db::agent::open_agent_db_at(arena.cpp_root / "agent.db");
  REQUIRE(opened.has_value());
  auto lock = opened->begin_transaction(planar::db::lock_mode::immediate);
  REQUIRE(lock.has_value());

  // The child forks, starts its session and then blocks on the lock we hold,
  // before it can insert. Kill the invoked process once the child exists: the
  // pipe then has no reader when the child comes to write the ticket.
  auto         invoker = spawn_queue(arena, "gone", sh_command("exit 0"), {}, {"--detach"});
  std::int64_t child   = 0;
  REQUIRE(await([&] {
    auto const kids = children_of(invoker.pid);
    if (kids.empty()) {
      return false;
    }
    child = kids.front();
    return true;
  }));
  detached_submitter straggler; // stops the child if the case fails before it ends
  straggler.pid = child;
  if (auto const at = ident::process_start_time(child); at && at->has_value()) {
    straggler.started = static_cast<std::int64_t>(**at);
  }
  REQUIRE(invoker.still_mine());
  REQUIRE(::kill(static_cast<::pid_t>(invoker.pid), SIGKILL) == 0);
  REQUIRE(await([&] { return invoker.ended(); }));

  REQUIRE(lock->commit().has_value()); // The child's insert now goes through.
  REQUIRE(await([&] { return !straggler.still_mine(); }));

  // It found nobody to hand the ticket to: no entry, no history row, no log.
  auto const snap = require_snapshot(arena);
  CHECK(snap.entries.empty());
  CHECK(snap.history.size() == 1); // The seed run's row and nothing else.
  auto logs = std::vector<std::string>{};
  if (present(arena.cpp_root / "queue-logs")) {
    for (auto const& item : std::filesystem::directory_iterator(arena.cpp_root / "queue-logs")) {
      logs.push_back(item.path().filename().string());
    }
  }
  CHECK(logs.empty());
}

TEST_CASE("queue run: a detached run never writes into a log that already exists", "[cmd][agent][queue][hq-detach]") {
  auto const arena = parity::make_arena("qd_excl");
  // A log left by an earlier store: sequence numbers restarted, so the next
  // run is entry 1 and its log path is already taken.
  std::filesystem::create_directories(arena.cpp_root / "queue-logs");
  auto const old = arena.cpp_root / "queue-logs" / "1.log";
  {
    std::ofstream out(old, std::ios::binary);
    out << "old run\n";
  }
  auto const ran = arena.cpp_root / "ran";
  auto const got = run_queue(arena, "excl", sh_command("echo x > \"$1\"", {ran.string()}), {"--detach"});
  INFO("stderr:\n" << got.err);
  CHECK(got.code == 125);
  CHECK(got.out.empty());
  CHECK(got.err.starts_with("error: queue: cannot create the log file"));
  CHECK(read_all(old) == "old run\n"); // Neither appended to nor removed.
  auto const snap = require_snapshot(arena);
  CHECK(snap.entries.empty());
  CHECK(snap.history.empty());
  CHECK_FALSE(present(ran));
}

namespace {

/// @brief The descriptor numbers process `pid` holds open right now, read from
/// the operating system (`proc_pidinfo` on macOS, `/proc/<pid>/fd` on Linux).
/// An unreadable process is a failed case, never an empty answer.
auto open_descriptors_of(std::int64_t pid) -> std::vector<int> {
  std::vector<int> found;
#if defined(__APPLE__)
  auto const bytes = ::proc_pidinfo(static_cast<int>(pid), PROC_PIDLISTFDS, 0, nullptr, 0);
  REQUIRE(bytes > 0);
  std::vector<proc_fdinfo> table(static_cast<std::size_t>(bytes) / sizeof(proc_fdinfo) + 16);
  auto const               got = ::proc_pidinfo(static_cast<int>(pid), PROC_PIDLISTFDS, 0, table.data(),
                                                static_cast<int>(table.size() * sizeof(proc_fdinfo)));
  REQUIRE(got > 0);
  for (std::size_t i = 0; i < static_cast<std::size_t>(got) / sizeof(proc_fdinfo); ++i) {
    found.push_back(table[i].proc_fd);
  }
#else
  std::error_code ec;
  for (auto const& item : std::filesystem::directory_iterator(std::format("/proc/{}/fd", pid), ec)) {
    int        value = -1;
    auto const name  = item.path().filename().string();
    if (std::from_chars(name.data(), name.data() + name.size(), value).ec == std::errc{}) {
      found.push_back(value);
    }
  }
  REQUIRE_FALSE(ec);
#endif
  return found;
}

/// @brief Holds `number` open in this process as a duplicate of /dev/null and
/// closes it on the way out.
struct held_descriptor {
  int number = -1;

  explicit held_descriptor(int wanted) {
    auto const null = ::open("/dev/null", O_RDONLY);
    REQUIRE(null >= 0);
    number = ::dup2(null, wanted);
    ::close(null);
    REQUIRE(number == wanted);
  }
  held_descriptor(const held_descriptor&)            = delete;
  held_descriptor& operator=(const held_descriptor&) = delete;
  ~held_descriptor() {
    if (number >= 0) {
      ::close(number);
    }
  }
};

/// @brief Raises this process's soft descriptor limit to at least `wanted`.
/// The case fails when the host will not allow it.
void require_descriptor_limit(rlim_t wanted) {
  struct rlimit limit{};
  REQUIRE(::getrlimit(RLIMIT_NOFILE, &limit) == 0);
  if (limit.rlim_cur >= wanted) {
    return;
  }
  limit.rlim_cur = limit.rlim_max == RLIM_INFINITY ? wanted : std::min(wanted, limit.rlim_max);
  REQUIRE(::setrlimit(RLIMIT_NOFILE, &limit) == 0);
  REQUIRE(limit.rlim_cur >= wanted);
}

} // namespace

TEST_CASE("queue run: --detach leaves the submitter holding none of its caller's descriptors, however high their number",
          "[cmd][agent][queue][hq-detach][hq-detach-fds]") {
  // The child used to close only descriptors below min(sysconf(_SC_OPEN_MAX),
  // 8192); one above that survived into a submitter that lives as long as its
  // command, and a reader waiting on it waited as long.
  constexpr int k_low  = 37;
  constexpr int k_high = 9000;
  require_descriptor_limit(k_high + 64);
  auto const arena = parity::make_arena("qd_fds");
  write_config(arena, k_fast_poll);
  gate               release(arena.cpp_root / "release.fifo");
  release_all        guard{.gates = {&release}};
  detached_submitter submitter;
  held_descriptor    low(k_low);
  held_descriptor    high(k_high);

  auto const submitted = run_queue(arena, "fds", sh_command("read x < \"$1\"", {release.path.string()}), {"--detach"});
  INFO("stderr:\n" << submitted.err);
  REQUIRE(submitted.code == 0);
  auto const  ticket = parse_ticket(submitted.out);
  auto const  snap   = require_snapshot(arena);
  auto const* entry  = entry_seq(snap, ticket.seq);
  REQUIRE(entry != nullptr);
  submitter.record(*entry);
  REQUIRE(submitter.still_mine());

  auto const held = open_descriptors_of(entry->pid);
  INFO("descriptors held by the submitter: " << held.size());
  CHECK(std::ranges::find(held, k_low) == held.end());
  CHECK(std::ranges::find(held, k_high) == held.end());
  // Not vacuous: the standard streams and the store are still there.
  CHECK(std::ranges::find(held, STDOUT_FILENO) != held.end());
  CHECK(held.size() >= 4);
  release.release();
}

TEST_CASE("queue run: --detach refuses an existing queue-logs directory that group or others can write to",
          "[cmd][agent][queue][hq-detach][hq-detach-logdir]") {
  auto const arena = parity::make_arena("qd_loose");
  auto const dir   = arena.cpp_root / "queue-logs";
  std::filesystem::create_directories(dir);
  auto const ran = arena.cpp_root / "ran";

  struct loose {
    std::string tag;
    mode_t      mode;
  };
  for (auto const& c : {loose{"world", 0777}, loose{"group", 0770}, loose{"other", 0702}}) {
    INFO("mode " << std::format("{:o}", static_cast<unsigned>(c.mode)));
    REQUIRE(::chmod(dir.c_str(), c.mode) == 0);
    auto const got = run_queue(arena, c.tag, sh_command("echo x > \"$1\"", {ran.string()}), {"--detach"});
    INFO("stderr:\n" << got.err);
    CHECK(got.code == 125);
    CHECK(got.out.empty());
    CHECK(got.err.starts_with("error: queue: "));
    CHECK(got.err.find("queue-logs") != std::string::npos);
    CHECK(got.err.find("writable") != std::string::npos);
    auto const snap = require_snapshot(arena);
    CHECK(snap.entries.empty());
    CHECK(snap.history.empty());
    CHECK_FALSE(present(ran));
    CHECK(std::filesystem::is_empty(dir)); // Nothing written into it, and it was not tightened behind the operator's back.
    CHECK(file_mode(dir) == static_cast<unsigned>(c.mode));
  }
}

TEST_CASE("queue run: --detach accepts an existing queue-logs directory that only its owner can write to",
          "[cmd][agent][queue][hq-detach][hq-detach-logdir]") {
  auto const arena = parity::make_arena("qd_tight");
  auto const dir   = arena.cpp_root / "queue-logs";
  std::filesystem::create_directories(dir);
  for (auto const mode : {mode_t{0700}, mode_t{0755}}) {
    INFO("mode " << std::format("{:o}", static_cast<unsigned>(mode)));
    REQUIRE(::chmod(dir.c_str(), mode) == 0);
    auto const got = run_queue(arena, "tight", sh_command("exit 0"), {"--detach"});
    INFO("stderr:\n" << got.err);
    REQUIRE(got.code == 0);
    auto const ticket = parse_ticket(got.out);
    CHECK(present(ticket.path));
    REQUIRE(await([&] {
      auto const now = try_snapshot(arena);
      return now && history_seq(*now, ticket.seq) != nullptr;
    }));
  }
}

TEST_CASE("queue run: --detach refuses an existing queue-logs directory owned by another user",
          "[cmd][agent][queue][hq-detach][hq-detach-logdir]") {
  // A directory of another user is built two ways. As root, by giving one away
  // (chown). As anyone else, by pointing `queue-logs` at `/`, which root owns
  // and which is not writable by group or others, so only the OWNER rule can
  // refuse it: without that rule the run would go on to fail at opening the
  // log with a different message.
  auto const arena = parity::make_arena("qd_foreign");
  auto const dir   = arena.cpp_root / "queue-logs";
  if (::geteuid() == 0) {
    std::filesystem::create_directories(dir);
    REQUIRE(::chmod(dir.c_str(), 0700) == 0);
    REQUIRE(::chown(dir.c_str(), 12345, static_cast<gid_t>(-1)) == 0);
  } else {
    REQUIRE(::symlink("/", dir.c_str()) == 0);
  }
  auto const got = run_queue(arena, "foreign", sh_command("exit 0"), {"--detach"});
  INFO("stderr:\n" << got.err);
  CHECK(got.code == 125);
  CHECK(got.out.empty());
  CHECK(got.err.starts_with("error: queue: the log directory "));
  CHECK(got.err.find("is owned by user") != std::string::npos);
  auto const snap = require_snapshot(arena);
  CHECK(snap.entries.empty());
  CHECK(snap.history.empty());
}
