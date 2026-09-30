// queue.t.cpp: `planar-watch queue` as an operator sees it (plan 1080, task
// hq-watch-queue; tech spec 647 § CLI surface and § Liveness; test spec 649
// scenarios citing task:hq-watch-queue).
//
// Every case runs the BUILT `planar-watch` through `run_pinned`, in an arena
// whose `PLANAR_AGENT_DB` is its own scratch store, so nothing can reach the
// operator's `~/.planar`. `--json` output is parsed with the DOM parser and
// asserted field by field; a substring check would pass for a field renamed or
// moved. The workflow case drives a REAL detached submitter of the built
// `planar-agent` (`queue run --detach`); the state cases seed the store
// through the engine (`enqueue`, `poll`, `begin_terminate`) the way the
// engine's own suites do, because a dead, stale or nested entry cannot be
// produced by a well-behaved submitter.
//
// Synchronisation is by FIFOs and files, not sleeps. Every detached submitter
// a case starts is recorded by pid and start time and stopped on the way out
// of the case, failing or not.

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
import planar.cmd.planar_watch.agentstore;
import planar.cmd.planar_watch.handlers.queue.history;
import planar.process.identity;
import planar.engine.hostqueue;

#include "parity_harness.hpp"

namespace {

namespace hq     = planar::engine::hostqueue;
namespace ident  = planar::process::identity;
namespace json   = planar::json_dom;
namespace parity = planar::cmd::parity;
namespace fs     = std::filesystem;

using parity::capture;

constexpr auto k_budget = std::chrono::seconds(30);

/// @brief The JSON row's fields, in order.
const std::vector<std::string> k_fields{"seq",       "state",      "live",         "position",     "terminating",
                                        "nested",    "parent_seq", "cwd",          "argv",         "label",
                                        "vendor",    "role",       "log_path",     "enqueued_at",  "started_at",
                                        "waited_ms", "ran_ms",     "run_limit_ms", "wait_limit_ms"};

auto watch_bin() -> fs::path {
  return fs::path{PLANAR_CPP_BIN};
}

auto agent_bin() -> fs::path {
  return fs::path{PLANAR_AGENT_CPP_BIN};
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

auto store_path(const parity::arena& arena) -> fs::path {
  return arena.cpp_root / "agent.db";
}

auto open_store(const parity::arena& arena) -> planar::db::connection {
  auto opened = planar::db::agent::open_agent_db_at(store_path(arena));
  REQUIRE(opened.has_value());
  return std::move(*opened);
}

auto bytes_of(const fs::path& p) -> std::string {
  std::ifstream in(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

/// @brief `planar-watch queue [--json]` under the arena's pinned environment.
auto run_queue(const parity::arena& arena, std::string_view tag, bool as_json = true) -> capture {
  std::vector<std::string> args{"queue"};
  if (as_json) {
    args.emplace_back("--json");
  }
  return parity::run_pinned(watch_bin(), args, arena.cpp_root, tag);
}

auto parse_document(const std::string& text) -> json::json_value {
  INFO("stdout:\n" << text);
  auto parsed = json::parse_json(text);
  REQUIRE(parsed.has_value());
  return std::move(*parsed);
}

/// @brief The rows of a successful `--json` run.
auto rows_of(const capture& run) -> std::vector<json::json_value> {
  INFO("stderr:\n" << run.err);
  REQUIRE(run.code == 0);
  auto doc = parse_document(run.out);
  REQUIRE(doc.kind == json::json_kind::array);
  return doc.array;
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

auto bool_of(const json::json_value& object, std::string_view key) -> bool {
  auto const& value = member(object, key);
  REQUIRE(value.kind == json::json_kind::boolean);
  return value.boolean;
}

auto argv_of(const json::json_value& object) -> std::vector<std::string> {
  auto const& value = member(object, "argv");
  REQUIRE(value.kind == json::json_kind::array);
  std::vector<std::string> argv;
  for (auto const& word : value.array) {
    REQUIRE(word.kind == json::json_kind::string);
    argv.push_back(word.string);
  }
  return argv;
}

auto keys_of(const json::json_value& object) -> std::vector<std::string> {
  std::vector<std::string> keys;
  for (auto const& [key, unused] : object.object) {
    keys.push_back(key);
  }
  return keys;
}

/// @brief The lines of `out`, without their terminators.
auto lines_of(const std::string& out) -> std::vector<std::string> {
  std::vector<std::string> lines;
  std::string_view         rest = out;
  while (!rest.empty()) {
    auto const end = rest.find('\n');
    lines.emplace_back(rest.substr(0, end));
    rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end + 1);
  }
  return lines;
}

/// @brief The first `count` whitespace-separated tokens of `line`.
auto leading_tokens(std::string_view line, std::size_t count) -> std::vector<std::string> {
  std::vector<std::string> tokens;
  std::size_t              at = 0;
  while (tokens.size() < count && at < line.size()) {
    while (at < line.size() && line[at] == ' ') {
      ++at;
    }
    auto const end = line.find(' ', at);
    tokens.emplace_back(line.substr(at, end == std::string_view::npos ? end : end - at));
    at = end == std::string_view::npos ? line.size() : end;
  }
  return tokens;
}

/// @brief A FIFO the test holds open read-write; a command reading it blocks
/// until `release()`.
struct gate {
  fs::path path;
  int      fd       = -1;
  bool     released = false;

  explicit gate(fs::path where) : path(std::move(where)) {
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

void write_config(const parity::arena& arena, std::string_view text) {
  auto const path = arena.cpp_root / "config.toml";
  auto const temp = arena.cpp_root / "config.toml.new";
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    out << text;
  }
  fs::rename(temp, path);
}

/// @brief `planar-agent queue run --detach <flags> -- <command>`: the sequence
/// number of the ticket it prints.
auto submit_detached(const parity::arena& arena, std::string_view tag, const std::vector<std::string>& command,
                     std::vector<std::string> flags, submitter_guard& guard) -> std::int64_t {
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
  std::int64_t seq = 0;
  REQUIRE(std::from_chars(run.out.data(), run.out.data() + first, seq).ec == std::errc{});

  auto conn  = open_store(arena);
  auto found = hq::find(conn, seq);
  REQUIRE(found.has_value());
  REQUIRE(found->has_value());
  guard.record((*found)->pid, (*found)->pid_started);
  return seq;
}

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
auto alive_request(std::string cwd, std::vector<std::string> argv) -> hq::enqueue_request {
  return hq::enqueue_request{.host_id        = this_host(),
                             .pid            = this_pid(),
                             .pid_started    = this_start(),
                             .cwd            = std::move(cwd),
                             .argv           = std::move(argv),
                             .vendor         = "claude",
                             .role           = "coder",
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
void start_entry(planar::db::connection& conn, std::int64_t seq, std::int64_t slots) {
  ident::system_clock clock;
  auto const          polled = hq::poll(
      conn,
      hq::poll_request{.seq = seq, .host_id = this_host(), .slots = slots, .stale_after_ms = 60'000, .run_limit_ms = 300'000},
      clock, hq::system_process_probe());
  REQUIRE(polled.has_value());
  REQUIRE(polled->running);
}

// ---------------------------------------------------------------------------
// Empty: a missing store is an empty queue.
// ---------------------------------------------------------------------------

TEST_CASE("queue view: a missing store lists nothing, exits 0 and creates no file", "[cmd][watch][queue][hq-watch-queue]") {
  auto const arena = parity::make_arena("wq_missing");
  REQUIRE_FALSE(fs::exists(store_path(arena)));

  auto const json_run = run_queue(arena, "missing_json");
  INFO("stderr:\n" << json_run.err);
  CHECK(json_run.code == 0);
  auto const doc = parse_document(json_run.out);
  REQUIRE(doc.kind == json::json_kind::array);
  CHECK(doc.array.empty());

  auto const text_run = run_queue(arena, "missing_text", false);
  INFO("stderr:\n" << text_run.err);
  CHECK(text_run.code == 0);
  CHECK(text_run.out.empty());
  CHECK(text_run.err.empty());

  CHECK_FALSE(fs::exists(store_path(arena)));
  CHECK_FALSE(fs::exists(store_path(arena).string() + "-wal"));
}

TEST_CASE("queue view: a store with no entries lists nothing", "[cmd][watch][queue][hq-watch-queue]") {
  auto const arena = parity::make_arena("wq_noentries");
  {
    auto conn = open_store(arena);
  }

  CHECK(rows_of(run_queue(arena, "noentries_json")).empty());
  auto const text = run_queue(arena, "noentries_text", false);
  CHECK(text.code == 0);
  CHECK(text.out.empty());
}

// ---------------------------------------------------------------------------
// The operator workflow: one running entry and two waiting entries.
// ---------------------------------------------------------------------------

TEST_CASE("queue view: one running and two waiting entries list in order with their columns",
          "[cmd][watch][queue][hq-watch-queue]") {
  auto const arena = parity::make_arena("wq_workflow");
  write_config(arena, "[queue]\npoll_interval = \"100ms\"\n");
  gate            hold(arena.cpp_root / "hold.fifo");
  submitter_guard guard;

  // A real detached holder, then a real detached waiter, then a waiter another
  // directory submitted (seeded through the engine: a run is pinned to one
  // working directory).
  auto const holder =
      submit_detached(arena, "holder", sh_command("read x < \"$1\"", {hold.path.string()}), {"--label", "holder"}, guard);
  await_state(arena, holder, hq::entry_state::running);
  auto const   waiter = submit_detached(arena, "waiter", sh_command("exit 0"),
                                        {"--label", "waiter", "--vendor", "codex", "--role", "tester"}, guard);
  std::int64_t other  = 0;
  {
    auto conn = open_store(arena);
    other     = enqueue_or_fail(conn, alive_request("/work/other", {"make", "test"}));
  }

  auto const rows = rows_of(run_queue(arena, "workflow_json"));
  REQUIRE(rows.size() == 3);
  for (auto const& row : rows) {
    CHECK(keys_of(row) == k_fields);
    CHECK(bool_of(row, "live"));
    CHECK_FALSE(bool_of(row, "nested"));
  }

  CHECK(int_of(rows[0], "seq") == holder);
  CHECK(text_of(rows[0], "state") == "running");
  CHECK(is_null(rows[0], "position"));
  CHECK(text_of(rows[0], "label") == "holder");
  CHECK(int_of(rows[0], "started_at") > 0);
  CHECK(int_of(rows[0], "ran_ms") >= 0);
  CHECK(int_of(rows[0], "waited_ms") >= 0);
  CHECK(int_of(rows[0], "run_limit_ms") > 0);
  CHECK_FALSE(is_null(rows[0], "log_path"));
  CHECK(argv_of(rows[0]) == sh_command("read x < \"$1\"", {hold.path.string()}));
  CHECK(text_of(rows[0], "cwd") == (arena.cpp_root / "proj").string());

  CHECK(int_of(rows[1], "seq") == waiter);
  CHECK(text_of(rows[1], "state") == "waiting");
  CHECK(int_of(rows[1], "position") == 1);
  CHECK(text_of(rows[1], "vendor") == "codex");
  CHECK(text_of(rows[1], "role") == "tester");
  CHECK(is_null(rows[1], "started_at"));
  CHECK(is_null(rows[1], "ran_ms"));
  CHECK(int_of(rows[1], "waited_ms") >= 0);
  CHECK(is_null(rows[1], "run_limit_ms"));

  CHECK(int_of(rows[2], "seq") == other);
  CHECK(text_of(rows[2], "state") == "waiting");
  CHECK(int_of(rows[2], "position") == 2);
  CHECK(text_of(rows[2], "cwd") == "/work/other");
  CHECK(argv_of(rows[2]) == std::vector<std::string>{"make", "test"});
  CHECK(text_of(rows[2], "vendor") == "claude");
  CHECK(text_of(rows[2], "role") == "coder");
  CHECK(is_null(rows[2], "label"));

  // The text form: a header, then one line per entry in the same order, with
  // the columns SEQ STATE POS NOTES WAITED RAN VENDOR ROLE LABEL DIRECTORY COMMAND.
  auto const text = run_queue(arena, "workflow_text", false);
  INFO("stdout:\n" << text.out << "stderr:\n" << text.err);
  REQUIRE(text.code == 0);
  auto const lines = lines_of(text.out);
  REQUIRE(lines.size() == 4);
  CHECK(leading_tokens(lines[0], 11) == std::vector<std::string>{"SEQ", "STATE", "POS", "NOTES", "WAITED", "RAN", "VENDOR",
                                                                 "ROLE", "LABEL", "DIRECTORY", "COMMAND"});
  auto const first  = leading_tokens(lines[1], 9);
  auto const second = leading_tokens(lines[2], 9);
  auto const third  = leading_tokens(lines[3], 11);
  CHECK(first[0] == std::to_string(holder));
  CHECK(first[1] == "running");
  CHECK(first[2] == "-");
  CHECK(first[3] == "-");
  CHECK(first[6] == "-");
  CHECK(first[7] == "-");
  CHECK(first[8] == "holder");
  CHECK(second[0] == std::to_string(waiter));
  CHECK(second[1] == "waiting");
  CHECK(second[2] == "1");
  CHECK(second[6] == "codex");
  CHECK(second[7] == "tester");
  CHECK(third[0] == std::to_string(other));
  CHECK(third[2] == "2");
  CHECK(third[6] == "claude");
  CHECK(third[7] == "coder");
  CHECK(third[9] == "/work/other");
  CHECK(third[10] == "make");
  CHECK(lines[3].ends_with("make test"));
  // The argument vector is shell-quoted: the script word is single-quoted.
  CHECK(lines[1].contains("sh -c 'read x < \"$1\"' sh "));

  hold.release();
}

TEST_CASE("queue view: entries list in sequence order, running or waiting, with positions among the waiting",
          "[cmd][watch][queue][hq-watch-queue]") {
  auto const arena  = parity::make_arena("wq_order");
  auto       conn   = open_store(arena);
  auto const first  = enqueue_or_fail(conn, alive_request("/w/one", {"a"}));
  auto const middle = enqueue_or_fail(conn, alive_request("/w/two", {"b"}));
  auto const last   = enqueue_or_fail(conn, alive_request("/w/three", {"c"}));
  // Two slots: the second entry starts while the first still waits, so the
  // running entry has a HIGHER sequence number than a waiting one. Rows stay
  // in sequence order (test spec 649); position counts waiting entries only.
  start_entry(conn, middle, 2);

  auto const rows = rows_of(run_queue(arena, "order_json"));
  REQUIRE(rows.size() == 3);
  CHECK(int_of(rows[0], "seq") == first);
  CHECK(text_of(rows[0], "state") == "waiting");
  CHECK(int_of(rows[0], "position") == 1);
  CHECK(int_of(rows[1], "seq") == middle);
  CHECK(text_of(rows[1], "state") == "running");
  CHECK(is_null(rows[1], "position"));
  CHECK(int_of(rows[2], "seq") == last);
  CHECK(text_of(rows[2], "state") == "waiting");
  CHECK(int_of(rows[2], "position") == 2);

  auto const text  = run_queue(arena, "order_text", false);
  auto const lines = lines_of(text.out);
  REQUIRE(lines.size() == 4);
  CHECK(leading_tokens(lines[1], 3) == std::vector<std::string>{std::to_string(first), "waiting", "1"});
  CHECK(leading_tokens(lines[2], 3) == std::vector<std::string>{std::to_string(middle), "running", "-"});
  CHECK(leading_tokens(lines[3], 3) == std::vector<std::string>{std::to_string(last), "waiting", "2"});
}

// ---------------------------------------------------------------------------
// Edge: a dead entry is shown, marked, and left in place.
// ---------------------------------------------------------------------------

TEST_CASE("queue view: a dead or stale entry is marked not live and is left in the store, byte for byte",
          "[cmd][watch][queue][hq-watch-queue]") {
  auto const   arena    = parity::make_arena("wq_dead");
  std::int64_t live_seq = 0, dead_seq = 0, stale_seq = 0;
  {
    auto conn = open_store(arena);
    live_seq  = enqueue_or_fail(conn, alive_request("/w/live", {"make"}));

    auto dead = alive_request("/w/dead", {"make"});
    dead.pid  = dead_pid();
    dead_seq  = enqueue_or_fail(conn, dead);

    auto stale           = alive_request("/w/stale", {"make"});
    stale.refreshed_mono = mono_now() - 3'600'000; // an hour without a refresh
    stale_seq            = enqueue_or_fail(conn, stale);
  }
  auto const before = bytes_of(store_path(arena));
  auto const when   = fs::last_write_time(store_path(arena));

  auto const rows = rows_of(run_queue(arena, "dead_json"));
  REQUIRE(rows.size() == 3);
  CHECK(int_of(rows[0], "seq") == live_seq);
  CHECK(bool_of(rows[0], "live"));
  CHECK(int_of(rows[1], "seq") == dead_seq);
  CHECK_FALSE(bool_of(rows[1], "live"));
  CHECK(int_of(rows[2], "seq") == stale_seq);
  CHECK_FALSE(bool_of(rows[2], "live"));

  auto const text  = run_queue(arena, "dead_text", false);
  auto const lines = lines_of(text.out);
  REQUIRE(lines.size() == 4);
  CHECK(leading_tokens(lines[1], 4)[3] == "-");
  CHECK(leading_tokens(lines[2], 4)[3] == "NOT-LIVE");
  CHECK(leading_tokens(lines[3], 4)[3] == "NOT-LIVE");

  // Nothing was reaped, refreshed or recorded: the entries are all still
  // there, and the main file did not change.
  CHECK(bytes_of(store_path(arena)) == before);
  CHECK((fs::last_write_time(store_path(arena)) == when));
  auto conn = open_store(arena);
  for (auto const seq : {live_seq, dead_seq, stale_seq}) {
    auto found = hq::find(conn, seq);
    REQUIRE(found.has_value());
    CHECK(found->has_value());
    auto ended = hq::find_history(conn, seq);
    REQUIRE(ended.has_value());
    CHECK_FALSE(ended->has_value());
  }
}

TEST_CASE("queue view: an unusable [queue] configuration degrades to the default window with a warning",
          "[cmd][watch][queue][hq-watch-queue]") {
  auto const arena = parity::make_arena("wq_badconfig");
  write_config(arena, "[queue]\nslots = \"many\"\n");
  {
    auto conn = open_store(arena);
    enqueue_or_fail(conn, alive_request("/w/live", {"make"}));
  }

  auto const run = run_queue(arena, "badconfig_json");
  INFO("stderr:\n" << run.err);
  auto const rows = rows_of(run);
  REQUIRE(rows.size() == 1);
  CHECK(bool_of(rows[0], "live"));
  CHECK(run.err.contains("warning: queue:"));
  CHECK(run.err.contains("[queue] configuration"));
}

TEST_CASE("queue view: liveness is judged against the [queue] staleness window", "[cmd][watch][queue][hq-watch-queue]") {
  // An entry last refreshed 45 seconds ago: stale under the 30 second default,
  // live under a 120 second window.
  auto const arena = parity::make_arena("wq_window");
  {
    auto conn            = open_store(arena);
    auto quiet           = alive_request("/w/quiet", {"make"});
    quiet.refreshed_mono = mono_now() - 45'000;
    enqueue_or_fail(conn, quiet);
  }

  auto const by_default = rows_of(run_queue(arena, "window_default"));
  REQUIRE(by_default.size() == 1);
  CHECK_FALSE(bool_of(by_default[0], "live"));

  write_config(arena, "[queue]\nstale_after = \"120s\"\n");
  auto const widened = rows_of(run_queue(arena, "window_widened"));
  REQUIRE(widened.size() == 1);
  CHECK(bool_of(widened[0], "live"));
}

// ---------------------------------------------------------------------------
// Nested and terminating entries are marked.
// ---------------------------------------------------------------------------

TEST_CASE("queue view: a nested entry is marked with its parent", "[cmd][watch][queue][hq-watch-queue]") {
  auto const   arena  = parity::make_arena("wq_nested");
  std::int64_t parent = 0, child = 0;
  {
    auto conn = open_store(arena);
    parent    = enqueue_or_fail(conn, alive_request("/w/outer", {"make", "all"}));
    start_entry(conn, parent, 1);
    auto nested       = alive_request("/w/outer", {"make", "inner"});
    nested.parent_seq = parent;
    child             = enqueue_or_fail(conn, nested);
  }

  auto const rows = rows_of(run_queue(arena, "nested_json"));
  REQUIRE(rows.size() == 2);
  CHECK(int_of(rows[0], "seq") == parent);
  CHECK_FALSE(bool_of(rows[0], "nested"));
  CHECK(is_null(rows[0], "parent_seq"));
  CHECK(int_of(rows[1], "seq") == child);
  CHECK(text_of(rows[1], "state") == "running");
  CHECK(bool_of(rows[1], "nested"));
  CHECK(int_of(rows[1], "parent_seq") == parent);
  CHECK(is_null(rows[1], "position"));

  auto const lines = lines_of(run_queue(arena, "nested_text", false).out);
  REQUIRE(lines.size() == 3);
  CHECK(leading_tokens(lines[1], 4)[3] == "-");
  CHECK(leading_tokens(lines[2], 4)[3] == std::format("nested:{}", parent));
}

TEST_CASE("queue view: an entry being stopped shows the reason", "[cmd][watch][queue][hq-watch-queue]") {
  auto const   arena = parity::make_arena("wq_terminating");
  std::int64_t seq   = 0;
  {
    auto conn = open_store(arena);
    seq       = enqueue_or_fail(conn, alive_request("/w/stopping", {"make"}));
    start_entry(conn, seq, 1);
    ident::system_clock clock;
    auto const          begun = hq::begin_terminate(
        conn, hq::begin_terminate_request{.seq = seq, .reason = hq::stop_reason::timeout, .host_id = this_host()}, clock,
        hq::system_process_probe(), [](std::int64_t, int) -> std::expected<void, ident::error> { return {}; });
    REQUIRE(begun.has_value());
    REQUIRE(begun->status == hq::begin_status::marked);
  }

  auto const rows = rows_of(run_queue(arena, "terminating_json"));
  REQUIRE(rows.size() == 1);
  CHECK(text_of(rows[0], "terminating") == "timeout");
  auto const lines = lines_of(run_queue(arena, "terminating_text", false).out);
  REQUIRE(lines.size() == 2);
  CHECK(leading_tokens(lines[1], 4)[3] == "stopping:timeout");
}

// ---------------------------------------------------------------------------
// Values that can carry control characters cannot spoof a listing.
// ---------------------------------------------------------------------------

TEST_CASE("queue view: control characters in submitted values are escaped, in text and in JSON",
          "[cmd][watch][queue][hq-watch-queue]") {
  auto const arena       = parity::make_arena("wq_escape");
  auto const evil_label  = std::string{"lbl\n99 running fake"};
  auto const evil_vendor = std::string{"v\x1b[31mred"};
  auto const evil_role   = std::string{"r\x7f"
                                       "del"};
  auto const evil_cwd    = std::string{"/w/a\nb"};
  auto const evil_arg    = std::string{"line1\nline2\r\x1b[2J\xc2\x9b"
                                       "31m"};
  {
    auto conn      = open_store(arena);
    auto request   = alive_request(evil_cwd, {"echo", evil_arg, "tab\there"});
    request.label  = evil_label;
    request.vendor = evil_vendor;
    request.role   = evil_role;
    enqueue_or_fail(conn, request);
  }

  auto const text = run_queue(arena, "escape_text", false);
  INFO("stdout:\n" << text.out);
  REQUIRE(text.code == 0);
  // A header and exactly one row; no raw control byte other than the two
  // line terminators reaches the terminal.
  CHECK(lines_of(text.out).size() == 2);
  for (auto const c : text.out) {
    auto const u = static_cast<unsigned char>(c);
    CHECK_FALSE((u < 0x20 && c != '\n'));
    CHECK(u != 0x7f);
  }
  CHECK(text.out.contains("\\n"));
  CHECK(text.out.contains("\\u001b"));
  CHECK(text.out.contains("\\u007f"));
  // A C1 control (CSI, U+009B) is escaped too, in text and in JSON.
  CHECK(text.out.contains("\\u009b"));
  CHECK_FALSE(text.out.contains("\xc2\x9b"));

  // JSON carries the exact bytes, escaped by the grammar, and none raw.
  auto const json_run = run_queue(arena, "escape_json");
  for (auto const c : json_run.out) {
    auto const u = static_cast<unsigned char>(c);
    CHECK_FALSE((u < 0x20 && c != '\n'));
    CHECK(u != 0x7f);
  }
  CHECK_FALSE(json_run.out.contains("\xc2\x9b"));
  auto const rows = rows_of(json_run);
  REQUIRE(rows.size() == 1);
  CHECK(text_of(rows[0], "label") == evil_label);
  CHECK(text_of(rows[0], "vendor") == evil_vendor);
  CHECK(text_of(rows[0], "role") == evil_role);
  CHECK(text_of(rows[0], "cwd") == evil_cwd);
  CHECK(argv_of(rows[0]) == std::vector<std::string>{"echo", evil_arg, "tab\there"});
}

// ---------------------------------------------------------------------------
// Errors: an incompatible store, and a file that is not an agent store.
// ---------------------------------------------------------------------------

TEST_CASE("queue view: a store from a newer release is refused with exit 7 naming both versions",
          "[cmd][watch][queue][hq-watch-queue]") {
  auto const arena = parity::make_arena("wq_incompat");
  {
    auto conn = open_store(arena);
  }
  auto const head = planar::db::agent::agent_schema_version();
  {
    auto raw = planar::db::connection::open(store_path(arena).string());
    REQUIRE(raw.has_value());
    REQUIRE(
        raw->execute(std::format("insert into agent_schema_migrations (version, compat, description) values ({}, {}, 'probe')",
                                 head + 2, head + 1))
            .has_value());
  }
  auto const before = bytes_of(store_path(arena));

  auto const run = run_queue(arena, "incompat_json");
  CHECK(run.code == 7);
  CHECK(run.out.empty());
  CHECK(run.err.contains(std::to_string(head + 1)));
  CHECK(run.err.contains(std::format("is {}", head)));
  auto const text = run_queue(arena, "incompat_text", false);
  CHECK(text.code == 7);
  CHECK(bytes_of(store_path(arena)) == before);
}

TEST_CASE("queue view: a database that is not an agent store, or not a database, is refused with exit 1",
          "[cmd][watch][queue][hq-watch-queue]") {
  SECTION("a SQLite file with other tables") {
    auto const arena = parity::make_arena("wq_foreign");
    {
      auto raw = planar::db::connection::open(store_path(arena).string());
      REQUIRE(raw.has_value());
      REQUIRE(raw->execute("create table plans (id integer primary key)").has_value());
    }
    auto const run = run_queue(arena, "foreign_json");
    CHECK(run.code == 1);
    CHECK(run.out.empty());
    CHECK(run.err.contains("not an agent store"));
  }
  SECTION("a file that is not SQLite") {
    auto const arena = parity::make_arena("wq_garbage");
    {
      std::ofstream out(store_path(arena), std::ios::binary);
      out << "this is not a database, and it is long enough to have a header to refuse\n";
    }
    auto const run = run_queue(arena, "garbage_json");
    CHECK(run.code == 1);
    CHECK(run.out.empty());
    CHECK_FALSE(run.err.empty());
  }
}

// ---------------------------------------------------------------------------
// The main database is not needed.
// ---------------------------------------------------------------------------

/// @brief The pinned map with the main database's two locators removed.
auto without_main_database(const parity::arena& arena) -> std::vector<parity::pinned_var> {
  auto env = parity::pinned_env(arena.cpp_root);
  for (auto& var : env) {
    if (var.name == "PLANAR_DB" || var.name == "HOME") {
      var.unset = true;
    }
  }
  return env;
}

TEST_CASE("queue view: works with no main database to locate, while every other verb still needs one",
          "[cmd][watch][queue][hq-watch-queue]") {
  auto const arena = parity::make_arena("wq_nomain");
  {
    auto conn = open_store(arena);
    enqueue_or_fail(conn, alive_request("/w/only", {"make"}));
  }
  auto const env = without_main_database(arena);

  auto const listing =
      parity::run_pinned(watch_bin(), std::vector<std::string>{"queue", "--json"}, arena.cpp_root, "nomain_queue", env);
  INFO("stderr:\n" << listing.err);
  auto const rows = rows_of(listing);
  REQUIRE(rows.size() == 1);
  CHECK(text_of(rows[0], "cwd") == "/w/only");
  auto const text = parity::run_pinned(watch_bin(), std::vector<std::string>{"queue"}, arena.cpp_root, "nomain_text", env);
  CHECK(text.code == 0);
  CHECK(lines_of(text.out).size() == 2);

  // The exemption is the `queue` domain's alone: every other verb keeps the
  // exit and the message it had.
  for (auto const& verb : {"ps", "claims", "version", "schema"}) {
    auto const run =
        parity::run_pinned(watch_bin(), std::vector<std::string>{verb}, arena.cpp_root, std::format("nomain_{}", verb), env);
    INFO("verb: " << verb << "\nstderr:\n" << run.err);
    CHECK(run.code == 1);
    CHECK(run.out.empty());
    CHECK(run.err == "error: neither PLANAR_DB nor HOME is set; cannot locate the Planar database\n");
  }
  auto const bare = parity::run_pinned(watch_bin(), std::vector<std::string>{}, arena.cpp_root, "nomain_bare", env);
  CHECK(bare.code == 1);
  CHECK(bare.err == "error: neither PLANAR_DB nor HOME is set; cannot locate the Planar database\n");
}

// ===========================================================================
// `planar-watch queue history` (plan 1080, task hq-watch-history; tech spec
// 647 § CLI surface; test spec 649 scenarios citing task:hq-watch-history).
// ===========================================================================

/// @brief The history row's JSON fields, in order.
const std::vector<std::string> k_history_fields{
    "seq",         "outcome",    "exit_code", "signal",    "cancelled_by", "superseded_by", "nested",
    "parent_seq",  "cwd",        "argv",      "label",     "vendor",       "role",          "log_path",
    "enqueued_at", "started_at", "ended_at",  "waited_ms", "ran_ms",       "run_limit_ms",  "wait_limit_ms"};

/// @brief `planar-watch queue history [flags]` under the arena's pinned environment.
auto run_history(const parity::arena& arena, std::string_view tag, std::vector<std::string> flags = {"--json"}) -> capture {
  std::vector<std::string> args{"queue", "history"};
  for (auto& flag : flags) {
    args.push_back(std::move(flag));
  }
  return parity::run_pinned(watch_bin(), args, arena.cpp_root, tag);
}

/// @brief A clock whose wall reading the test sets; the monotonic reading is the host's.
class wall_clock_at final : public ident::clock {
public:
  std::int64_t wall = 0;

  [[nodiscard]] auto monotonic_ms() -> std::expected<std::int64_t, ident::error> override {
    return real.monotonic_ms();
  }
  [[nodiscard]] auto wall_ms() -> std::int64_t override {
    return wall;
  }

private:
  ident::system_clock real;
};

/// @brief One ended entry to seed through the engine.
struct ended_spec {
  std::string                 cwd = "/w/history";
  std::vector<std::string>    argv{"make", "test"};
  std::optional<std::string>  label;
  std::optional<std::string>  vendor    = "claude";
  std::optional<std::string>  role      = "coder";
  std::int64_t                waited_ms = 1000;
  std::optional<std::int64_t> ran_ms    = 2000; ///< Empty: the entry never started.
  std::optional<std::int64_t> wait_limit_ms;
  hq::end_request             end; ///< `ended_at` is the case's own choice and is set here.
};

/// @brief Enqueues, starts (when it ran) and ends one entry so that its history
/// row has exactly the wait and run times and end time the spec names.
auto seed_ended(planar::db::connection& conn, ended_spec spec) -> std::int64_t {
  auto const ran        = spec.ran_ms.value_or(0);
  auto       request    = alive_request(spec.cwd, spec.argv);
  request.label         = spec.label;
  request.vendor        = spec.vendor;
  request.role          = spec.role;
  request.enqueued_at   = spec.end.ended_at - spec.waited_ms - ran;
  request.wait_limit_ms = spec.wait_limit_ms;
  auto const seq        = enqueue_or_fail(conn, request);
  if (spec.ran_ms) {
    wall_clock_at clock;
    clock.wall        = request.enqueued_at + spec.waited_ms;
    auto const polled = hq::poll(
        conn, hq::poll_request{.seq = seq, .host_id = this_host(), .slots = 1, .stale_after_ms = 60'000, .run_limit_ms = 300'000},
        clock, hq::system_process_probe());
    REQUIRE(polled.has_value());
    REQUIRE(polled->running);
  }
  auto const ended = hq::end_entry(conn, seq, spec.end);
  REQUIRE(ended.has_value());
  REQUIRE(*ended == hq::end_result::ended);
  return seq;
}

auto iso_utc(std::int64_t ms) -> std::string {
  return std::format("{:%Y-%m-%dT%H:%M:%SZ}", std::chrono::sys_seconds{std::chrono::seconds{ms / 1000}});
}

// ---- empty -----------------------------------------------------------------

TEST_CASE("queue history: a missing store lists nothing, exits 0 and creates no file", "[cmd][watch][queue][hq-watch-history]") {
  auto const arena = parity::make_arena("wh_missing");
  REQUIRE_FALSE(fs::exists(store_path(arena)));

  auto const json_run = run_history(arena, "missing_json");
  INFO("stderr:\n" << json_run.err);
  CHECK(json_run.code == 0);
  CHECK(json_run.out == "[]\n");
  CHECK(json_run.err.empty());

  auto const text_run = run_history(arena, "missing_text", {});
  CHECK(text_run.code == 0);
  CHECK(text_run.out.empty());
  CHECK(text_run.err.empty());

  auto const since_run = run_history(arena, "missing_since", {"--since", "1h", "--json"});
  CHECK(since_run.code == 0);
  CHECK(since_run.out == "[]\n");
  CHECK_FALSE(fs::exists(store_path(arena)));
}

TEST_CASE("queue history: a store whose only entries are still running lists no history",
          "[cmd][watch][queue][hq-watch-history]") {
  auto const arena = parity::make_arena("wh_empty");
  {
    auto conn = open_store(arena);
    enqueue_or_fail(conn, alive_request("/w/only", {"make"}));
  }
  auto const json_run = run_history(arena, "empty_json");
  CHECK(json_run.code == 0);
  CHECK(json_run.out == "[]\n");
  auto const text_run = run_history(arena, "empty_text", {});
  CHECK(text_run.code == 0);
  CHECK(text_run.out.empty());
}

// ---- real runs -------------------------------------------------------------

TEST_CASE(
    "queue history: commands that really ended as exited, timeout and cancelled are listed with outcome, times and canceller",
    "[cmd][watch][queue][hq-watch-history]") {
  auto const arena = parity::make_arena("wh_real");
  write_config(arena, "[queue]\npoll_interval = \"100ms\"\ngrace = \"200ms\"\n");
  gate            hold(arena.cpp_root / "hold.fifo");
  submitter_guard guard;

  // 1. A foreground command that exits 3.
  auto const exited = parity::run_pinned(agent_bin(),
                                         std::vector<std::string>{"queue", "run", "--label", "exits-three", "--vendor", "claude",
                                                                  "--role", "coder", "--", "sh", "-c", "exit 3"},
                                         arena.cpp_root, "real_exit");
  INFO("stderr:\n" << exited.err);
  REQUIRE(exited.code == 3);

  // 2. A command stopped at its run limit.
  auto const timed = parity::run_pinned(
      agent_bin(),
      std::vector<std::string>{"queue", "run", "--timeout", "300ms", "--label", "too-slow", "--", "sh", "-c", "sleep 30"},
      arena.cpp_root, "real_timeout");
  INFO("stderr:\n" << timed.err);
  REQUIRE(timed.code == 124);

  // 3. A detached holder that another agent cancels.
  auto const held = submit_detached(arena, "real_hold", sh_command("read x < \"$1\"", {hold.path.string()}),
                                    {"--label", "cancel-me", "--vendor", "codex", "--role", "tester"}, guard);
  await_state(arena, held, hq::entry_state::running);
  auto const cancelled = parity::run_pinned(
      agent_bin(), std::vector<std::string>{"queue", "cancel", std::to_string(held), "--vendor", "claude", "--role", "reviewer"},
      arena.cpp_root, "real_cancel");
  INFO("stderr:\n" << cancelled.err);
  REQUIRE(cancelled.code == 0);
  REQUIRE(await([&] {
    auto opened = planar::db::agent::open_agent_db_at(store_path(arena));
    if (!opened) {
      return false;
    }
    auto found = hq::find_history(*opened, held);
    return found && found->has_value();
  }));

  auto const rows = rows_of(run_history(arena, "real_json"));
  REQUIRE(rows.size() == 3);
  for (auto const& row : rows) {
    CHECK(keys_of(row) == k_history_fields);
    CHECK(int_of(row, "waited_ms") >= 0);
    CHECK(int_of(row, "ended_at") >= int_of(row, "enqueued_at"));
  }
  // Oldest first by end time: the exit, the timeout, then the cancel.
  CHECK(text_of(rows[0], "outcome") == "exited");
  CHECK(int_of(rows[0], "exit_code") == 3);
  CHECK(is_null(rows[0], "signal"));
  CHECK(is_null(rows[0], "cancelled_by"));
  CHECK(text_of(rows[0], "label") == "exits-three");
  CHECK(text_of(rows[0], "vendor") == "claude");
  CHECK(text_of(rows[0], "role") == "coder");
  CHECK(int_of(rows[0], "ran_ms") >= 0);

  CHECK(text_of(rows[1], "outcome") == "timeout");
  CHECK(text_of(rows[1], "label") == "too-slow");
  CHECK(int_of(rows[1], "ran_ms") >= 250);
  CHECK(int_of(rows[1], "run_limit_ms") == 300);

  CHECK(int_of(rows[2], "seq") == held);
  CHECK(text_of(rows[2], "outcome") == "cancelled");
  CHECK(text_of(rows[2], "label") == "cancel-me");
  CHECK(text_of(rows[2], "vendor") == "codex");
  auto const& who = member(rows[2], "cancelled_by");
  REQUIRE(who.kind == json::json_kind::object);
  CHECK(keys_of(who) == std::vector<std::string>{"vendor", "role", "pid"});
  CHECK(text_of(who, "vendor") == "claude");
  CHECK(text_of(who, "role") == "reviewer");
  CHECK(int_of(who, "pid") > 1);

  auto const text = run_history(arena, "real_text", {});
  REQUIRE(text.code == 0);
  auto const lines = lines_of(text.out);
  REQUIRE(lines.size() == 4);
  CHECK(leading_tokens(lines[0], 3) == std::vector<std::string>{"SEQ", "OUTCOME", "RESULT"});
  CHECK(leading_tokens(lines[1], 3)[1] == "exited");
  CHECK(leading_tokens(lines[1], 3)[2] == "code:3");
  CHECK(lines[3].contains("cancelled-by:claude/reviewer/"));
}

// ---- every outcome ---------------------------------------------------------

TEST_CASE("queue history: every outcome lists its columns, in JSON and in text", "[cmd][watch][queue][hq-watch-history]") {
  auto const                arena = parity::make_arena("wh_outcomes");
  auto const                now   = wall_now();
  std::vector<std::int64_t> seqs;
  std::int64_t              rejoined = 0;
  {
    auto conn = open_store(arena);
    auto add  = [&](ended_spec spec, std::int64_t minutes_ago) {
      spec.end.ended_at = now - minutes_ago * 60'000;
      seqs.push_back(seed_ended(conn, std::move(spec)));
    };
    ended_spec exited;
    exited.label     = "build";
    exited.waited_ms = 1500;
    exited.ran_ms    = 65'000;
    exited.end       = hq::end_request{.outcome = hq::history_outcome::exited, .exit_code = 0};
    add(exited, 50);

    ended_spec signaled;
    signaled.end = hq::end_request{.outcome = hq::history_outcome::signaled, .signal = 9};
    add(signaled, 40);

    ended_spec timeout;
    timeout.ran_ms = 300'000;
    timeout.end    = hq::end_request{.outcome = hq::history_outcome::timeout};
    add(timeout, 30);

    ended_spec cancelled;
    cancelled.end = hq::end_request{.outcome      = hq::history_outcome::cancelled,
                                    .cancelled_by = hq::canceller{.vendor = "claude", .role = "reviewer", .pid = 4242}};
    add(cancelled, 20);

    ended_spec not_started;
    not_started.waited_ms = 300;
    not_started.ran_ms    = 5;
    not_started.end       = hq::end_request{.outcome = hq::history_outcome::not_started, .exit_code = 127};
    add(not_started, 15);

    ended_spec abandoned;
    abandoned.ran_ms = std::nullopt;
    abandoned.end    = hq::end_request{.outcome = hq::history_outcome::abandoned};
    add(abandoned, 10);

    ended_spec wait_timeout;
    wait_timeout.ran_ms        = std::nullopt;
    wait_timeout.waited_ms     = 5000;
    wait_timeout.wait_limit_ms = 5000;
    wait_timeout.end           = hq::end_request{.outcome = hq::history_outcome::wait_timeout};
    add(wait_timeout, 5);

    ended_spec anonymous;
    anonymous.vendor = std::nullopt;
    anonymous.role   = std::nullopt;
    anonymous.end    = hq::end_request{.outcome      = hq::history_outcome::cancelled,
                                       .cancelled_by = hq::canceller{.vendor = std::nullopt, .role = std::nullopt, .pid = 77}};
    add(anonymous, 4);

    // The abandoned waiter rejoins the queue: its history row names the new entry.
    auto const back = hq::rejoin(conn, seqs[5], alive_request("/w/history", {"make", "test"}));
    REQUIRE(back.has_value());
    REQUIRE(back->status == hq::rejoin_status::rejoined);
    rejoined = back->seq;
  }

  auto const rows = rows_of(run_history(arena, "outcomes_json"));
  REQUIRE(rows.size() == 8);
  for (auto const& row : rows) {
    CHECK(keys_of(row) == k_history_fields);
    CHECK(text_of(row, "cwd") == "/w/history");
    CHECK_FALSE(bool_of(row, "nested"));
    CHECK(is_null(row, "parent_seq"));
    CHECK(is_null(row, "log_path"));
  }
  // Oldest end first, which here is also sequence order.
  for (std::size_t i = 0; i < rows.size(); ++i) {
    CHECK(int_of(rows[i], "seq") == seqs[i]);
  }

  // exited: the exit code, exact times, the limit the run had.
  CHECK(text_of(rows[0], "outcome") == "exited");
  CHECK(int_of(rows[0], "exit_code") == 0);
  CHECK(is_null(rows[0], "signal"));
  CHECK(is_null(rows[0], "cancelled_by"));
  CHECK(is_null(rows[0], "superseded_by"));
  CHECK(argv_of(rows[0]) == std::vector<std::string>{"make", "test"});
  CHECK(text_of(rows[0], "label") == "build");
  CHECK(text_of(rows[0], "vendor") == "claude");
  CHECK(text_of(rows[0], "role") == "coder");
  CHECK(int_of(rows[0], "ended_at") == now - 50 * 60'000);
  CHECK(int_of(rows[0], "enqueued_at") == now - 50 * 60'000 - 1500 - 65'000);
  CHECK(int_of(rows[0], "started_at") == now - 50 * 60'000 - 65'000);
  CHECK(int_of(rows[0], "waited_ms") == 1500);
  CHECK(int_of(rows[0], "ran_ms") == 65'000);
  CHECK(int_of(rows[0], "run_limit_ms") == 300'000);
  CHECK(is_null(rows[0], "wait_limit_ms"));

  // signaled: the signal instead of a code.
  CHECK(text_of(rows[1], "outcome") == "signaled");
  CHECK(is_null(rows[1], "exit_code"));
  CHECK(int_of(rows[1], "signal") == 9);

  CHECK(text_of(rows[2], "outcome") == "timeout");
  CHECK(is_null(rows[2], "exit_code"));
  CHECK(is_null(rows[2], "signal"));
  CHECK(int_of(rows[2], "ran_ms") == 300'000);

  // cancelled: who did it.
  CHECK(text_of(rows[3], "outcome") == "cancelled");
  auto const& who = member(rows[3], "cancelled_by");
  REQUIRE(who.kind == json::json_kind::object);
  CHECK(keys_of(who) == std::vector<std::string>{"vendor", "role", "pid"});
  CHECK(text_of(who, "vendor") == "claude");
  CHECK(text_of(who, "role") == "reviewer");
  CHECK(int_of(who, "pid") == 4242);

  CHECK(text_of(rows[4], "outcome") == "not_started");
  CHECK(int_of(rows[4], "exit_code") == 127);

  // abandoned then rejoined: the successor, and never started.
  CHECK(text_of(rows[5], "outcome") == "abandoned");
  CHECK(int_of(rows[5], "superseded_by") == rejoined);
  CHECK(is_null(rows[5], "started_at"));
  CHECK(is_null(rows[5], "ran_ms"));
  CHECK(is_null(rows[5], "run_limit_ms"));

  CHECK(text_of(rows[6], "outcome") == "wait_timeout");
  CHECK(int_of(rows[6], "waited_ms") == 5000);
  CHECK(int_of(rows[6], "wait_limit_ms") == 5000);
  CHECK(is_null(rows[6], "ran_ms"));

  // a canceller whose vendor and role were never given: null members, a pid.
  CHECK(is_null(rows[7], "vendor"));
  CHECK(is_null(rows[7], "role"));
  auto const& nobody = member(rows[7], "cancelled_by");
  REQUIRE(nobody.kind == json::json_kind::object);
  CHECK(is_null(nobody, "vendor"));
  CHECK(is_null(nobody, "role"));
  CHECK(int_of(nobody, "pid") == 77);

  // The same rows as text: one line each after a header, columns in order.
  auto const text = run_history(arena, "outcomes_text", {});
  INFO("stdout:\n" << text.out);
  REQUIRE(text.code == 0);
  auto const lines = lines_of(text.out);
  REQUIRE(lines.size() == 9);
  CHECK(leading_tokens(lines[0], 6) == std::vector<std::string>{"SEQ", "OUTCOME", "RESULT", "ENDED", "WAITED", "RAN"});
  CHECK(lines[0].contains("NOTES"));
  CHECK(leading_tokens(lines[1], 6) ==
        std::vector<std::string>{std::to_string(seqs[0]), "exited", "code:0", iso_utc(now - 50 * 60'000), "1s", "1m05s"});
  CHECK(leading_tokens(lines[2], 3) == std::vector<std::string>{std::to_string(seqs[1]), "signaled", "signal:9"});
  CHECK(leading_tokens(lines[3], 3) == std::vector<std::string>{std::to_string(seqs[2]), "timeout", "-"});
  CHECK(lines[4].contains("cancelled-by:claude/reviewer/4242"));
  CHECK(leading_tokens(lines[5], 3) == std::vector<std::string>{std::to_string(seqs[4]), "not_started", "code:127"});
  CHECK(leading_tokens(lines[6], 6)[5] == "-"); // an abandoned waiter never ran
  CHECK(lines[6].contains(std::format("superseded-by:{}", rejoined)));
  CHECK(lines[7].contains("wait_timeout"));
  CHECK(lines[8].contains("cancelled-by:-/-/77"));
  CHECK(lines[1].ends_with("make test"));
  CHECK(lines[1].contains("/w/history"));
  CHECK(lines[1].contains("build"));
}

// ---- --since ---------------------------------------------------------------

TEST_CASE("queue history: --since keeps only rows that ended within the duration", "[cmd][watch][queue][hq-watch-history]") {
  auto const   arena       = parity::make_arena("wh_since");
  auto const   now         = wall_now();
  std::int64_t two_hours   = 0;
  std::int64_t two_minutes = 0;
  std::int64_t inside      = 0;
  std::int64_t outside     = 0;
  {
    auto conn = open_store(arena);
    auto add  = [&](std::int64_t ms_ago) {
      ended_spec spec;
      spec.waited_ms = 10;
      spec.ran_ms    = 10;
      spec.end       = hq::end_request{.outcome = hq::history_outcome::exited, .exit_code = 0, .ended_at = now - ms_ago};
      return seed_ended(conn, std::move(spec));
    };
    two_hours   = add(2 * 3'600'000);
    inside      = add(58'000);
    outside     = add(62'000);
    two_minutes = add(2 * 60'000);
  }
  auto const seqs_of = [&](const std::vector<json::json_value>& rows) {
    std::vector<std::int64_t> out;
    for (auto const& row : rows) {
      out.push_back(int_of(row, "seq"));
    }
    return out;
  };

  // The spec's own example: two hours ago and two minutes ago, `--since 1h`
  // returns the recent ones only (the two 6x-second rows are boundary probes).
  // Oldest end first: 2m ago, 62s ago, 58s ago.
  CHECK(seqs_of(rows_of(run_history(arena, "since_1h_order", {"--since", "1h", "--json"}))) ==
        std::vector<std::int64_t>{two_minutes, outside, inside});
  CHECK(seqs_of(rows_of(run_history(arena, "since_3h", {"--since", "3h", "--json"}))) ==
        std::vector<std::int64_t>{two_hours, two_minutes, outside, inside});
  // A minute: the row that ended 58s ago stays, the one that ended 62s ago and everything older goes.
  CHECK(seqs_of(rows_of(run_history(arena, "since_1m", {"--since", "1m", "--json"}))) == std::vector<std::int64_t>{inside});
  // Milliseconds and seconds spell the same cutoff.
  CHECK(seqs_of(rows_of(run_history(arena, "since_60s", {"--since", "60s", "--json"}))) == std::vector<std::int64_t>{inside});
  CHECK(seqs_of(rows_of(run_history(arena, "since_60000ms", {"--since", "60000ms", "--json"}))) ==
        std::vector<std::int64_t>{inside});
  // Nothing ended in the last second.
  CHECK(rows_of(run_history(arena, "since_1s", {"--since", "1s", "--json"})).empty());
  // Text honours it too, and the flag order does not matter.
  auto const text = run_history(arena, "since_text", {"--since", "1m"});
  REQUIRE(text.code == 0);
  CHECK(lines_of(text.out).size() == 2);
  CHECK(leading_tokens(lines_of(text.out)[1], 1)[0] == std::to_string(inside));
  auto const flipped = run_history(arena, "since_flipped", {"--json", "--since", "1m"});
  CHECK(seqs_of(rows_of(flipped)) == std::vector<std::int64_t>{inside});
}

TEST_CASE("queue history: --since takes days and keeps a week of history", "[cmd][watch][queue][hq-watch-history][hq-since-days]") {
  auto const   arena    = parity::make_arena("wh_since_days");
  auto const   now      = wall_now();
  std::int64_t six_days = 0;
  std::int64_t eight    = 0;
  {
    auto conn = open_store(arena);
    auto add  = [&](std::int64_t ms_ago) {
      ended_spec spec;
      spec.waited_ms = 10;
      spec.ran_ms    = 10;
      spec.end       = hq::end_request{.outcome = hq::history_outcome::exited, .exit_code = 0, .ended_at = now - ms_ago};
      return seed_ended(conn, std::move(spec));
    };
    eight    = add(8 * 86'400'000LL);
    six_days = add(6 * 86'400'000LL);
  }
  auto const seqs_of = [&](const std::vector<json::json_value>& rows) {
    std::vector<std::int64_t> out;
    for (auto const& row : rows) {
      out.push_back(int_of(row, "seq"));
    }
    return out;
  };
  // 7d keeps the row that ended 6 days ago and drops the one 8 days ago.
  CHECK(seqs_of(rows_of(run_history(arena, "days_7d", {"--since", "7d", "--json"}))) == std::vector<std::int64_t>{six_days});
  CHECK(seqs_of(rows_of(run_history(arena, "days_9d", {"--since", "9d", "--json"}))) ==
        std::vector<std::int64_t>{eight, six_days});
  // 168h is the same cutoff as 7d.
  CHECK(seqs_of(rows_of(run_history(arena, "days_168h", {"--since", "168h", "--json"}))) == std::vector<std::int64_t>{six_days});
  // The cap is the retention maximum, inclusive: 36500d and its hour spelling are accepted.
  CHECK(run_history(arena, "days_cap", {"--since", "36500d", "--json"}).code == 0);
  CHECK(run_history(arena, "days_cap_h", {"--since", "876000h", "--json"}).code == 0);
  auto const over = run_history(arena, "days_over", {"--since", "36501d", "--json"});
  CHECK(over.code == 2);
  CHECK(over.out.empty());
  CHECK(over.err.contains("'36501d'"));
  CHECK(run_history(arena, "days_zero", {"--since", "0d", "--json"}).code == 2);
  CHECK(run_history(arena, "days_bare", {"--since", "7", "--json"}).code == 2);
}

TEST_CASE("queue history: an invalid --since is refused at exit 2 and names the value", "[cmd][watch][queue][hq-watch-history]") {
  auto const arena = parity::make_arena("wh_badsince");
  {
    auto       conn = open_store(arena);
    ended_spec spec;
    spec.end = hq::end_request{.outcome = hq::history_outcome::exited, .exit_code = 0, .ended_at = wall_now()};
    seed_ended(conn, spec);
  }
  for (std::string const bad : {"banana", "5", "0s", "0d", "0ms", "-1d", "36501d", "876001h", "1.5h", "1w", ""}) {
    auto const run = run_history(arena, std::format("badsince_{}", bad.empty() ? "empty" : bad), {"--since", bad, "--json"});
    INFO("value: '" << bad << "'\nstderr:\n" << run.err);
    CHECK(run.code == 2);
    CHECK(run.out.empty());
    CHECK(run.err.contains("--since"));
    if (!bad.empty()) {
      CHECK(run.err.contains(std::format("'{}'", bad)));
    }
  }
  // Refused whether or not a store exists.
  auto const bare = parity::make_arena("wh_badsince_bare");
  auto const run  = run_history(bare, "badsince_nostore", {"--since", "banana"});
  CHECK(run.code == 2);
  CHECK(run.out.empty());
  CHECK(run.err.contains("'banana'"));
  CHECK_FALSE(fs::exists(store_path(bare)));
}

TEST_CASE("queue history: the --since cutoff is exact to the millisecond and a row ending on it is kept",
          "[cmd][watch][queue][hq-watch-history]") {
  // The black-box cases above run against the real clock, so they can only
  // probe the boundary to within a second. This pins it exactly: the cutoff is
  // `now - duration`, and the store read keeps a row that ended AT the cutoff
  // and drops the one that ended a millisecond before it.
  constexpr std::int64_t now    = 1'800'000'000'000;
  constexpr std::int64_t hour   = 3'600'000;
  auto const             cutoff = planar::cmd::watch::handlers::since_cutoff_ms(now, hour);
  CHECK(cutoff == now - hour);
  CHECK(planar::cmd::watch::handlers::since_cutoff_ms(hour, hour) == 0);
  CHECK(planar::cmd::watch::handlers::since_cutoff_ms(now, 1) == now - 1);

  auto const                arena = parity::make_arena("wh_cutoff");
  std::vector<std::int64_t> seqs;
  {
    auto conn = open_store(arena);
    for (auto const ended_at : {cutoff - 1, cutoff, cutoff + 1}) {
      ended_spec spec;
      spec.waited_ms = 1;
      spec.ran_ms    = 1;
      spec.end       = hq::end_request{.outcome = hq::history_outcome::exited, .exit_code = 0, .ended_at = ended_at};
      seqs.push_back(seed_ended(conn, spec));
    }
  }
  auto store = planar::cmd::watch::open_agent_store_at(store_path(arena));
  REQUIRE(store.has_value());
  auto const kept = store->history(cutoff);
  REQUIRE(kept.has_value());
  REQUIRE(kept->size() == 2);
  CHECK(kept->at(0).seq == seqs[1]);
  CHECK(kept->at(1).seq == seqs[2]);
  CHECK(store->history()->size() == 3);
}

// ---- an older store, and a store that must not change ----------------------

TEST_CASE("queue history: a store still at agent schema version 2 lists with null limits and stays at version 2",
          "[cmd][watch][queue][hq-watch-history]") {
  auto const arena = parity::make_arena("wh_v2");
  auto const chain = planar::db::agent::migrations();
  REQUIRE(chain.size() >= 3);
  {
    auto raw = planar::db::connection::open(store_path(arena).string());
    REQUIRE(raw.has_value());
    REQUIRE(planar::db::apply_all(*raw, chain.subspan(0, 2), planar::db::k_agent_version_table).has_value());
    REQUIRE(raw->execute("insert into queue_history (seq, outcome, exit_code, cwd, argv, label, vendor, role, enqueued_at, "
                         "started_at, ended_at, waited_ms, ran_ms) values (7, 'exited', 0, '/w', '[\"true\"]', 'old-h', "
                         "'claude', 'coder', 900, 950, 990, 50, 40)")
                .has_value());
  }
  auto const before = bytes_of(store_path(arena));

  auto const rows = rows_of(run_history(arena, "v2_json"));
  REQUIRE(rows.size() == 1);
  CHECK(keys_of(rows[0]) == k_history_fields);
  CHECK(int_of(rows[0], "seq") == 7);
  CHECK(text_of(rows[0], "label") == "old-h");
  CHECK(int_of(rows[0], "ran_ms") == 40);
  CHECK(is_null(rows[0], "run_limit_ms"));
  CHECK(is_null(rows[0], "wait_limit_ms"));

  auto const text = run_history(arena, "v2_text", {});
  CHECK(text.code == 0);
  CHECK(lines_of(text.out).size() == 2);
  CHECK(rows_of(run_history(arena, "v2_since", {"--since", "1h", "--json"})).empty());

  CHECK(bytes_of(store_path(arena)) == before);
  auto opened = planar::db::connection::open_read_only(store_path(arena).string());
  REQUIRE(opened.has_value());
  CHECK(planar::db::current_version(*opened, planar::db::k_agent_version_table).value() == 2);
}

TEST_CASE("queue history: reading changes nothing in the store", "[cmd][watch][queue][hq-watch-history]") {
  auto const arena = parity::make_arena("wh_readonly");
  {
    auto       conn = open_store(arena);
    ended_spec spec;
    spec.end = hq::end_request{.outcome = hq::history_outcome::exited, .exit_code = 0, .ended_at = wall_now()};
    seed_ended(conn, spec);
    enqueue_or_fail(conn, alive_request("/w/waiting", {"make"}));
  }
  auto const before_bytes = bytes_of(store_path(arena));
  auto const before_time  = fs::last_write_time(store_path(arena));

  for (auto const& flags :
       std::vector<std::vector<std::string>>{{}, {"--json"}, {"--since", "1h"}, {"--since", "1ms", "--json"}}) {
    auto const run = run_history(arena, "readonly", flags);
    INFO("stderr:\n" << run.err);
    CHECK(run.code == 0);
  }
  CHECK(bytes_of(store_path(arena)) == before_bytes);
  CHECK((fs::last_write_time(store_path(arena)) == before_time));
  // The waiting entry is still there: history reads never reap.
  auto conn = open_store(arena);
  CHECK(hq::list(conn).value().size() == 1);
}

// ---- queue still lists -----------------------------------------------------

TEST_CASE("planar-watch queue still lists after history lands", "[cmd][watch][queue][hq-watch-history]") {
  // `queue` is now a node with a child, and a node with children used to print
  // help. It must stay a verb of its own: running and waiting entries list, the
  // ended one does not, and the two views never show each other's rows.
  auto const   arena   = parity::make_arena("wh_both");
  std::int64_t ended   = 0;
  std::int64_t running = 0;
  std::int64_t waiting = 0;
  {
    auto       conn = open_store(arena);
    ended_spec spec;
    spec.label = "finished";
    spec.end   = hq::end_request{.outcome = hq::history_outcome::exited, .exit_code = 0, .ended_at = wall_now()};
    ended      = seed_ended(conn, spec);
    running    = enqueue_or_fail(conn, alive_request("/w/run", {"make", "build"}));
    start_entry(conn, running, 1);
    waiting = enqueue_or_fail(conn, alive_request("/w/wait", {"make", "test"}));
  }

  auto const listing = run_queue(arena, "both_json");
  auto const rows    = rows_of(listing);
  REQUIRE(rows.size() == 2);
  CHECK(keys_of(rows[0]) == k_fields);
  CHECK(int_of(rows[0], "seq") == running);
  CHECK(text_of(rows[0], "state") == "running");
  CHECK(int_of(rows[1], "seq") == waiting);
  CHECK(text_of(rows[1], "state") == "waiting");

  auto const text = run_queue(arena, "both_text", false);
  REQUIRE(text.code == 0);
  auto const lines = lines_of(text.out);
  REQUIRE(lines.size() == 3);
  CHECK(leading_tokens(lines[0], 2) == std::vector<std::string>{"SEQ", "STATE"});
  CHECK_FALSE(text.out.contains("Usage"));
  CHECK_FALSE(text.out.contains("SUBCOMMANDS"));

  auto const history = rows_of(run_history(arena, "both_history"));
  REQUIRE(history.size() == 1);
  CHECK(int_of(history[0], "seq") == ended);
  CHECK(text_of(history[0], "label") == "finished");

  // `--json` given before the child word reaches the right verb too.
  auto const before_child =
      parity::run_pinned(watch_bin(), std::vector<std::string>{"queue", "--json", "history"}, arena.cpp_root, "both_flag_first");
  CHECK(rows_of(before_child).size() == 1);

  // The group's help page names the child; asking for it prints help, not rows.
  auto const help = parity::run_pinned(watch_bin(), std::vector<std::string>{"queue", "--help"}, arena.cpp_root, "both_help");
  CHECK(help.code == 0);
  CHECK(help.out.contains("history"));
  CHECK(help.out.contains("SUBCOMMANDS"));
  auto const child_help =
      parity::run_pinned(watch_bin(), std::vector<std::string>{"queue", "history", "--help"}, arena.cpp_root, "both_child_help");
  CHECK(child_help.code == 0);
  CHECK(child_help.out.contains("OPTIONS"));
  CHECK(child_help.out.contains("--since"));
  CHECK(child_help.out.contains("--json"));
}

// ---- escaping --------------------------------------------------------------

TEST_CASE("queue history: control characters in submitted values are escaped, in text and in JSON",
          "[cmd][watch][queue][hq-watch-history]") {
  auto const arena       = parity::make_arena("wh_escape");
  auto const evil_label  = std::string{"lbl\n99 exited fake"};
  auto const evil_vendor = std::string{"v\x1b[31mred"};
  auto const evil_role   = std::string{"r\x7f"
                                       "del"};
  auto const evil_cwd    = std::string{"/w/a\nb"};
  auto const evil_arg    = std::string{"line1\nline2\r\x1b[2J\xc2\x9b"
                                       "31m"};
  auto const evil_who    = std::string{"c\nfake-row"};
  {
    auto       conn = open_store(arena);
    ended_spec spec;
    spec.cwd    = evil_cwd;
    spec.argv   = {"echo", evil_arg, "tab\there"};
    spec.label  = evil_label;
    spec.vendor = evil_vendor;
    spec.role   = evil_role;
    spec.end    = hq::end_request{.outcome      = hq::history_outcome::cancelled,
                                  .cancelled_by = hq::canceller{.vendor = evil_who, .role = evil_role, .pid = 5},
                                  .ended_at     = wall_now()};
    seed_ended(conn, spec);
  }

  auto const text = run_history(arena, "escape_text", {});
  INFO("stdout:\n" << text.out);
  REQUIRE(text.code == 0);
  CHECK(lines_of(text.out).size() == 2);
  for (auto const c : text.out) {
    auto const u = static_cast<unsigned char>(c);
    CHECK_FALSE((u < 0x20 && c != '\n'));
    CHECK(u != 0x7f);
  }
  CHECK(text.out.contains("\\n"));
  CHECK(text.out.contains("\\u001b"));
  CHECK(text.out.contains("\\u007f"));
  CHECK(text.out.contains("\\u009b"));
  CHECK_FALSE(text.out.contains("\xc2\x9b"));

  auto const json_run = run_history(arena, "escape_json");
  for (auto const c : json_run.out) {
    auto const u = static_cast<unsigned char>(c);
    CHECK_FALSE((u < 0x20 && c != '\n'));
    CHECK(u != 0x7f);
  }
  CHECK_FALSE(json_run.out.contains("\xc2\x9b"));
  auto const rows = rows_of(json_run);
  REQUIRE(rows.size() == 1);
  CHECK(text_of(rows[0], "label") == evil_label);
  CHECK(text_of(rows[0], "vendor") == evil_vendor);
  CHECK(text_of(rows[0], "role") == evil_role);
  CHECK(text_of(rows[0], "cwd") == evil_cwd);
  CHECK(argv_of(rows[0]) == std::vector<std::string>{"echo", evil_arg, "tab\there"});
  CHECK(text_of(member(rows[0], "cancelled_by"), "vendor") == evil_who);
}

// ---- errors ----------------------------------------------------------------

TEST_CASE("queue history: a store from a newer release is refused with exit 7 naming both versions",
          "[cmd][watch][queue][hq-watch-history]") {
  auto const arena = parity::make_arena("wh_incompat");
  {
    auto conn = open_store(arena);
  }
  auto const head = planar::db::agent::agent_schema_version();
  {
    auto raw = planar::db::connection::open(store_path(arena).string());
    REQUIRE(raw.has_value());
    REQUIRE(
        raw->execute(std::format("insert into agent_schema_migrations (version, compat, description) values ({}, {}, 'probe')",
                                 head + 2, head + 1))
            .has_value());
  }
  auto const before = bytes_of(store_path(arena));

  auto const run = run_history(arena, "incompat_json");
  CHECK(run.code == 7);
  CHECK(run.out.empty());
  CHECK(run.err.contains(std::to_string(head + 1)));
  CHECK(run.err.contains(std::format("is {}", head)));
  CHECK(run_history(arena, "incompat_text", {}).code == 7);
  CHECK(run_history(arena, "incompat_since", {"--since", "1h"}).code == 7);
  CHECK(bytes_of(store_path(arena)) == before);
}

TEST_CASE("queue history: a database that is not an agent store, or not a database, is refused with exit 1",
          "[cmd][watch][queue][hq-watch-history]") {
  SECTION("a SQLite file with other tables") {
    auto const arena = parity::make_arena("wh_foreign");
    {
      auto raw = planar::db::connection::open(store_path(arena).string());
      REQUIRE(raw.has_value());
      REQUIRE(raw->execute("create table plans (id integer primary key)").has_value());
    }
    auto const run = run_history(arena, "foreign_json");
    CHECK(run.code == 1);
    CHECK(run.out.empty());
    CHECK(run.err.contains("not an agent store"));
  }
  SECTION("a file that is not SQLite") {
    auto const arena = parity::make_arena("wh_garbage");
    {
      std::ofstream out(store_path(arena), std::ios::binary);
      out << "this is not a database, and it is long enough to have a header to refuse\n";
    }
    auto const run = run_history(arena, "garbage_json");
    CHECK(run.code == 1);
    CHECK(run.out.empty());
    CHECK_FALSE(run.err.empty());
  }
}

// ---- the main database is not needed ---------------------------------------

TEST_CASE("queue history: works with no main database to locate", "[cmd][watch][queue][hq-watch-history]") {
  auto const arena = parity::make_arena("wh_nomain");
  {
    auto       conn = open_store(arena);
    ended_spec spec;
    spec.cwd = "/w/only";
    spec.end = hq::end_request{.outcome = hq::history_outcome::exited, .exit_code = 0, .ended_at = wall_now()};
    seed_ended(conn, spec);
  }
  auto const env = without_main_database(arena);

  auto const listing = parity::run_pinned(watch_bin(), std::vector<std::string>{"queue", "history", "--json"}, arena.cpp_root,
                                          "nomain_history", env);
  INFO("stderr:\n" << listing.err);
  auto const rows = rows_of(listing);
  REQUIRE(rows.size() == 1);
  CHECK(text_of(rows[0], "cwd") == "/w/only");
  auto const text = parity::run_pinned(watch_bin(), std::vector<std::string>{"queue", "history", "--since", "1h"}, arena.cpp_root,
                                       "nomain_text", env);
  CHECK(text.code == 0);
  CHECK(lines_of(text.out).size() == 2);
}

} // namespace
