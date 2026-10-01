// queue_cancel.t.cpp: `planar-agent queue cancel <seq>` as an operator sees it
// (plan 1080, task hq-queue-cancel; tech spec 647 § CLI surface, Stopping a
// command, Cancellation is open to every caller and is attributed, Cancel
// finishes what it starts; test spec 649 scenarios citing
// task:hq-queue-cancel).
//
// Every case runs the BUILT binary in an arena with its own PLANAR_DB,
// PLANAR_DB, HOME and PLANAR_CONFIG_PATH. The store is read back through the
// engine's typed reads after every step. Synchronisation is by files and
// FIFOs, never by sleeps: a command that has to stay alive blocks reading a
// FIFO the test holds open read-write. The only clocks are the bounded waits
// that turn a hang into a failure.
//
// Nothing is signalled by name. Every pid a case signals is one it recorded,
// checked against its start time, and never 0, 1 or a group. A gate is
// released, and a spawned process stopped, on the way out of a failing case.

#include <catch2/catch_test_macros.hpp>

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

import std;
import planar.db;
import planar.db.migrate;
import planar.db.migrations;
import planar.process.identity;
import planar.engine.hostqueue;

#include "parity_harness.hpp"
#include "queue_test_store.hpp"

namespace {

namespace hq     = planar::engine::hostqueue;
namespace ident  = planar::process::identity;
namespace parity = planar::cmd::parity;

/// @brief A fresh arena whose `planar.db` already exists at the head of the
/// embedded chain: the queue verbs open the main database and never create it.
auto seeded_arena(std::string_view tag) -> parity::arena {
  auto arena = parity::make_arena(tag);
  planar::cmd::qfix::head_store(arena.cpp_root / "planar.db");
  return arena;
}

using parity::capture;
using parity::pinned_var;
using parity::read_all;

constexpr auto k_budget = std::chrono::seconds(30);

auto agent_bin() -> std::filesystem::path {
  return std::filesystem::path{PLANAR_CPP_BIN};
}

auto sh_command(std::string script, std::vector<std::string> args = {}) -> std::vector<std::string> {
  std::vector<std::string> command{"sh", "-c", std::move(script), "sh"};
  for (auto& arg : args) {
    command.push_back(std::move(arg));
  }
  return command;
}

auto queue_run_args(const std::vector<std::string>& command, std::vector<std::string> flags = {}) -> std::vector<std::string> {
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

auto cancel_args(std::string seq, std::vector<std::string> flags = {}) -> std::vector<std::string> {
  // A small number is an ordinal (see `seq_of`).
  if (std::int64_t number = 0;
      !seq.empty() && std::from_chars(seq.data(), seq.data() + seq.size(), number).ptr == seq.data() + seq.size() && number > 0) {
    seq = std::to_string(planar::cmd::qfix::seq_of(number));
  }
  std::vector<std::string> args{"queue", "cancel"};
  for (auto& flag : flags) {
    args.push_back(std::move(flag));
  }
  args.push_back(std::move(seq));
  return args;
}

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
// A poll interval no test waits for, and a one-second grace: nothing but the
// cancelling process can send the SIGKILL in time.
constexpr std::string_view k_nobody_polls = "[queue]\npoll_interval = \"20s\"\ngrace = \"1s\"\n";

struct snapshot {
  std::vector<hq::entry>       entries;
  std::vector<hq::history_row> history;
};

auto try_snapshot(const parity::arena& arena) -> std::optional<snapshot> {
  auto const path = arena.cpp_root / "planar.db";
  if (!std::filesystem::exists(path)) {
    return std::nullopt;
  }
  auto opened = planar::cmd::qfix::open_store(path);
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
  seq              = planar::cmd::qfix::seq_of(seq);
  auto const found = std::ranges::find_if(snap.entries, [&](const hq::entry& e) { return e.seq == seq; });
  return found == snap.entries.end() ? nullptr : &*found;
}

auto history_seq(const snapshot& snap, std::int64_t seq) -> const hq::history_row* {
  seq              = planar::cmd::qfix::seq_of(seq);
  auto const found = std::ranges::find_if(snap.history, [&](const hq::history_row& r) { return r.seq == seq; });
  return found == snap.history.end() ? nullptr : &*found;
}

/// @brief The one history row of `seq`; fails when there is none or more than one.
auto sole_history(const parity::arena& arena, std::int64_t seq) -> hq::history_row {
  seq             = planar::cmd::qfix::seq_of(seq);
  auto const snap = require_snapshot(arena);
  auto const rows = std::ranges::count_if(snap.history, [&](const hq::history_row& r) { return r.seq == seq; });
  REQUIRE(rows == 1);
  return *history_seq(snap, seq);
}

auto await_entry(const parity::arena& arena, std::int64_t seq, hq::entry_state state) -> hq::entry {
  seq = planar::cmd::qfix::seq_of(seq);
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

/// @brief Waits for entry `seq` to be running with its child group recorded.
auto await_child_recorded(const parity::arena& arena, std::int64_t seq) -> hq::entry {
  seq = planar::cmd::qfix::seq_of(seq);
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

struct release_all {
  std::vector<gate*> gates;
  ~release_all() {
    for (auto* g : gates) {
      g->release();
    }
  }
};

/// @brief A `planar-agent` invocation started in the background. It owns the
/// process: `stop` (run by the destructor) SIGTERMs the one recorded pid when
/// it has not ended, and SIGKILLs it after a bounded wait; the recorded start
/// time guards against a reused pid.
struct spawned {
  std::filesystem::path            root;
  std::string                      tag;
  std::int64_t                     pid = 0;
  std::optional<ident::start_time> started;

  spawned() = default;
  spawned(std::filesystem::path where, std::string name, std::int64_t process, std::optional<ident::start_time> when)
      : root(std::move(where)), tag(std::move(name)), pid(process), started(when) {
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

  [[nodiscard]] auto ended() const -> bool {
    return read_all(root / std::format("{}.code", tag)).ends_with('\n');
  }
  [[nodiscard]] auto still_mine() const -> bool {
    if (pid <= 1 || !started) {
      return false;
    }
    auto const now = ident::process_start_time(pid);
    return now && now->has_value() && **now == *started;
  }
  void stop() {
    if (pid <= 1 || ended()) {
      pid = 0;
      return;
    }
    auto const bound = std::chrono::seconds(10);
    if (still_mine()) {
      ::kill(static_cast<::pid_t>(pid), SIGTERM);
    }
    if (!await([&] { return ended(); }, bound) && still_mine()) {
      ::kill(static_cast<::pid_t>(pid), SIGKILL);
      await([&] { return ended(); }, bound);
    }
    pid = 0;
  }
};

/// @brief Starts `planar-agent <args>` in the background under the arena's
/// pinned environment plus `extra`, and returns at once. Its streams go to
/// files and its exit status to `<tag>.code`; `finish` reads them back.
auto spawn_agent(const parity::arena& arena, std::string tag, const std::vector<std::string>& args,
                 std::vector<pinned_var> extra = {}) -> spawned {
  auto vars = parity::pinned_env(arena.cpp_root);
  for (auto& var : extra) {
    vars.push_back(std::move(var));
  }

  std::string child = parity::pinned_env_prefix(vars) + parity::shell_quote(agent_bin().string());
  for (auto const& arg : args) {
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

auto spawn_queue(const parity::arena& arena, std::string tag, const std::vector<std::string>& command,
                 std::vector<std::string> flags = {}) -> spawned {
  return spawn_agent(arena, std::move(tag), queue_run_args(command, std::move(flags)));
}

/// @brief `queue cancel` run to completion under the pinned environment plus `extra`.
auto run_cancel(const parity::arena& arena, std::string_view tag, std::string seq, std::vector<std::string> flags = {},
                std::vector<pinned_var> extra = {}) -> capture {
  auto env = parity::pinned_env(arena.cpp_root);
  for (auto& var : extra) {
    env.push_back(std::move(var));
  }
  auto const args = cancel_args(std::move(seq), std::move(flags));
  return parity::run_pinned(agent_bin(), args, arena.cpp_root, tag, env);
}

/// @brief The vendor and role variables; an empty value reads as "not set".
auto identity_env(std::string vendor, std::string role) -> std::vector<pinned_var> {
  return {pinned_var{.name = "PLANAR_VENDOR", .value = std::move(vendor)},
          pinned_var{.name = "PLANAR_ROLE", .value = std::move(role)}};
}

auto present(const std::filesystem::path& path) -> bool {
  std::error_code ec;
  return std::filesystem::exists(path, ec);
}

void await_file(const std::filesystem::path& path) {
  INFO("waiting for " << path.string());
  REQUIRE(await([&] { return present(path); }));
}

auto split_lines(std::string_view text) -> std::vector<std::string> {
  std::vector<std::string> lines;
  while (!text.empty()) {
    auto const end = text.find('\n');
    lines.emplace_back(text.substr(0, end));
    text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
  }
  return lines;
}

/// @brief True when process group `pgid` has no member. Signal 0 only asks;
/// `pgid` must be a real group this test recorded.
auto group_empty(std::int64_t pgid) -> bool {
  REQUIRE(pgid > 1);
  return ::kill(-static_cast<::pid_t>(pgid), 0) != 0 && errno == ESRCH;
}

/// @brief A command that records it started and then blocks on the FIFO `$2`.
/// With `ignore_term` it ignores SIGTERM first, and records the start only
/// after the trap is in place.
auto blocked_script(bool ignore_term) -> std::string {
  return std::string{ignore_term ? "trap '' TERM; " : ""} + "echo x > \"$1\"; read x < \"$2\"";
}

/// @brief The same, with the command's streams and the harness's capture
/// descriptor closed once started, so that nothing keeps a capture open after
/// its submitter is killed.
auto detached_blocked_script(bool ignore_term) -> std::string {
  return std::string{ignore_term ? "trap '' TERM; " : ""} +
         "echo x > \"$1\"; exec >/dev/null 2>&1 </dev/null 3>&-; read x < \"$2\"";
}

/// @brief `queue status <seq> --json`, which must succeed.
auto status_json(const parity::arena& arena, std::string_view tag, std::int64_t seq) -> std::string {
  seq            = planar::cmd::qfix::seq_of(seq);
  auto const got = parity::run_pinned(agent_bin(), std::vector<std::string>{"queue", "status", std::to_string(seq), "--json"},
                                      arena.cpp_root, tag);
  INFO("status stderr:\n" << got.err);
  REQUIRE(got.code == 0);
  return got.out;
}

/// @brief What `queue status --json` must say about a cancelled entry: the
/// outcome and the canceller as the `{vendor, role, pid}` object cancel wrote.
void check_status_names_canceller(const std::string& json, std::string_view vendor, std::string_view role, std::int64_t pid) {
  INFO("status json:\n" << json);
  CHECK(json.find("\"outcome\":\"cancelled\"") != std::string::npos);
  CHECK(json.find(std::format("\"cancelled_by\":{{\"vendor\":\"{}\",\"role\":\"{}\",\"pid\":{}}}", vendor, role, pid)) !=
        std::string::npos);
}

/// @brief Whether the process recorded on `e` is still the one that submitted it.
auto submitter_alive(const hq::entry& e) -> bool {
  auto const now = ident::process_start_time(e.pid);
  return now && now->has_value() && static_cast<std::int64_t>(**now) == e.pid_started;
}

} // namespace

// ---------------------------------------------------------------------------
// Scenario: Happy path: cancelling a waiting entry
// ---------------------------------------------------------------------------
TEST_CASE("queue cancel: a waiting entry is removed, attributed to its canceller, and its submitter exits 125",
          "[cmd][agent][queue][hq-queue-cancel]") {
  auto const arena = seeded_arena("qc_waiting");
  write_config(arena, k_fast_poll);
  gate        hold(arena.cpp_root / "hold.fifo");
  auto const  started_a = arena.cpp_root / "started_a";
  auto const  started_b = arena.cpp_root / "started_b";
  spawned     a;
  spawned     b;
  spawned     canceller;
  release_all guard{.gates = {&hold}};

  a = spawn_queue(arena, "a", sh_command(blocked_script(false), {started_a.string(), hold.path.string()}));
  await_file(started_a);
  static_cast<void>(await_child_recorded(arena, 1));
  b = spawn_queue(arena, "b", sh_command("echo b > \"$1\"", {started_b.string()}), {"--notices"});
  await_entry(arena, 2, hq::entry_state::waiting);

  canceller = spawn_agent(arena, "cancel", cancel_args("2", {"--vendor", "claude", "--role", "reviewer"}));
  REQUIRE(canceller.pid > 1);
  auto const cancelled = finish(canceller);
  INFO("cancel stderr:\n" << cancelled.err);
  CHECK(cancelled.code == 0);
  CHECK(cancelled.out.find("1000002") != std::string::npos);

  // The waiting entry is gone, the holder is untouched, and the one history
  // row names who cancelled it.
  auto const snap = require_snapshot(arena);
  CHECK(entry_seq(snap, 2) == nullptr);
  REQUIRE(entry_seq(snap, 1) != nullptr);
  CHECK(entry_seq(snap, 1)->state == hq::entry_state::running);
  CHECK_FALSE(entry_seq(snap, 1)->terminating_since_mono.has_value());
  auto const row = sole_history(arena, 2);
  CHECK(row.outcome == hq::history_outcome::cancelled);
  REQUIRE(row.cancelled_by.has_value());
  CHECK(row.cancelled_by->vendor == "claude");
  CHECK(row.cancelled_by->role == "reviewer");
  CHECK(row.cancelled_by->pid == canceller.pid);
  CHECK(history_seq(snap, 1) == nullptr);
  check_status_names_canceller(status_json(arena, "status_w", 2), "claude", "reviewer", canceller.pid);

  // Its submitter exits 125 without running the command, and says why.
  auto const submitted = finish(b);
  INFO("submitter stderr:\n" << submitted.err);
  CHECK(submitted.code == 125);
  CHECK_FALSE(present(started_b));
  CHECK(split_lines(submitted.err).back() == "queue: entry 1000002 cancelled");

  hold.release();
  CHECK(finish(a).code == 0);
}

// ---------------------------------------------------------------------------
// Scenario: Happy path: cancelling a running entry
// ---------------------------------------------------------------------------
TEST_CASE(
    "queue cancel: a running command that honours SIGTERM ends cancelled, its submitter exits 125 and the next entry starts",
    "[cmd][agent][queue][hq-queue-cancel]") {
  auto const arena = seeded_arena("qc_running");
  write_config(arena, k_fast_poll);
  gate        hold(arena.cpp_root / "hold.fifo");
  auto const  started_a = arena.cpp_root / "started_a";
  auto const  started_b = arena.cpp_root / "started_b";
  spawned     a;
  spawned     b;
  spawned     canceller;
  release_all guard{.gates = {&hold}};

  a = spawn_queue(arena, "a", sh_command(blocked_script(false), {started_a.string(), hold.path.string()}), {"--notices"});
  await_file(started_a);
  auto const running = await_child_recorded(arena, 1);
  REQUIRE(running.child_pgid.has_value());
  b = spawn_queue(arena, "b", sh_command("echo b > \"$1\"", {started_b.string()}));
  await_entry(arena, 2, hq::entry_state::waiting);

  canceller            = spawn_agent(arena, "cancel", cancel_args("1", {"--vendor", "codex", "--role", "operator"}));
  auto const cancelled = finish(canceller);
  INFO("cancel stderr:\n" << cancelled.err);
  CHECK(cancelled.code == 0);

  // When cancel returns the group is empty and the entry is gone, with
  // exactly one history row: outcome cancelled, canceller recorded.
  CHECK(group_empty(*running.child_pgid));
  CHECK(entry_seq(require_snapshot(arena), 1) == nullptr);
  auto const row = sole_history(arena, 1);
  CHECK(row.outcome == hq::history_outcome::cancelled);
  REQUIRE(row.cancelled_by.has_value());
  CHECK(row.cancelled_by->vendor == "codex");
  CHECK(row.cancelled_by->role == "operator");
  CHECK(row.cancelled_by->pid == canceller.pid);
  check_status_names_canceller(status_json(arena, "status_r", 1), "codex", "operator", canceller.pid);

  auto const submitted = finish(a);
  INFO("submitter stderr:\n" << submitted.err);
  CHECK(submitted.code == 125);
  CHECK(split_lines(submitted.err).back() == "queue: entry 1000001 cancelled");

  // The freed slot goes to the next entry.
  auto const next = finish(b);
  INFO("next stderr:\n" << next.err);
  CHECK(next.code == 0);
  CHECK(present(started_b));
  CHECK(sole_history(arena, 2).outcome == hq::history_outcome::exited);
  CHECK(sole_history(arena, 1).outcome == hq::history_outcome::cancelled);
}

// ---------------------------------------------------------------------------
// Scenario: Edge: cancel stops a command that ignores SIGTERM with nothing polling
// ---------------------------------------------------------------------------
TEST_CASE("queue cancel: sends SIGKILL itself after the grace period to a command that ignores SIGTERM",
          "[cmd][agent][queue][hq-queue-cancel]") {
  auto const arena = seeded_arena("qc_stubborn");
  write_config(arena, k_nobody_polls);
  gate        hold(arena.cpp_root / "hold.fifo");
  auto const  started = arena.cpp_root / "started";
  spawned     a;
  release_all guard{.gates = {&hold}};

  a = spawn_queue(arena, "a", sh_command(blocked_script(true), {started.string(), hold.path.string()}));
  await_file(started);
  auto const running = await_child_recorded(arena, 1);
  REQUIRE(running.child_pgid.has_value());

  auto const begun     = std::chrono::steady_clock::now();
  auto const cancelled = run_cancel(arena, "cancel", "1");
  auto const took      = std::chrono::steady_clock::now() - begun;
  INFO("cancel stderr:\n" << cancelled.err);
  CHECK(cancelled.code == 0);

  // SIGTERM alone cannot have emptied the group: the grace period (1s) had
  // to pass before cancel's own SIGKILL, and nothing else was polling.
  CHECK(took >= std::chrono::milliseconds(900));
  CHECK(took < std::chrono::seconds(15));
  CHECK(group_empty(*running.child_pgid));
  CHECK(entry_seq(require_snapshot(arena), 1) == nullptr);
  CHECK(sole_history(arena, 1).outcome == hq::history_outcome::cancelled);

  auto const submitted = finish(a);
  INFO("submitter stderr:\n" << submitted.err);
  CHECK(submitted.code == 125);
}

// ---------------------------------------------------------------------------
// Scenario: Edge: cancelling an orphan with nothing polling
// ---------------------------------------------------------------------------
TEST_CASE("queue cancel: stops the command of an entry whose submitter is gone and removes the entry",
          "[cmd][agent][queue][hq-queue-cancel]") {
  auto const arena = seeded_arena("qc_orphan");
  write_config(arena, k_nobody_polls);
  gate        hold(arena.cpp_root / "hold.fifo");
  auto const  started = arena.cpp_root / "started";
  spawned     a;
  release_all guard{.gates = {&hold}};

  a = spawn_queue(arena, "a", sh_command(detached_blocked_script(true), {started.string(), hold.path.string()}));
  await_file(started);
  auto const running = await_child_recorded(arena, 1);
  REQUIRE(running.child_pgid.has_value());
  REQUIRE(running.pid > 1);
  REQUIRE(running.pid != static_cast<std::int64_t>(::getpid()));
  // The recorded pid is the submitter this case started.
  REQUIRE(::kill(static_cast<::pid_t>(running.pid), SIGKILL) == 0);
  CHECK(finish(a).code == 128 + SIGKILL);

  // The submitter is gone; its command still runs and nothing polls.
  CHECK_FALSE(group_empty(*running.child_pgid));
  REQUIRE(entry_seq(require_snapshot(arena), 1) != nullptr);

  auto const cancelled = run_cancel(arena, "cancel", "1", {"--vendor", "claude", "--role", "janitor"});
  INFO("cancel stderr:\n" << cancelled.err);
  CHECK(cancelled.code == 0);
  CHECK(group_empty(*running.child_pgid));
  CHECK(entry_seq(require_snapshot(arena), 1) == nullptr);
  auto const row = sole_history(arena, 1);
  CHECK(row.outcome == hq::history_outcome::cancelled);
  REQUIRE(row.cancelled_by.has_value());
  CHECK(row.cancelled_by->vendor == "claude");
  CHECK(row.cancelled_by->role == "janitor");
}

// ---------------------------------------------------------------------------
// Scenario: Edge: a detached entry has no terminal and is cancelled the same way
// ---------------------------------------------------------------------------
TEST_CASE("queue cancel: stops a detached run and its detached submitter ends the entry as cancelled",
          "[cmd][agent][queue][hq-queue-cancel]") {
  auto const arena = seeded_arena("qc_detached");
  write_config(arena, k_fast_poll);
  gate        hold(arena.cpp_root / "hold.fifo");
  auto const  started = arena.cpp_root / "started";
  release_all guard{.gates = {&hold}};

  auto const args      = queue_run_args(sh_command(blocked_script(false), {started.string(), hold.path.string()}), {"--detach"});
  auto const submitted = parity::run_pinned(agent_bin(), args, arena.cpp_root, "detach");
  INFO("detach stderr:\n" << submitted.err);
  REQUIRE(submitted.code == 0);
  auto const lines = split_lines(submitted.out);
  REQUIRE(lines.size() >= 1);
  std::int64_t seq = 0;
  REQUIRE(std::from_chars(lines[0].data(), lines[0].data() + lines[0].size(), seq).ec == std::errc{});
  await_file(started);
  auto const running = await_child_recorded(arena, seq);
  REQUIRE(running.child_pgid.has_value());
  REQUIRE(running.pid > 1);

  auto const cancelled = run_cancel(arena, "cancel", std::to_string(seq));
  INFO("cancel stderr:\n" << cancelled.err);
  CHECK(cancelled.code == 0);
  CHECK(group_empty(*running.child_pgid));
  CHECK(entry_seq(require_snapshot(arena), seq) == nullptr);
  CHECK(sole_history(arena, seq).outcome == hq::history_outcome::cancelled);
  // The detached submitter leaves on its own; nothing here signals it.
  REQUIRE(await([&] { return !submitter_alive(running); }));
}

// ---------------------------------------------------------------------------
// Scenario: Error: cancelling an unknown or ended entry
// ---------------------------------------------------------------------------
TEST_CASE("queue cancel: an unknown entry exits 1, an ended one exits 6, and neither changes anything",
          "[cmd][agent][queue][hq-queue-cancel]") {
  auto const arena = seeded_arena("qc_unknown");
  write_config(arena, k_fast_poll);

  // Never issued: nothing exists yet, not even entry 1.
  auto const unknown = run_cancel(arena, "unknown", "99");
  INFO("stderr:\n" << unknown.err);
  CHECK(unknown.code == 1);
  CHECK(unknown.err.find("99") != std::string::npos);

  // An entry that has ended: a command ran and left one history row.
  auto const ran = parity::run_pinned(agent_bin(), queue_run_args(sh_command("exit 0")), arena.cpp_root, "ran");
  REQUIRE(ran.code == 0);
  auto const before = require_snapshot(arena);
  REQUIRE(before.history.size() == 1);
  REQUIRE(before.entries.empty());

  auto const ended = run_cancel(arena, "ended", "1", {"--vendor", "claude", "--role", "reviewer"});
  INFO("stderr:\n" << ended.err);
  CHECK(ended.code == 6);
  CHECK(ended.err.find("exited") != std::string::npos);

  auto const after = require_snapshot(arena);
  CHECK(after.entries.empty());
  REQUIRE(after.history.size() == 1);
  CHECK(after.history[0].outcome == hq::history_outcome::exited);
  CHECK_FALSE(after.history[0].cancelled_by.has_value());

  // A number beyond the last one issued is unknown too.
  CHECK(run_cancel(arena, "unknown_again", "5").code == 1);
}

TEST_CASE("queue cancel: a sequence number that is not a positive integer is refused at 2, and none at all at 1",
          "[cmd][agent][queue][hq-queue-cancel]") {
  auto const arena = seeded_arena("qc_badseq");
  write_config(arena, k_fast_poll);
  for (auto const* bad : {"abc", "0", "1x", ""}) {
    INFO("seq '" << bad << "'");
    auto const got = run_cancel(arena, "bad", bad);
    INFO("stderr:\n" << got.err);
    CHECK(got.code == 2);
  }
  auto const missing = parity::run_pinned(agent_bin(), std::vector<std::string>{"queue", "cancel"}, arena.cpp_root, "noseq");
  CHECK(missing.code == 1);
}

// ---------------------------------------------------------------------------
// Scenario: Error: an unreachable store
// ---------------------------------------------------------------------------
TEST_CASE("queue cancel: an unreachable store exits 125", "[cmd][agent][queue][hq-queue-cancel]") {
  auto const arena = seeded_arena("qc_nostore");
  write_config(arena, k_fast_poll);
  // The database's path is a directory, so it can be neither opened nor created.
  for (auto const* suffix : {"", "-wal", "-shm"}) {
    std::filesystem::remove(arena.cpp_root / (std::string{"planar.db"} + suffix));
  }
  std::filesystem::create_directories(arena.cpp_root / "planar.db");
  auto const got = run_cancel(arena, "nostore", "1");
  INFO("stderr:\n" << got.err);
  CHECK(got.code == 125);
  CHECK(got.err.find("error: queue:") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Scenario: Edge: cancel records who cancelled
// ---------------------------------------------------------------------------
TEST_CASE("queue cancel: the vendor and role come from the flags, then the environment, and are stored empty otherwise",
          "[cmd][agent][queue][hq-queue-cancel][hq-vendor-role]") {
  auto const arena = seeded_arena("qc_identity");
  write_config(arena, k_fast_poll);
  gate        hold(arena.cpp_root / "hold.fifo");
  auto const  started = arena.cpp_root / "started";
  spawned     holder;
  release_all guard{.gates = {&hold}};

  holder = spawn_queue(arena, "holder", sh_command(blocked_script(false), {started.string(), hold.path.string()}));
  await_file(started);
  static_cast<void>(await_child_recorded(arena, 1));

  // Entries 2..7 wait behind the holder; each is cancelled one way.
  std::vector<spawned> waiters;
  for (int i = 0; i < 6; ++i) {
    waiters.push_back(spawn_queue(arena, std::format("w{}", i), sh_command("exit 0")));
    await_entry(arena, 2 + i, hq::entry_state::waiting);
  }
  REQUIRE(run_cancel(arena, "c2", "2", {"--vendor", "flagv", "--role", "flagr"}, identity_env("", "")).code == 0);
  REQUIRE(run_cancel(arena, "c3", "3", {}, identity_env("envv", "envr")).code == 0);
  REQUIRE(run_cancel(arena, "c4", "4", {"--vendor", "flagv", "--role", "flagr"}, identity_env("envv", "envr")).code == 0);
  REQUIRE(run_cancel(arena, "c5", "5", {}, identity_env("", "")).code == 0);
  REQUIRE(run_cancel(arena, "c6", "6", {"--vendor", "", "--role", ""}, identity_env("", "")).code == 0);
  REQUIRE(run_cancel(arena, "c7", "7", {"--vendor", "", "--role", ""}, identity_env("envv", "envr")).code == 0);

  auto const check_row = [&](std::int64_t seq, std::optional<std::string> vendor, std::optional<std::string> role) {
    INFO("history row " << seq);
    auto const row = sole_history(arena, seq);
    CHECK(row.outcome == hq::history_outcome::cancelled);
    REQUIRE(row.cancelled_by.has_value());
    CHECK(row.cancelled_by->vendor == vendor);
    CHECK(row.cancelled_by->role == role);
    CHECK(row.cancelled_by->pid > 1);
  };
  check_row(2, "flagv", "flagr");
  check_row(3, "envv", "envr");
  check_row(4, "flagv", "flagr");
  check_row(5, std::nullopt, std::nullopt);
  check_row(6, std::nullopt, std::nullopt);
  check_row(7, "envv", "envr");

  for (auto& waiter : waiters) {
    CHECK(finish(waiter).code == 125);
  }
  hold.release();
  CHECK(finish(holder).code == 0);
}

// ---------------------------------------------------------------------------
// Scenario: Edge: cancel racing the command's own exit
// ---------------------------------------------------------------------------
TEST_CASE("queue cancel: racing the command's own exit leaves exactly one history row, cancelled or exited",
          "[cmd][agent][queue][hq-queue-cancel]") {
  auto const arena = seeded_arena("qc_race");
  write_config(arena, k_fast_poll);
  constexpr int k_rounds         = 24;
  int           cancelled_rounds = 0;
  int           exited_rounds    = 0;

  for (int round = 0; round < k_rounds; ++round) {
    INFO("round " << round);
    auto const  seq = planar::cmd::qfix::seq_of(round + 1);
    gate        hold(arena.cpp_root / std::format("hold{}.fifo", round));
    auto const  started = arena.cpp_root / std::format("started{}", round);
    release_all guard{.gates = {&hold}};

    auto submitter = spawn_queue(arena, std::format("sub{}", round),
                                 sh_command(blocked_script(false), {started.string(), hold.path.string()}));
    await_file(started);
    static_cast<void>(await_child_recorded(arena, seq));

    // Two ways to land the exit inside the cancel. Odd rounds are synchronised
    // on state, not on time: the command is told to exit only once the cancel
    // has committed its marker (or the entry is already gone), so the exit
    // races the SIGTERM and the cancel's own end however loaded the host is.
    // Even rounds keep the timing race: the exit lands a varying number of
    // milliseconds after the cancel starts, before it marks the entry, while
    // it marks it, and after its SIGTERM. Under load a fixed delay lands the
    // exit first every time, which is why only the odd rounds pin the cancel
    // branch.
    auto const canceller = spawn_agent(arena, std::format("can{}", round), cancel_args(std::to_string(seq)));
    if (round % 2 == 1) {
      REQUIRE(await([&] {
        auto const snap = try_snapshot(arena);
        if (!snap) {
          return false;
        }
        auto const* found = entry_seq(*snap, seq);
        return found == nullptr || found->terminating_since_mono.has_value();
      }));
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds((round % 12) * 3));
    }
    hold.release();

    auto const cancelled = finish(canceller);
    auto const submitted = finish(submitter);
    INFO("cancel stderr:\n" << cancelled.err << "submitter stderr:\n" << submitted.err);

    // Exactly one history row, no entry left, and every party agrees on it.
    auto const snap = require_snapshot(arena);
    CHECK(entry_seq(snap, seq) == nullptr);
    auto const rows = std::ranges::count_if(snap.history, [&](const hq::history_row& r) { return r.seq == seq; });
    REQUIRE(rows == 1);
    auto const outcome = history_seq(snap, seq)->outcome;
    if (round % 2 == 1) {
      // The marker was committed before the command exited, so the cancel won.
      CHECK(outcome == hq::history_outcome::cancelled);
    }
    if (outcome == hq::history_outcome::cancelled) {
      ++cancelled_rounds;
      CHECK(cancelled.code == 0);
      // The submitter reports the cancel: it reads the marker, or, when cancel
      // ended the entry after the command exited, it re-reads the history row
      // its own end found already written (decision 1188).
      CHECK(submitted.code == 125);
    } else {
      REQUIRE(outcome == hq::history_outcome::exited);
      ++exited_rounds;
      CHECK(cancelled.code == 6);
      CHECK(submitted.code == 0);
    }
  }
  INFO("cancelled " << cancelled_rounds << ", exited " << exited_rounds);
  // Both branches are reported, and the cancel branch must have run; the exit
  // branch is pinned deterministically by the ended-entry case and the seams.
  WARN("race split over " << k_rounds << " rounds: cancelled " << cancelled_rounds << ", exited " << exited_rounds);
  CHECK(cancelled_rounds > 0);
  CHECK(cancelled_rounds + exited_rounds == k_rounds);
  CHECK(require_snapshot(arena).history.size() == static_cast<std::size_t>(k_rounds));
}
