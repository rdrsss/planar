// queue_status.t.cpp: `planar-agent queue status <seq>` as an operator sees it
// (plan 1080, task hq-queue-status; tech spec 647 § CLI surface, `queue status
// --json`; test spec 649 scenarios citing task:hq-queue-status).
//
// Every case runs the BUILT binary through `run_pinned`, in an arena with its
// own `PLANAR_AGENT_DB`, `PLANAR_DB`, `HOME` and `PLANAR_CONFIG_PATH`, so
// nothing can reach the operator's `~/.planar`. `--json` output is parsed with
// the DOM parser and asserted field by field; a substring check would pass for
// a field renamed or moved. The workflow case drives REAL detached submitters
// (the ticket `queue run --detach` prints is what status takes); the state
// cases seed the store through the engine (`enqueue`, `poll`, `end_entry`,
// `rejoin`, `begin_terminate`), the way the engine's own suites do, because
// `queue cancel` is a later task.
//
// Synchronisation is by FIFOs and files, not sleeps: a command that must stay
// alive blocks reading a FIFO the test holds open read-write. A gate is
// released, and a detached submitter stopped, on the way out of a failing case.

#include <catch2/catch_test_macros.hpp>

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

import std;
import planar.db;
import planar.db.agentdb;
import planar.db.migrate;
import planar.db.migrations_agent;
import planar.json_dom;
import planar.process.identity;
import planar.engine.hostqueue;

#include "parity_harness.hpp"

namespace {

namespace hq     = planar::engine::hostqueue;
namespace ident  = planar::process::identity;
namespace json   = planar::json_dom;
namespace parity = planar::cmd::parity;

using parity::capture;
using parity::read_all;

constexpr auto k_budget = std::chrono::seconds(30);

/// @brief The field set the tech spec names for `queue status --json`, in its order.
const std::vector<std::string> k_fields{
    "seq",         "state",        "live",          "position",      "outcome",     "exit_code",  "signal",
    "terminating", "cancelled_by", "superseded_by", "nested",        "parent_seq",  "cwd",        "argv",
    "label",       "vendor",       "role",          "log_path",      "enqueued_at", "started_at", "ended_at",
    "waited_ms",   "ran_ms",       "run_limit_ms",  "wait_limit_ms", "slots",       "grace_ms"};

auto agent_bin() -> std::filesystem::path {
  return std::filesystem::path{PLANAR_CPP_BIN};
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

auto store_path(const parity::arena& arena) -> std::filesystem::path {
  return arena.cpp_root / "agent.db";
}

auto open_store(const parity::arena& arena) -> planar::db::connection {
  auto opened = planar::db::agent::open_agent_db_at(store_path(arena));
  REQUIRE(opened.has_value());
  return std::move(*opened);
}

/// @brief `queue status <seq> [--json]` under the arena's pinned environment.
auto run_status(const parity::arena& arena, std::string_view tag, std::string_view seq, bool as_json = true) -> capture {
  std::vector<std::string> args{"queue", "status", std::string{seq}};
  if (as_json) {
    args.emplace_back("--json");
  }
  return parity::run_pinned(agent_bin(), args, arena.cpp_root, tag);
}

auto run_status(const parity::arena& arena, std::string_view tag, std::int64_t seq, bool as_json = true) -> capture {
  return run_status(arena, tag, std::to_string(seq), as_json);
}

/// @brief Parses a JSON document, failing the case when it is not one.
auto parse_document(const std::string& text) -> json::json_value {
  INFO("stdout:\n" << text);
  auto parsed = json::parse_json(text);
  REQUIRE(parsed.has_value());
  return std::move(*parsed);
}

/// @brief The status object a successful `--json` run prints.
auto status_object(const capture& run) -> json::json_value {
  INFO("stderr:\n" << run.err);
  REQUIRE(run.code == 0);
  auto doc = parse_document(run.out);
  REQUIRE(doc.kind == json::json_kind::object);
  return doc;
}

auto member(const json::json_value& object, std::string_view key) -> const json::json_value& {
  INFO("field: " << key);
  auto const* found = object.find(key);
  REQUIRE(found != nullptr);
  return *found;
}

auto is_null(const json::json_value& object, std::string_view key) -> bool {
  return member(object, key).kind == json::json_kind::null_;
}

auto text_of(const json::json_value& object, std::string_view key) -> std::string {
  auto const& value = member(object, key);
  REQUIRE(value.kind == json::json_kind::string);
  return value.string;
}

auto int_of(const json::json_value& object, std::string_view key) -> std::int64_t {
  auto const& value = member(object, key);
  REQUIRE(value.kind == json::json_kind::integer);
  return value.integer;
}

auto keys_of(const json::json_value& object) -> std::vector<std::string> {
  std::vector<std::string> keys;
  for (auto const& [key, unused] : object.object) {
    keys.push_back(key);
  }
  return keys;
}

/// @brief The `key: value` lines of the text form, in order.
auto text_lines(const std::string& out) -> std::vector<std::pair<std::string, std::string>> {
  std::vector<std::pair<std::string, std::string>> lines;
  std::string_view                                 rest = out;
  while (!rest.empty()) {
    auto const end  = rest.find('\n');
    auto const line = rest.substr(0, end);
    rest            = end == std::string_view::npos ? std::string_view{} : rest.substr(end + 1);
    auto const sep  = line.find(": ");
    REQUIRE(sep != std::string_view::npos);
    lines.emplace_back(std::string{line.substr(0, sep)}, std::string{line.substr(sep + 2)});
  }
  return lines;
}

auto text_value(const std::vector<std::pair<std::string, std::string>>& lines, std::string_view key)
    -> std::optional<std::string> {
  auto const found = std::ranges::find_if(lines, [&](const auto& line) { return line.first == key; });
  if (found == lines.end()) {
    return std::nullopt;
  }
  return found->second;
}

/// @brief A FIFO the test holds open read-write; a command reading it blocks
/// until `release()`.
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

/// @brief Stops the detached submitters a case recorded when it leaves scope:
/// SIGTERM to a recorded pid whose start time still matches, SIGKILL only if
/// it outlives a bounded wait. Never signals by name, and never pid 0, 1 or a
/// group.
struct submitter_guard {
  struct target {
    std::int64_t pid     = 0;
    std::int64_t started = 0;
  };
  std::vector<target> targets;

  submitter_guard()                                  = default;
  submitter_guard(const submitter_guard&)            = delete;
  submitter_guard& operator=(const submitter_guard&) = delete;

  void record(std::int64_t pid, std::int64_t started) {
    targets.push_back({pid, started});
  }
  static auto still_mine(const target& t) -> bool {
    if (t.pid <= 1) {
      return false;
    }
    auto const now = ident::process_start_time(t.pid);
    return now && now->has_value() && static_cast<std::int64_t>(**now) == t.started;
  }
  ~submitter_guard() {
    for (auto const& t : targets) {
      if (!still_mine(t)) {
        continue;
      }
      ::kill(static_cast<::pid_t>(t.pid), SIGTERM);
      if (!await([&] { return !still_mine(t); }, std::chrono::seconds(10)) && still_mine(t)) {
        ::kill(static_cast<::pid_t>(t.pid), SIGKILL);
        await([&] { return !still_mine(t); }, std::chrono::seconds(10));
      }
    }
  }
};

auto sh_command(std::string script, std::vector<std::string> args = {}) -> std::vector<std::string> {
  std::vector<std::string> command{"sh", "-c", std::move(script), "sh"};
  for (auto& arg : args) {
    command.push_back(std::move(arg));
  }
  return command;
}

struct ticket {
  std::int64_t seq = 0;
  std::string  path;
};

/// @brief `queue run --detach <flags> -- <command>`: the ticket it prints.
auto submit_detached(const parity::arena& arena, std::string_view tag, const std::vector<std::string>& command,
                     std::vector<std::string> flags, submitter_guard& guard) -> ticket {
  std::vector<std::string> args{"queue", "run", "--detach"};
  for (auto& flag : flags) {
    args.push_back(std::move(flag));
  }
  args.emplace_back("--");
  for (auto const& word : command) {
    args.push_back(word);
  }
  auto const run = parity::run_pinned(agent_bin(), args, arena.cpp_root, tag);
  INFO("stderr:\n" << run.err);
  REQUIRE(run.code == 0);
  auto const first = run.out.find('\n');
  REQUIRE(first != std::string::npos);
  auto const second = run.out.find('\n', first + 1);
  REQUIRE(second != std::string::npos);
  ticket result;
  REQUIRE(std::from_chars(run.out.data(), run.out.data() + first, result.seq).ec == std::errc{});
  result.path = run.out.substr(first + 1, second - first - 1);

  auto conn  = open_store(arena);
  auto found = hq::find(conn, result.seq);
  REQUIRE(found.has_value());
  REQUIRE(found->has_value());
  guard.record((*found)->pid, (*found)->pid_started);
  return result;
}

/// @brief Waits for entry `seq` to be in `state`.
void await_state(const parity::arena& arena, std::int64_t seq, hq::entry_state state) {
  REQUIRE(await([&] {
    auto opened = planar::db::agent::open_agent_db_at(store_path(arena));
    if (!opened) {
      return false;
    }
    auto found = hq::find(*opened, seq);
    return found && found->has_value() && (*found)->state == state;
  }));
}

/// @brief Waits for the history row of `seq`.
void await_history(const parity::arena& arena, std::int64_t seq) {
  REQUIRE(await([&] {
    auto opened = planar::db::agent::open_agent_db_at(store_path(arena));
    if (!opened) {
      return false;
    }
    auto found = hq::find_history(*opened, seq);
    return found && found->has_value();
  }));
}

// ---- engine seeding -------------------------------------------------------

auto this_pid() -> std::int64_t {
  return static_cast<std::int64_t>(::getpid());
}

auto this_start() -> std::int64_t {
  auto const started = ident::process_start_time(this_pid());
  REQUIRE((started.has_value() && started->has_value()));
  return static_cast<std::int64_t>(**started);
}

auto this_host() -> std::string {
  return ident::host_identity(ident::native_identity_source());
}

auto mono_now() -> std::int64_t {
  ident::system_clock clock;
  auto const          now = clock.monotonic_ms();
  REQUIRE(now.has_value());
  return *now;
}

auto wall_now() -> std::int64_t {
  ident::system_clock clock;
  return clock.wall_ms();
}

/// @brief A request whose submitter is this test process: alive, its start
/// time matching, and fresh.
auto alive_request(std::string label, std::string log = "") -> hq::enqueue_request {
  return hq::enqueue_request{.host_id        = this_host(),
                             .pid            = this_pid(),
                             .pid_started    = this_start(),
                             .cwd            = "/work/project",
                             .argv           = {"make", "test"},
                             .label          = std::move(label),
                             .vendor         = "claude",
                             .role           = "coder",
                             .log_path       = log.empty() ? std::nullopt : std::optional<std::string>{log},
                             .enqueued_at    = wall_now(),
                             .refreshed_mono = mono_now()};
}

/// @brief A process id that no longer exists: a child that exited and was reaped.
auto dead_pid() -> std::int64_t {
  auto const child = ::fork();
  REQUIRE(child >= 0);
  if (child == 0) {
    ::_exit(0);
  }
  int status = 0;
  REQUIRE(::waitpid(child, &status, 0) == child);
  return static_cast<std::int64_t>(child);
}

auto enqueue_or_fail(planar::db::connection& conn, const hq::enqueue_request& request) -> std::int64_t {
  auto const seq = hq::enqueue(conn, request);
  REQUIRE(seq.has_value());
  return *seq;
}

/// @brief Starts entry `seq` the way a poll does, as its submitter (this process).
void start_entry(planar::db::connection& conn, std::int64_t seq, std::int64_t slots = 1, std::int64_t run_limit_ms = 300'000) {
  ident::system_clock clock;
  auto const          polled =
      hq::poll(conn,
               hq::poll_request{
                   .seq = seq, .host_id = this_host(), .slots = slots, .stale_after_ms = 60'000, .run_limit_ms = run_limit_ms},
               clock, hq::system_process_probe());
  REQUIRE(polled.has_value());
  REQUIRE(polled->running);
}

// ---------------------------------------------------------------------------
// The operator workflow: a detached ticket, polled from waiting to ended.
// ---------------------------------------------------------------------------

TEST_CASE("queue status: a detached run is polled from waiting through running to ended",
          "[cmd][agent][queue][hq-queue-status]") {
  auto const arena = parity::make_arena("qs_workflow");
  write_config(arena, k_fast_poll);
  gate            hold(arena.cpp_root / "hold.fifo");
  submitter_guard guard;

  auto const holder =
      submit_detached(arena, "holder", sh_command("read x < \"$1\"", {hold.path.string()}), {"--label", "holder"}, guard);
  await_state(arena, holder.seq, hq::entry_state::running);
  auto const second = submit_detached(arena, "second", sh_command("echo second-out; exit 5"),
                                      {"--label", "second", "--vendor", "codex", "--role", "tester"}, guard);
  auto const third  = submit_detached(arena, "third", sh_command("echo third-out"), {"--label", "third"}, guard);

  // The running entry: state running, alive, not in the waiting order.
  auto const running = status_object(run_status(arena, "st_running", holder.seq));
  CHECK(int_of(running, "seq") == holder.seq);
  CHECK(text_of(running, "state") == "running");
  CHECK(member(running, "live").kind == json::json_kind::boolean);
  CHECK(member(running, "live").boolean);
  CHECK(is_null(running, "position"));
  CHECK(is_null(running, "outcome"));
  CHECK(text_of(running, "log_path") == holder.path);
  CHECK(int_of(running, "started_at") > 0);

  // Waiting entries report their place among the waiting entries only: the
  // running holder does not count.
  auto const w2 = status_object(run_status(arena, "st_second", second.seq));
  CHECK(text_of(w2, "state") == "waiting");
  CHECK(int_of(w2, "position") == 1);
  CHECK(member(w2, "live").boolean);
  CHECK(text_of(w2, "label") == "second");
  CHECK(text_of(w2, "vendor") == "codex");
  CHECK(text_of(w2, "role") == "tester");
  CHECK(is_null(w2, "started_at"));
  auto const w3 = status_object(run_status(arena, "st_third", third.seq));
  CHECK(text_of(w3, "state") == "waiting");
  CHECK(int_of(w3, "position") == 2);

  // The text form carries the same answer as lines.
  auto const text = run_status(arena, "st_third_text", third.seq, false);
  REQUIRE(text.code == 0);
  auto const lines = text_lines(text.out);
  CHECK(text_value(lines, "seq") == std::to_string(third.seq));
  CHECK(text_value(lines, "state") == "waiting");
  CHECK(text_value(lines, "position") == "2");
  CHECK(text_value(lines, "live") == "true");
  CHECK(text_value(lines, "log_path") == third.path);

  // Release the holder: the rest run in order, and the entries end.
  hold.release();
  await_history(arena, holder.seq);
  await_history(arena, second.seq);
  await_history(arena, third.seq);

  auto const done = status_object(run_status(arena, "st_second_done", second.seq));
  CHECK(text_of(done, "state") == "ended");
  CHECK(text_of(done, "outcome") == "exited");
  CHECK(int_of(done, "exit_code") == 5);
  CHECK(is_null(done, "signal"));
  CHECK(is_null(done, "live"));
  CHECK(is_null(done, "position"));
  CHECK(is_null(done, "terminating"));
  CHECK(text_of(done, "log_path") == second.path);
  CHECK(int_of(done, "waited_ms") >= 0);
  CHECK(int_of(done, "ran_ms") >= 0);
  CHECK(int_of(done, "ended_at") >= int_of(done, "started_at"));
  CHECK(read_all(second.path) == "second-out\n");

  auto const done_text = run_status(arena, "st_second_done_text", second.seq, false);
  REQUIRE(done_text.code == 0);
  auto const done_lines = text_lines(done_text.out);
  CHECK(text_value(done_lines, "state") == "ended");
  CHECK(text_value(done_lines, "outcome") == "exited");
  CHECK(text_value(done_lines, "exit_code") == "5");
  CHECK(text_value(done_lines, "log_path") == second.path);
  CHECK_FALSE(text_value(done_lines, "position").has_value());
}

// ---------------------------------------------------------------------------
// The documented field set.
// ---------------------------------------------------------------------------

TEST_CASE("queue status: a waiting, a running and an ended entry each return exactly the documented field set",
          "[cmd][agent][queue][hq-queue-status]") {
  auto const arena = parity::make_arena("qs_fields");
  write_config(arena, "[queue]\nslots = 2\ngrace = \"3s\"\n");
  auto conn = open_store(arena);

  auto const ended_seq = enqueue_or_fail(conn, alive_request("ended", "/logs/ended.log"));
  REQUIRE(hq::end_entry(conn, ended_seq,
                        hq::end_request{.outcome = hq::history_outcome::exited, .exit_code = 5, .ended_at = wall_now()})
              .has_value());
  // Test-spec "status carries the documented fields": `--timeout 5m` and
  // `--wait-timeout 10m` in force for the running entry. The wait limit is
  // what enqueue records; the run limit is the poll's, recorded at the start.
  auto running_request          = alive_request("running", "/logs/running.log");
  running_request.wait_limit_ms = 600'000;
  auto const running_seq        = enqueue_or_fail(conn, running_request);
  start_entry(conn, running_seq, 2, 300'000);
  // Both of the two slots are taken before the waiting entry arrives.
  start_entry(conn, enqueue_or_fail(conn, alive_request("second-runner")), 2);
  auto const waiting_seq = enqueue_or_fail(conn, alive_request("waiting"));

  for (auto const seq : {waiting_seq, running_seq, ended_seq}) {
    INFO("seq " << seq);
    auto const doc = status_object(run_status(arena, std::format("st_fields_{}", seq), seq));
    CHECK(keys_of(doc) == k_fields);
    CHECK(int_of(doc, "seq") == seq);
  }

  auto const running = status_object(run_status(arena, "st_fields_running", running_seq));
  CHECK(text_of(running, "state") == "running");
  CHECK(int_of(running, "slots") == 2);
  CHECK(int_of(running, "grace_ms") == 3000);
  CHECK(int_of(running, "run_limit_ms") == 300'000);
  CHECK(int_of(running, "wait_limit_ms") == 600'000);
  CHECK(text_of(running, "cwd") == "/work/project");
  auto const& argv = member(running, "argv");
  REQUIRE(argv.kind == json::json_kind::array);
  REQUIRE(argv.array.size() == 2);
  CHECK(argv.array[0].string == "make");
  CHECK(argv.array[1].string == "test");
  CHECK(text_of(running, "label") == "running");
  CHECK(text_of(running, "vendor") == "claude");
  CHECK(text_of(running, "role") == "coder");
  CHECK(member(running, "nested").kind == json::json_kind::boolean);
  CHECK_FALSE(member(running, "nested").boolean);
  CHECK(is_null(running, "parent_seq"));

  auto const waiting = status_object(run_status(arena, "st_fields_waiting", waiting_seq));
  CHECK(text_of(waiting, "state") == "waiting");
  CHECK(int_of(waiting, "position") == 1);
  CHECK(is_null(waiting, "started_at"));
  CHECK(is_null(waiting, "ended_at"));
  CHECK(is_null(waiting, "ran_ms"));
  CHECK(is_null(waiting, "outcome"));
  CHECK(is_null(waiting, "exit_code"));
  CHECK(is_null(waiting, "run_limit_ms"));  // not started
  CHECK(is_null(waiting, "wait_limit_ms")); // submitted without one

  auto const ended = status_object(run_status(arena, "st_fields_ended", ended_seq));
  CHECK(text_of(ended, "state") == "ended");
  CHECK(is_null(ended, "live"));
  CHECK(is_null(ended, "position"));
  CHECK(is_null(ended, "slots"));
  CHECK(is_null(ended, "grace_ms"));
  CHECK(is_null(ended, "cancelled_by"));
  CHECK(is_null(ended, "superseded_by"));
  CHECK(text_of(ended, "outcome") == "exited");
  CHECK(int_of(ended, "exit_code") == 5);
  CHECK(text_of(ended, "log_path") == "/logs/ended.log");
  CHECK(is_null(ended, "run_limit_ms")); // ended without ever starting
  CHECK(is_null(ended, "wait_limit_ms"));
}

// Task 7089 (hq-queue-limit-columns): the limits are the ones the submitter
// was given, read back from the store, through the whole life of a ticket.
// Real detached submitters carry `--timeout` and `--wait-timeout` from argv
// to the store, so this pins the verb's wiring, not only the engine's.
TEST_CASE("queue status: a detached run reports the --timeout and --wait-timeout it was submitted with, waiting, running "
          "and ended",
          "[cmd][agent][queue][hq-queue-status][hq-queue-limit-columns]") {
  auto const arena = parity::make_arena("qs_limits");
  write_config(arena, k_fast_poll);
  gate            hold(arena.cpp_root / "hold.fifo");
  submitter_guard guard;

  auto const holder = submit_detached(arena, "limits_holder", sh_command("read x < \"$1\"", {hold.path.string()}),
                                      {"--timeout", "5m", "--wait-timeout", "10m"}, guard);
  await_state(arena, holder.seq, hq::entry_state::running);
  auto const limited =
      submit_detached(arena, "limits_waiter", sh_command("exit 0"), {"--timeout", "90s", "--wait-timeout", "1h"}, guard);
  auto const defaults = submit_detached(arena, "limits_default", sh_command("exit 0"), {}, guard);

  auto const running = status_object(run_status(arena, "lim_running", holder.seq));
  CHECK(text_of(running, "state") == "running");
  CHECK(int_of(running, "run_limit_ms") == 300'000);
  CHECK(int_of(running, "wait_limit_ms") == 600'000);
  auto const running_text = text_lines(run_status(arena, "lim_running_text", holder.seq, false).out);
  CHECK(text_value(running_text, "run_limit_ms") == "300000");
  CHECK(text_value(running_text, "wait_limit_ms") == "600000");

  // A waiting entry has its wait limit in force; its run limit is set when
  // its turn comes.
  auto const waiting = status_object(run_status(arena, "lim_waiting", limited.seq));
  CHECK(text_of(waiting, "state") == "waiting");
  CHECK(int_of(waiting, "wait_limit_ms") == 3'600'000);
  CHECK(is_null(waiting, "run_limit_ms"));
  auto const waiting_default = status_object(run_status(arena, "lim_waiting_default", defaults.seq));
  CHECK(text_of(waiting_default, "state") == "waiting");
  CHECK(is_null(waiting_default, "wait_limit_ms"));
  CHECK(is_null(waiting_default, "run_limit_ms"));

  hold.release();
  await_history(arena, holder.seq);
  await_history(arena, limited.seq);
  await_history(arena, defaults.seq);

  // The history row carries the limits the entry had when it ended.
  auto const holder_done = status_object(run_status(arena, "lim_holder_done", holder.seq));
  CHECK(text_of(holder_done, "state") == "ended");
  CHECK(int_of(holder_done, "run_limit_ms") == 300'000);
  CHECK(int_of(holder_done, "wait_limit_ms") == 600'000);
  auto const limited_done = status_object(run_status(arena, "lim_limited_done", limited.seq));
  CHECK(text_of(limited_done, "outcome") == "exited");
  CHECK(int_of(limited_done, "run_limit_ms") == 90'000);
  CHECK(int_of(limited_done, "wait_limit_ms") == 3'600'000);
  // No `--timeout` is the 30-minute default; no `--wait-timeout` is no limit.
  auto const defaults_done = status_object(run_status(arena, "lim_default_done", defaults.seq));
  CHECK(text_of(defaults_done, "outcome") == "exited");
  CHECK(int_of(defaults_done, "run_limit_ms") == 1'800'000);
  CHECK(is_null(defaults_done, "wait_limit_ms"));
  auto const defaults_text = text_lines(run_status(arena, "lim_default_done_text", defaults.seq, false).out);
  CHECK(text_value(defaults_text, "run_limit_ms") == "1800000");
  CHECK_FALSE(text_value(defaults_text, "wait_limit_ms").has_value());
}

// Review F1 on task 7089: `queue status` opens the store read-only and never
// migrates it, so after an upgrade it reads a store the older, still running
// submitters left at agent schema version 2. The store is built and seeded
// with raw SQL (the chain up to 00002, rows as that binary wrote them),
// because no code at head writes a version-2 store.
TEST_CASE("queue status: a store still at agent schema version 2 is answered with null limits and left at version 2",
          "[cmd][agent][queue][hq-queue-status][hq-queue-limit-columns]") {
  auto const arena = parity::make_arena("qs_v2store");
  auto const chain = planar::db::agent::migrations();
  REQUIRE(chain.size() >= 3);
  {
    auto raw = planar::db::connection::open(store_path(arena).string());
    REQUIRE(raw.has_value());
    REQUIRE(planar::db::apply_all(*raw, chain.subspan(0, 2), planar::db::k_agent_version_table).has_value());
    REQUIRE(raw->execute(std::format("insert into queue_entries (state, host_id, pid, pid_started, cwd, argv, label, "
                                     "enqueued_at, refreshed_mono, wait_deadline_mono) values ('waiting', '{}', {}, {}, "
                                     "'/work/project', '[\"make\",\"test\"]', 'old-waiter', {}, {}, {});"
                                     "insert into queue_history (seq, outcome, exit_code, cwd, argv, label, enqueued_at, "
                                     "started_at, ended_at, waited_ms, ran_ms) values (40, 'exited', 3, '/work/project', "
                                     "'[\"true\"]', 'old-ended', 900, 950, 990, 50, 40);",
                                     this_host(), this_pid(), this_start(), wall_now(), mono_now(), mono_now() + 600'000))
                .has_value());
  }

  auto const waiting = run_status(arena, "v2_waiting", 1);
  INFO("stderr:\n" << waiting.err);
  CHECK(waiting.code == 0);
  auto const waiting_doc = status_object(waiting);
  CHECK(keys_of(waiting_doc) == k_fields);
  CHECK(text_of(waiting_doc, "state") == "waiting");
  CHECK(text_of(waiting_doc, "label") == "old-waiter");
  CHECK(member(waiting_doc, "live").boolean);
  CHECK(is_null(waiting_doc, "run_limit_ms"));
  CHECK(is_null(waiting_doc, "wait_limit_ms"));

  auto const ended = run_status(arena, "v2_ended", 40);
  INFO("stderr:\n" << ended.err);
  CHECK(ended.code == 0);
  auto const ended_doc = status_object(ended);
  CHECK(text_of(ended_doc, "state") == "ended");
  CHECK(text_of(ended_doc, "outcome") == "exited");
  CHECK(int_of(ended_doc, "exit_code") == 3);
  CHECK(is_null(ended_doc, "run_limit_ms"));
  CHECK(is_null(ended_doc, "wait_limit_ms"));

  auto const text = run_status(arena, "v2_waiting_text", 1, false);
  CHECK(text.code == 0);
  CHECK_FALSE(text_value(text_lines(text.out), "wait_limit_ms").has_value());

  // Read-only: the store was not migrated. Checked through a read-only open,
  // since `open_store` would migrate it.
  auto after = planar::db::agent::open_agent_db_read_only_at(store_path(arena));
  REQUIRE(after.has_value());
  CHECK(planar::db::current_version(*after, planar::db::k_agent_version_table).value() == 2);
}

// ---------------------------------------------------------------------------
// A store ONE MIGRATION BEHIND the binary, for every migration there ever
// is (task 7091; review caveat on 7089).
//
// `queue status` opens the store read-only and never migrates it, so after an
// upgrade it reads whatever the older, still running submitters left: the
// store at (head - 1). Each additive agent migration must therefore leave
// every read path tolerant of a store without its columns. The v2 tests above
// pin that for migration 00003 by name; this case pins it for WHATEVER the
// newest migration is, so a migration that adds a column the reads select
// without a fallback fails here on the day it lands, with no edit to the test.
//
// How it adapts when a migration lands:
//   * The store is built by applying all but the last migration of
//     `planar::db::agent::migrations()`, so "head - 1" moves by itself.
//   * Rows are inserted with raw SQL, as the older binary wrote them, but the
//     column list is read from the store (`pragma_table_info`) instead of
//     being spelled per version: a seed value below is used for each column
//     the store at (head - 1) has, and a column it lacks is skipped. Columns
//     with neither a seed value nor a default are left to their default.
//   * The one thing that needs a human is a NEW `not null` column with no
//     default. It cannot be inserted without a value, so the case FAILS and
//     names the column: add a value for it to `k_entry_seed` or
//     `k_history_seed` below. (A migration that adds such a column to a table
//     with rows cannot be applied in place at all, so this is rare.)
//   * Nothing else changes. A column added at head has no seed value; the
//     store at (head - 1) does not have it, which is the situation under test.
//
// The reads exercised are the four the engine exposes (`find`, `list`,
// `find_history`, `list_history`) and `queue status` through the built binary,
// in JSON and in text, for a waiting entry, a running entry and a history row.
// ---------------------------------------------------------------------------

namespace {

/// @brief One column's value, as SQL text.
struct seed_value {
  std::string column;
  std::string sql;

  // A constructor, not an aggregate: `{"cwd", "'/work'"}` would otherwise read
  // as a pair of iterators when both members are string literals.
  seed_value(std::string_view name, std::string value) : column(name), sql(std::move(value)) {
  }
};

using seed_row = std::vector<seed_value>;

auto sql_quoted(std::string_view text) -> std::string {
  std::string out{"'"};
  for (auto const c : text) {
    out += c == '\'' ? std::string{"''"} : std::string{c};
  }
  out += '\'';
  return out;
}

/// @brief Inserts `row` into `table` of `raw`, using only the columns the
/// table has, and failing the case, by name, when the table has a `not null`
/// column without a default that the row does not supply.
void seed_row_into(planar::db::connection& raw, std::string_view table, const seed_row& row) {
  auto stmt = raw.prepare("select name, \"notnull\", dflt_value is null, pk from pragma_table_info(?)");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->bind_text(1, table).has_value());
  std::vector<std::string> columns;
  std::vector<std::string> values;
  for (auto stepped = stmt->step(); stepped.has_value() && *stepped == planar::db::step_result::row; stepped = stmt->step()) {
    auto const name     = stmt->column_text(0);
    auto const required = stmt->column_int64(1) != 0 && stmt->column_int64(2) != 0 && stmt->column_int64(3) == 0;
    auto const supplied = std::ranges::find(row, name, &seed_value::column);
    if (supplied != row.end()) {
      columns.push_back(name);
      values.push_back(supplied->sql);
    } else if (required) {
      FAIL("agent table " << table << " has a not-null column '" << name
                          << "' without a default that this test does not seed: add a value for it to the seed rows in "
                             "queue_status.t.cpp (task 7091)");
    }
  }
  std::string names;
  std::string vals;
  for (std::size_t i = 0; i < columns.size(); ++i) {
    names += (i == 0 ? "" : ", ") + columns[i];
    vals += (i == 0 ? "" : ", ") + values[i];
  }
  REQUIRE(raw.execute(std::format("insert into {} ({}) values ({});", table, names, vals)).has_value());
}

} // namespace

TEST_CASE("queue status and every engine read answer on a store one migration behind the binary, and leave it there",
          "[cmd][agent][queue][hq-queue-status][hq-status-behind-head]") {
  auto const arena = parity::make_arena("qs_behind");
  auto const chain = planar::db::agent::migrations();
  REQUIRE(chain.size() >= 2);
  auto const previous = chain.size() - 1; // The version the store is left at.

  auto const host    = sql_quoted(this_host());
  auto const pid     = std::to_string(this_pid());
  auto const started = std::to_string(this_start());
  auto const mono    = mono_now();
  auto const entry   = [&](std::string state, std::string label, seed_row extra) {
    seed_row row;
    row.emplace_back("state", sql_quoted(state));
    row.emplace_back("host_id", host);
    row.emplace_back("pid", pid);
    row.emplace_back("pid_started", started);
    row.emplace_back("cwd", "'/work/project'");
    row.emplace_back("argv", "'[\"make\",\"test\"]'");
    row.emplace_back("label", sql_quoted(label));
    row.emplace_back("enqueued_at", std::to_string(wall_now()));
    row.emplace_back("refreshed_mono", std::to_string(mono));
    for (auto& more : extra) {
      row.push_back(std::move(more));
    }
    return row;
  };
  auto const history = seed_row{{"seq", "40"},
                                {"outcome", "'exited'"},
                                {"exit_code", "3"},
                                {"cwd", "'/work/project'"},
                                {"argv", "'[\"true\"]'"},
                                {"label", "'prev-ended'"},
                                {"enqueued_at", "900"},
                                {"started_at", "950"},
                                {"ended_at", "990"},
                                {"waited_ms", "50"},
                                {"ran_ms", "40"}};
  {
    auto raw = planar::db::connection::open(store_path(arena).string());
    REQUIRE(raw.has_value());
    REQUIRE(planar::db::apply_all(*raw, chain.subspan(0, previous), planar::db::k_agent_version_table).has_value());
    seed_row_into(*raw, "queue_entries",
                  entry("waiting", "prev-waiter", {{"wait_deadline_mono", std::to_string(mono + 600'000)}}));
    seed_row_into(*raw, "queue_entries",
                  entry("running", "prev-runner",
                        {{"started_at", std::to_string(wall_now())},
                         {"child_pgid", pid},
                         {"child_started", started},
                         {"deadline_mono", std::to_string(mono + 600'000)}}));
    seed_row_into(*raw, "queue_history", history);
    REQUIRE(static_cast<std::size_t>(planar::db::current_version(*raw, planar::db::k_agent_version_table).value()) == previous);
  }

  // The engine reads, on a read-only connection as `queue status` has.
  {
    auto opened = planar::db::agent::open_agent_db_read_only_at(store_path(arena));
    REQUIRE(opened.has_value());
    auto& conn = *opened;

    auto const found = hq::find(conn, 1);
    REQUIRE(found.has_value());
    REQUIRE(found->has_value());
    CHECK((*found)->label == "prev-waiter");
    CHECK((*found)->state == hq::entry_state::waiting);
    CHECK_FALSE((*found)->run_limit_ms.has_value());
    CHECK_FALSE((*found)->wait_limit_ms.has_value());

    auto const all = hq::list(conn);
    REQUIRE(all.has_value());
    REQUIRE(all->size() == 2);
    CHECK(all->at(1).label == "prev-runner");
    CHECK(all->at(1).state == hq::entry_state::running);

    auto const row = hq::find_history(conn, 40);
    REQUIRE(row.has_value());
    REQUIRE(row->has_value());
    CHECK((*row)->label == "prev-ended");
    CHECK((*row)->ran_ms == 40);
    CHECK_FALSE((*row)->run_limit_ms.has_value());
    CHECK_FALSE((*row)->wait_limit_ms.has_value());

    auto const rows = hq::list_history(conn);
    REQUIRE(rows.has_value());
    REQUIRE(rows->size() == 1);
    CHECK(rows->front().seq == 40);
  }

  // `queue status`, through the built binary.
  for (auto const [seq, label, state] : {std::tuple{1, "prev-waiter", "waiting"}, std::tuple{2, "prev-runner", "running"},
                                         std::tuple{40, "prev-ended", "ended"}}) {
    INFO("entry " << seq);
    auto const doc = status_object(run_status(arena, std::format("behind_{}", seq), seq));
    CHECK(keys_of(doc) == k_fields);
    CHECK(text_of(doc, "state") == state);
    CHECK(text_of(doc, "label") == label);
    CHECK(is_null(doc, "run_limit_ms"));
    CHECK(is_null(doc, "wait_limit_ms"));
    auto const text = run_status(arena, std::format("behind_text_{}", seq), seq, false);
    INFO("stderr:\n" << text.err);
    CHECK(text.code == 0);
    CHECK(text_value(text_lines(text.out), "label") == label);
  }

  // Read-only: the store is still where the older binary left it.
  auto after = planar::db::agent::open_agent_db_read_only_at(store_path(arena));
  REQUIRE(after.has_value());
  CHECK(static_cast<std::size_t>(planar::db::current_version(*after, planar::db::k_agent_version_table).value()) == previous);
}

// ---------------------------------------------------------------------------
// An unusable configuration (task 7090, hq-status-degrade-config).
// ---------------------------------------------------------------------------

TEST_CASE("queue status: an unusable [queue] configuration degrades the answer for a running entry instead of exiting 125",
          "[cmd][agent][queue][hq-queue-status][hq-status-degrade-config]") {
  auto const arena = parity::make_arena("qs_badconfig");
  // `slots = 0` is refused, which makes the whole [queue] table unusable,
  // `stale_after` included: the answer must not borrow its 60 seconds.
  write_config(arena, "[queue]\nslots = 0\nstale_after = \"60s\"\ngrace = \"3s\"\n");
  auto conn = open_store(arena);

  auto const fresh = enqueue_or_fail(conn, alive_request("fresh"));
  start_entry(conn, fresh, 2);
  auto const stale = enqueue_or_fail(conn, alive_request("stale"));
  start_entry(conn, stale, 2);
  // Both submitters are this process, alive, and neither entry records a
  // child group, so liveness is freshness alone. 20 seconds old is live under
  // the default 30-second window; 45 seconds old is not, though it would be
  // under the 60 seconds the broken table names. The staleness is written
  // directly: no verb can backdate a refresh.
  auto const now = mono_now();
  REQUIRE(conn.execute(std::format("update queue_entries set refreshed_mono = {} where seq = {};"
                                   "update queue_entries set refreshed_mono = {} where seq = {};",
                                   now - 20'000, fresh, now - 45'000, stale))
              .has_value());

  auto const run = run_status(arena, "bad_fresh", fresh);
  INFO("stderr:\n" << run.err);
  CHECK(run.code == 0);
  CHECK(run.err.starts_with("warning: queue status: "));
  CHECK(run.err.find("slots") != std::string::npos);
  CHECK(run.err.find("error:") == std::string::npos);
  auto const doc = status_object(run);
  CHECK(keys_of(doc) == k_fields);
  CHECK(text_of(doc, "state") == "running");
  CHECK(is_null(doc, "slots"));
  CHECK(is_null(doc, "grace_ms"));
  CHECK(member(doc, "live").kind == json::json_kind::boolean);
  CHECK(member(doc, "live").boolean);

  auto const stale_doc = status_object(run_status(arena, "bad_stale", stale));
  CHECK(member(stale_doc, "live").kind == json::json_kind::boolean);
  CHECK_FALSE(member(stale_doc, "live").boolean);

  // The text form degrades the same way: exit 0, the warning on stderr, and
  // no slots or grace line.
  auto const text = run_status(arena, "bad_fresh_text", fresh, false);
  CHECK(text.code == 0);
  CHECK(text.err.starts_with("warning: queue status: "));
  auto const lines = text_lines(text.out);
  CHECK(text_value(lines, "state") == "running");
  CHECK(text_value(lines, "live") == "true");
  CHECK_FALSE(text_value(lines, "slots").has_value());
  CHECK_FALSE(text_value(lines, "grace_ms").has_value());

  // Status only read: both entries are still in the store.
  auto const entries = hq::list(conn);
  REQUIRE(entries.has_value());
  CHECK(entries->size() == 2);
}

// ---------------------------------------------------------------------------
// The position among waiting entries.
// ---------------------------------------------------------------------------

TEST_CASE("queue status: with one running entry and two waiting, the second waiting entry is at position 2",
          "[cmd][agent][queue][hq-queue-status]") {
  auto const arena  = parity::make_arena("qs_position");
  auto       conn   = open_store(arena);
  auto const runner = enqueue_or_fail(conn, alive_request("runner"));
  start_entry(conn, runner);
  auto const first  = enqueue_or_fail(conn, alive_request("first"));
  auto const second = enqueue_or_fail(conn, alive_request("second"));

  auto const doc = status_object(run_status(arena, "st_pos2", second));
  CHECK(text_of(doc, "state") == "waiting");
  CHECK(int_of(doc, "position") == 2);
  CHECK(int_of(status_object(run_status(arena, "st_pos1", first)), "position") == 1);
  CHECK(is_null(status_object(run_status(arena, "st_posr", runner)), "position"));
}

// ---------------------------------------------------------------------------
// Who cancelled.
// ---------------------------------------------------------------------------

TEST_CASE("queue status: a cancelled entry shows who cancelled it, in history and while it is being stopped",
          "[cmd][agent][queue][hq-queue-status]") {
  auto const arena = parity::make_arena("qs_cancelled");
  auto       conn  = open_store(arena);

  // An ended entry: the history row carries the canceller.
  auto const ended = enqueue_or_fail(conn, alive_request("ended-by-cancel"));
  REQUIRE(hq::end_entry(conn, ended,
                        hq::end_request{.outcome      = hq::history_outcome::cancelled,
                                        .cancelled_by = hq::canceller{.vendor = "codex", .role = "reviewer", .pid = 4242},
                                        .ended_at     = wall_now()})
              .has_value());
  auto const doc = status_object(run_status(arena, "st_cancelled", ended));
  CHECK(text_of(doc, "state") == "ended");
  CHECK(text_of(doc, "outcome") == "cancelled");
  auto const& by = member(doc, "cancelled_by");
  REQUIRE(by.kind == json::json_kind::object);
  CHECK(keys_of(by) == std::vector<std::string>{"vendor", "role", "pid"});
  CHECK(text_of(by, "vendor") == "codex");
  CHECK(text_of(by, "role") == "reviewer");
  CHECK(int_of(by, "pid") == 4242);

  auto const text = run_status(arena, "st_cancelled_text", ended, false);
  REQUIRE(text.code == 0);
  CHECK(text_value(text_lines(text.out), "cancelled_by") == "vendor=codex role=reviewer pid=4242");

  // A running entry whose cancellation is under way: the entry carries it.
  auto const running = enqueue_or_fail(conn, alive_request("stopping"));
  start_entry(conn, running);
  REQUIRE(hq::record_child(conn, running, 4'000'000, 7).has_value());
  std::vector<std::pair<std::int64_t, int>> signals;
  hq::process_probe                         probe{
      .process_exists     = [](std::int64_t) -> std::expected<bool, ident::error> { return true; },
      .process_start_time = [](std::int64_t) -> std::expected<std::optional<ident::start_time>, ident::error> {
        return std::optional<ident::start_time>{ident::start_time{7}};
      },
      .group_has_members = [](std::int64_t) -> std::expected<bool, ident::error> { return true; }};
  hq::group_signaller const signaller = [&](std::int64_t pgid, int sig) -> std::expected<void, ident::error> {
    signals.emplace_back(pgid, sig);
    return {};
  };
  ident::system_clock clock;
  auto const          begun = hq::begin_terminate(
      conn,
      hq::begin_terminate_request{.seq          = running,
                                  .reason       = hq::stop_reason::cancelled,
                                  .cancelled_by = hq::canceller{.vendor = "claude", .role = "orchestrator", .pid = 777},
                                  .host_id      = this_host()},
      clock, probe, signaller);
  REQUIRE(begun.has_value());
  REQUIRE(begun->status == hq::begin_status::marked);

  auto const stopping = status_object(run_status(arena, "st_stopping", running));
  CHECK(text_of(stopping, "state") == "running");
  CHECK(text_of(stopping, "terminating") == "cancelled");
  auto const& who = member(stopping, "cancelled_by");
  REQUIRE(who.kind == json::json_kind::object);
  CHECK(text_of(who, "vendor") == "claude");
  CHECK(text_of(who, "role") == "orchestrator");
  CHECK(int_of(who, "pid") == 777);
}

// ---------------------------------------------------------------------------
// A re-enqueued ticket.
// ---------------------------------------------------------------------------

TEST_CASE("queue status: a reaped waiter's old sequence number reports the successor's state, through a chain",
          "[cmd][agent][queue][hq-queue-status]") {
  auto const arena  = parity::make_arena("qs_successor");
  auto       conn   = open_store(arena);
  auto const runner = enqueue_or_fail(conn, alive_request("runner"));
  start_entry(conn, runner);

  auto const first = enqueue_or_fail(conn, alive_request("waiter"));
  REQUIRE(
      hq::end_entry(conn, first, hq::end_request{.outcome = hq::history_outcome::abandoned, .ended_at = wall_now()}).has_value());
  auto const rejoined = hq::rejoin(conn, first, alive_request("waiter"));
  REQUIRE(rejoined.has_value());
  REQUIRE(rejoined->status == hq::rejoin_status::rejoined);
  auto const second = rejoined->seq;

  auto const one = status_object(run_status(arena, "st_succ1", first));
  CHECK(int_of(one, "seq") == first);
  CHECK(int_of(one, "superseded_by") == second);
  CHECK(text_of(one, "state") == "waiting");
  CHECK(int_of(one, "position") == 1);
  CHECK(is_null(one, "outcome"));
  auto const lines = text_lines(run_status(arena, "st_succ1_text", first, false).out);
  CHECK(text_value(lines, "seq") == std::to_string(first));
  CHECK(text_value(lines, "superseded_by") == std::to_string(second));
  CHECK(text_value(lines, "state") == "waiting");

  // Asked for by its own number, the successor names no successor.
  CHECK(is_null(status_object(run_status(arena, "st_succ_self", second)), "superseded_by"));

  // The successor is reaped too and rejoins: the chain is followed to its end.
  REQUIRE(hq::end_entry(conn, second, hq::end_request{.outcome = hq::history_outcome::abandoned, .ended_at = wall_now()})
              .has_value());
  auto const again = hq::rejoin(conn, second, alive_request("waiter"));
  REQUIRE(again.has_value());
  REQUIRE(again->status == hq::rejoin_status::rejoined);
  auto const third = again->seq;

  auto const chain = status_object(run_status(arena, "st_succ2", first));
  CHECK(int_of(chain, "seq") == first);
  CHECK(int_of(chain, "superseded_by") == third);
  CHECK(text_of(chain, "state") == "waiting");

  // The final entry starts and ends: the old number now reports that ending.
  REQUIRE(
      hq::end_entry(conn, runner, hq::end_request{.outcome = hq::history_outcome::exited, .exit_code = 0, .ended_at = wall_now()})
          .has_value());
  start_entry(conn, third);
  REQUIRE(
      hq::end_entry(conn, third, hq::end_request{.outcome = hq::history_outcome::exited, .exit_code = 9, .ended_at = wall_now()})
          .has_value());
  auto const finished = status_object(run_status(arena, "st_succ3", first));
  CHECK(int_of(finished, "seq") == first);
  CHECK(int_of(finished, "superseded_by") == third);
  CHECK(text_of(finished, "state") == "ended");
  CHECK(text_of(finished, "outcome") == "exited");
  CHECK(int_of(finished, "exit_code") == 9);
}

// ---------------------------------------------------------------------------
// Liveness, judged with nothing polling and nothing changed.
// ---------------------------------------------------------------------------

TEST_CASE("queue status: an entry whose submitter is gone reports live false and is left in the store",
          "[cmd][agent][queue][hq-queue-status]") {
  auto const arena = parity::make_arena("qs_dead");
  auto       conn  = open_store(arena);

  auto dead_request = alive_request("dead");
  dead_request.pid  = dead_pid();
  auto const dead   = enqueue_or_fail(conn, dead_request);

  auto stale_request           = alive_request("stale");
  stale_request.refreshed_mono = 1; // this process is alive, but the entry was refreshed long ago
  auto const stale             = enqueue_or_fail(conn, stale_request);

  auto const fine = enqueue_or_fail(conn, alive_request("fine"));

  auto const dead_doc = status_object(run_status(arena, "st_dead", dead));
  CHECK(text_of(dead_doc, "state") == "waiting");
  CHECK(member(dead_doc, "live").kind == json::json_kind::boolean);
  CHECK_FALSE(member(dead_doc, "live").boolean);
  CHECK_FALSE(member(status_object(run_status(arena, "st_stale", stale)), "live").boolean);
  CHECK(member(status_object(run_status(arena, "st_fine", fine)), "live").boolean);

  auto const text = text_lines(run_status(arena, "st_dead_text", dead, false).out);
  CHECK(text_value(text, "live") == "false");

  // Nothing was reaped: all three entries remain and no history row exists.
  auto const entries = hq::list(conn);
  REQUIRE(entries.has_value());
  CHECK(entries->size() == 3);
  auto const history = hq::list_history(conn);
  REQUIRE(history.has_value());
  CHECK(history->empty());
}

// ---------------------------------------------------------------------------
// Read-only: the store is unchanged.
// ---------------------------------------------------------------------------

/// @brief Every row the store holds, as text: what a change to any entry or
/// history column would alter.
auto dump_rows(planar::db::connection& conn) -> std::string {
  std::string text;
  auto const  entries = hq::list(conn);
  REQUIRE(entries.has_value());
  for (auto const& e : *entries) {
    text +=
        std::format("E {} {} {} {} {} {} {} {} {} {}|{}|{}\n", e.seq, hq::to_string(e.state), e.pid, e.pid_started,
                    e.refreshed_mono, e.started_at.value_or(-1), e.child_pgid.value_or(-1), e.terminating_since_mono.value_or(-1),
                    e.terminate_reason.value_or("-"), e.cancelled_by.value_or("-"), e.label.value_or("-"), e.cwd);
  }
  auto const history = hq::list_history(conn);
  REQUIRE(history.has_value());
  for (auto const& h : *history) {
    text += std::format("H {} {} {} {} {} {}\n", h.seq, hq::to_string(h.outcome), h.exit_code.value_or(-1),
                        h.successor_seq.value_or(-1), h.ended_at, h.label.value_or("-"));
  }
  return text;
}

TEST_CASE("queue status: asking about every kind of entry leaves the store byte for byte as it was",
          "[cmd][agent][queue][hq-queue-status]") {
  auto const                arena = parity::make_arena("qs_readonly");
  std::vector<std::int64_t> seqs;
  std::string               rows_before;
  {
    auto conn         = open_store(arena);
    auto dead_request = alive_request("dead");
    dead_request.pid  = dead_pid();
    seqs.push_back(enqueue_or_fail(conn, dead_request)); // waiting, not live, unreaped
    seqs.push_back(enqueue_or_fail(conn, alive_request("waiting")));
    auto const ended = enqueue_or_fail(conn, alive_request("ended"));
    REQUIRE(hq::end_entry(conn, ended, hq::end_request{.outcome = hq::history_outcome::abandoned, .ended_at = wall_now()})
                .has_value());
    auto const rejoin = hq::rejoin(conn, ended, alive_request("ended"));
    REQUIRE(rejoin.has_value());
    seqs.push_back(ended);
    rows_before = dump_rows(conn);
  } // the last connection closes; the store settles into its files

  auto const db_before  = read_all(store_path(arena));
  auto const wal_before = read_all(std::filesystem::path{store_path(arena).string() + "-wal"});

  for (auto const seq : seqs) {
    REQUIRE(run_status(arena, std::format("ro_j_{}", seq), seq).code == 0);
    REQUIRE(run_status(arena, std::format("ro_t_{}", seq), seq, false).code == 0);
  }
  REQUIRE(run_status(arena, "ro_unknown", 9999).code != 0);

  CHECK(read_all(store_path(arena)) == db_before);
  // SQLite may leave its `-shm` WAL-index sidecar behind; it holds no store
  // content, so the proof is the database file, its WAL and every row.
  CHECK(read_all(std::filesystem::path{store_path(arena).string() + "-wal"}) == wal_before);
  auto conn = open_store(arena);
  CHECK(dump_rows(conn) == rows_before);
}

TEST_CASE("queue status: a missing store is refused at 125 and is not created", "[cmd][agent][queue][hq-queue-status]") {
  auto const arena = parity::make_arena("qs_nostore");
  REQUIRE_FALSE(std::filesystem::exists(store_path(arena)));

  auto const text = run_status(arena, "st_nostore_text", 1, false);
  CHECK(text.code == 125);
  CHECK(text.out.empty());
  CHECK(text.err.starts_with("error: queue status: "));
  CHECK(text.err.find(store_path(arena).string()) != std::string::npos);

  auto const as_json = run_status(arena, "st_nostore_json", 1);
  CHECK(as_json.code == 125);
  auto const doc = parse_document(as_json.out);
  REQUIRE(doc.kind == json::json_kind::object);
  auto const& error = member(doc, "error");
  CHECK(text_of(error, "verb") == "queue status");
  CHECK_FALSE(text_of(error, "message").empty());

  CHECK_FALSE(std::filesystem::exists(store_path(arena)));
}

TEST_CASE("queue status: still answers when the main database is unusable", "[cmd][agent][queue][hq-queue-status]") {
  auto const arena = parity::make_arena("qs_maindb");
  auto       conn  = open_store(arena);
  auto const seq   = enqueue_or_fail(conn, alive_request("waiting"));
  {
    std::ofstream out(arena.cpp_root / "planar.db", std::ios::binary | std::ios::trunc);
    out << "this is not a database";
  }
  auto const doc = status_object(run_status(arena, "st_maindb", seq));
  CHECK(text_of(doc, "state") == "waiting");
  CHECK(read_all(arena.cpp_root / "planar.db") == "this is not a database");
}

// ---------------------------------------------------------------------------
// Refusals.
// ---------------------------------------------------------------------------

TEST_CASE("queue status: a sequence number that was never issued exits 1 with a message naming it, in text and in JSON",
          "[cmd][agent][queue][hq-queue-status]") {
  auto const arena = parity::make_arena("qs_unknown");
  auto       conn  = open_store(arena);
  static_cast<void>(enqueue_or_fail(conn, alive_request("only")));

  auto const text = run_status(arena, "st_unknown_text", 424242, false);
  CHECK(text.code == 1);
  CHECK(text.out.empty());
  CHECK(text.err.find("424242") != std::string::npos);
  CHECK(text.err.starts_with("error: queue status: "));

  auto const as_json = run_status(arena, "st_unknown_json", 424242);
  CHECK(as_json.code == 1);
  auto const doc = parse_document(as_json.out);
  REQUIRE(doc.kind == json::json_kind::object);
  auto const& error = member(doc, "error");
  CHECK(text_of(error, "verb") == "queue status");
  CHECK(text_of(error, "message").find("424242") != std::string::npos);
  CHECK(int_of(error, "seq") == 424242);
}

TEST_CASE("queue status: a sequence number that is not a positive integer exits 2, and a missing one exits 1",
          "[cmd][agent][queue][hq-queue-status]") {
  auto const arena = parity::make_arena("qs_badarg");
  for (auto const* bad : {"abc", "0", "1.5", "99999999999999999999"}) {
    INFO("seq argument: " << bad);
    auto const run = run_status(arena, "st_bad", bad, false);
    CHECK(run.code == 2);
    CHECK(run.out.empty());
    CHECK(run.err.starts_with("error: queue status: "));
  }
  std::vector<std::string> const none{"queue", "status"};
  auto const                     missing = parity::run_pinned(agent_bin(), none, arena.cpp_root, "st_missing");
  CHECK(missing.code == 1);
  CHECK(missing.out.empty());
}

TEST_CASE("queue status: text is unambiguous for quotes and backslashes, escapes format characters, and cuts long values; JSON "
          "is complete",
          "[cmd][agent][queue][hq-view-escape-fields]") {
  auto const   arena  = parity::make_arena("qs_display");
  auto const   quoted = std::string{"\"already quoted\""};
  auto const   bidi   = std::string{"v\xE2\x80\xAE"
                                    "evil"};
  auto const   role   = std::string(100, 'r') + "RTAIL";
  std::int64_t seq    = 0;
  {
    auto conn      = open_store(arena);
    auto request   = alive_request(quoted);
    request.vendor = bidi;
    request.role   = role;
    seq            = enqueue_or_fail(conn, request);
  }
  auto const text = run_status(arena, "st_display_text", seq, false);
  INFO("stdout:\n" << text.out);
  REQUIRE(text.code == 0);
  auto const lines = text_lines(text.out);
  // A value that starts with a double quote is shown quoted, so it cannot be
  // read as the quoting the view itself uses.
  CHECK(text_value(lines, "label") == std::string{"\"\\\"already quoted\\\"\""});
  // A format character is escaped, never printed.
  CHECK_FALSE(text.out.contains("\xE2\x80\xAE"));
  CHECK(text_value(lines, "vendor") == std::string{"\"v\\u202eevil\""});
  // A long value is cut and marked.
  CHECK_FALSE(text.out.contains("RTAIL"));
  CHECK(text_value(lines, "role") == std::string(47, 'r') + "\xE2\x80\xA6");
  // JSON is complete and unchanged.
  auto const doc = status_object(run_status(arena, "st_display_json", seq));
  CHECK(text_of(doc, "label") == quoted);
  CHECK(text_of(doc, "vendor") == bidi);
  CHECK(text_of(doc, "role") == role);

  // A bare backslash is quoted.
  std::int64_t other = 0;
  {
    auto conn = open_store(arena);
    other     = enqueue_or_fail(conn, alive_request("a\\b"));
  }
  auto const backslash = text_lines(run_status(arena, "st_display_backslash", other, false).out);
  CHECK(text_value(backslash, "label") == std::string{"\"a\\\\b\""});
}

TEST_CASE("queue status: a value that escapes wide is cut by its escaped width",
          "[cmd][agent][queue][hq-view-escape-fields][hq-escaped-cap]") {
  auto const arena    = parity::make_arena("qs_escaped_cap");
  auto const repeated = [](std::string_view unit, std::size_t n) {
    std::string out;
    for (std::size_t i = 0; i < n; ++i) {
      out += unit;
    }
    return out;
  };
  std::int64_t seq = 0;
  {
    auto conn      = open_store(arena);
    auto request   = alive_request(repeated("\xE2\x80\x8B", 300));
    request.vendor = repeated("\xE2\x80\xAE", 300);
    request.role   = repeated("\x01\xFF\n", 100);
    seq            = enqueue_or_fail(conn, request);
  }
  auto const text = run_status(arena, "st_escaped_cap", seq, false);
  REQUIRE(text.code == 0);
  auto const lines = text_lines(text.out);
  for (auto const* key : {"label", "vendor", "role"}) {
    auto const value = text_value(lines, key);
    REQUIRE(value.has_value());
    INFO(key << " is " << value->size() << " bytes");
    CHECK(value->size() < 70);
  }
}

} // namespace
