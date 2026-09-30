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
#include <sys/stat.h>
#include <unistd.h>

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
struct spawned {
  std::filesystem::path root; ///< The arena root the invocation ran under.
  std::string           tag;  ///< Its capture-file name: `<tag>.out`, `.err`, `.code`.
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
auto spawn_queue(const parity::arena& arena, std::string tag, const std::vector<std::string>& command,
                 std::vector<pinned_var> extra = {}, std::vector<std::string> flags = {}) -> spawned {
  auto vars = parity::pinned_env(arena.cpp_root);
  for (auto& var : extra) {
    vars.push_back(std::move(var));
  }
  parity::require_agent_db_pinned(arena.cpp_root, vars);

  std::string child = parity::pinned_env_prefix(vars) + parity::shell_quote(agent_bin().string());
  for (auto const& arg : queue_args(command, std::move(flags))) {
    child += " " + parity::shell_quote(arg);
  }
  auto const path = [&](std::string_view suffix) {
    return parity::shell_quote((arena.cpp_root / std::format("{}.{}", tag, suffix)).string());
  };
  std::error_code ec;
  std::filesystem::remove(arena.cpp_root / std::format("{}.code", tag), ec);
  auto const line =
      std::format("( cd {} && {{ {} ; echo $? > {} ; }} > {} 2> {} ) </dev/null >/dev/null 2>&1 &",
                  parity::shell_quote((arena.cpp_root / "proj").string()), child, path("code"), path("out"), path("err"));
  static_cast<void>(std::system(line.c_str()));
  return spawned{.root = arena.cpp_root, .tag = std::move(tag)};
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
  auto const running = await_entry(arena, 1, hq::entry_state::running);
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
  auto const running = await_entry(arena, 1, hq::entry_state::running);
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
  auto const running = await_entry(arena, 1, hq::entry_state::running);
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
  auto const running = await_entry(arena, 1, hq::entry_state::running);
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
  auto const running = await_entry(arena, 1, hq::entry_state::running);
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
    auto const running = await_entry(arena, 1, hq::entry_state::running);
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
