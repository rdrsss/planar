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
import planar.json_dom;
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

TEST_CASE("queue view: running entries come first, then waiting entries in queue order, with positions among the waiting",
          "[cmd][watch][queue][hq-watch-queue]") {
  auto const arena  = parity::make_arena("wq_order");
  auto       conn   = open_store(arena);
  auto const first  = enqueue_or_fail(conn, alive_request("/w/one", {"a"}));
  auto const middle = enqueue_or_fail(conn, alive_request("/w/two", {"b"}));
  auto const last   = enqueue_or_fail(conn, alive_request("/w/three", {"c"}));
  // Two slots: the second entry starts while the first still waits, so the
  // running entry has a HIGHER sequence number than a waiting one.
  start_entry(conn, middle, 2);

  auto const rows = rows_of(run_queue(arena, "order_json"));
  REQUIRE(rows.size() == 3);
  CHECK(int_of(rows[0], "seq") == middle);
  CHECK(text_of(rows[0], "state") == "running");
  CHECK(is_null(rows[0], "position"));
  CHECK(int_of(rows[1], "seq") == first);
  CHECK(text_of(rows[1], "state") == "waiting");
  CHECK(int_of(rows[1], "position") == 1);
  CHECK(int_of(rows[2], "seq") == last);
  CHECK(int_of(rows[2], "position") == 2);

  auto const text  = run_queue(arena, "order_text", false);
  auto const lines = lines_of(text.out);
  REQUIRE(lines.size() == 4);
  CHECK(leading_tokens(lines[1], 3) == std::vector<std::string>{std::to_string(middle), "running", "-"});
  CHECK(leading_tokens(lines[2], 3) == std::vector<std::string>{std::to_string(first), "waiting", "1"});
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
  auto const evil_arg    = std::string{"line1\nline2\r\x1b[2J"};
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

  // JSON carries the exact bytes, escaped by the grammar, and none raw.
  auto const json_run = run_queue(arena, "escape_json");
  for (auto const c : json_run.out) {
    auto const u = static_cast<unsigned char>(c);
    CHECK_FALSE((u < 0x20 && c != '\n'));
    CHECK(u != 0x7f);
  }
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

} // namespace
