// queue_run_checks.t.cpp: the checks `planar-agent queue run` makes on a
// command, as an operator sees them (plan 1080, tasks hq-command-guard and
// hq-not-started; tech spec 647 § Submitting, § Which commands may be queued,
// § Running).
//
// Every case runs the BUILT binary through `run_pinned` (or `spawn_queue`
// for the one case that needs a submitter waiting), in an arena with its own
// `PLANAR_DB`, `HOME` and `PLANAR_CONFIG_PATH`, so nothing can reach the
// operator's `~/.planar`. The store is read back through the engine's own
// typed reads: an exit code alone would pass for a refusal that enqueued the
// command first and ended it after.
//
// The programs a case queues are small scripts in the arena's `fakebin`
// directory, which is put first on `PATH`. A script that ran would create a
// marker file, so "the command was never started" is a file that does not
// exist rather than an inference from an exit code.
//
// The waiting case synchronises by files and a FIFO, not by sleeps: the first
// command blocks reading a FIFO the test holds open read-write, and the test
// learns that an entry is waiting from the store. The only clocks are the
// bounded waits that turn a hang into a failure.

#include <catch2/catch_test_macros.hpp>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

import std;
import planar.db;
import planar.db.migrate;
import planar.db.migrations;
import planar.engine.hostqueue;

#include "parity_harness.hpp"
#include "queue_test_store.hpp"

namespace {

namespace hq     = planar::engine::hostqueue;
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

/// @brief The program names the tech spec lists as model launchers. Spelled
/// out here rather than read from the engine, so a change to the list has to
/// change this test too.
constexpr std::array<std::string_view, 7> k_launchers{"claude", "codex",    "gemini",      "copilot",
                                                      "aider",  "opencode", "cursor-agent"};

auto agent_bin() -> std::filesystem::path {
  return std::filesystem::path{PLANAR_CPP_BIN};
}

auto fakebin(const parity::arena& arena) -> std::filesystem::path {
  return arena.cpp_root / "fakebin";
}

auto marker(const parity::arena& arena, std::string_view name) -> std::filesystem::path {
  return arena.cpp_root / std::format("ran_{}", name);
}

auto present(const std::filesystem::path& path) -> bool {
  std::error_code ec;
  return std::filesystem::exists(path, ec);
}

/// @brief Writes an executable shell script named `name` into the arena's
/// `fakebin`: it creates `ran_<name>` and exits 0.
auto make_program(const parity::arena& arena, std::string_view name) -> std::filesystem::path {
  std::filesystem::create_directories(fakebin(arena));
  auto const path = fakebin(arena) / name;
  {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << "#!/bin/sh\n: > " << parity::shell_quote(marker(arena, name).string()) << "\n";
  }
  std::filesystem::permissions(path, std::filesystem::perms::owner_all);
  return path;
}

/// @brief The pinned environment plus `fakebin` in front of the inherited
/// `PATH`.
auto env_with_fakebin(const parity::arena& arena) -> std::vector<pinned_var> {
  auto       env       = parity::pinned_env(arena.cpp_root);
  auto const inherited = std::getenv("PATH");
  env.push_back(
      pinned_var{.name  = "PATH",
                 .value = std::format("{}:{}", fakebin(arena).string(), inherited != nullptr ? inherited : "/usr/bin:/bin")});
  return env;
}

/// @brief The arguments of `queue run -- <command>`.
auto queue_args(const std::vector<std::string>& command) -> std::vector<std::string> {
  std::vector<std::string> args{"queue", "run", "--"};
  for (auto const& word : command) {
    args.push_back(word);
  }
  return args;
}

auto run_queue(const parity::arena& arena, std::string_view tag, const std::vector<std::string>& command) -> capture {
  auto const args = queue_args(command);
  auto const env  = env_with_fakebin(arena);
  return parity::run_pinned(agent_bin(), args, arena.cpp_root, tag, env);
}

/// @brief What the store holds at one instant.
struct snapshot {
  std::vector<hq::entry>       entries;
  std::vector<hq::history_row> history;
};

/// @brief Reads the arena's store, or nothing when it does not exist or
/// cannot be read at this instant.
auto try_snapshot(const parity::arena& arena) -> std::optional<snapshot> {
  auto const path = arena.cpp_root / "planar.db";
  if (!present(path)) {
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

/// @brief Requires that the store was never touched: nothing was enqueued and
/// the database was never even opened. The guard and the 126/127 checks run
/// before the store is opened (tech spec 647 § Submitting). The arena's
/// `planar.db` exists (the queue verbs never create it), so "never opened" is
/// read from SQLite's own footprint: a database in WAL mode that any
/// connection touches grows a `-wal` and `-shm`, and the seeding connection
/// removed them when it closed. Every caller runs in an arena no earlier case
/// has touched.
void require_nothing_enqueued(const parity::arena& arena) {
  CHECK(planar::cmd::qfix::untouched(arena.cpp_root / "planar.db"));
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

/// @brief Waits for entry `seq` to be in `state`.
void await_entry(const parity::arena& arena, std::int64_t seq, hq::entry_state state) {
  seq = planar::cmd::qfix::seq_of(seq);
  INFO("waiting for entry " << seq);
  REQUIRE(await([&] {
    auto const snap = try_snapshot(arena);
    if (!snap) {
      return false;
    }
    return std::ranges::any_of(snap->entries, [&](const hq::entry& e) { return e.seq == seq && e.state == state; });
  }));
}

void write_config(const parity::arena& arena, std::string_view text) {
  std::ofstream out(arena.cpp_root / "config.toml", std::ios::binary | std::ios::trunc);
  out << text;
}

/// @brief A FIFO the test holds open read-write. A command that reads it
/// blocks until `release()`, and a failed assertion cannot leave it blocked.
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

/// @brief A `queue run` started in the background by `spawn_queue`.
struct spawned {
  std::filesystem::path root;
  std::string           tag;
};

/// @brief Starts `planar-agent queue run -- <command>` in the background
/// under the arena's pinned environment plus `fakebin` on `PATH`, and
/// returns at once. Not `run_pinned` on a thread: `std::system` serialises
/// concurrent callers on macOS, so a second `run_pinned` would not start until
/// the first ended.
auto spawn_queue(const parity::arena& arena, std::string tag, const std::vector<std::string>& command) -> spawned {
  auto const vars = env_with_fakebin(arena);

  std::string child = parity::pinned_env_prefix(vars) + parity::shell_quote(agent_bin().string());
  for (auto const& arg : queue_args(command)) {
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

} // namespace

// ---------------------------------------------------------------------------
// Scenario: Error: a model launcher is refused
// ---------------------------------------------------------------------------

TEST_CASE("queue run: a model launcher is refused before anything is enqueued", "[cmd][agent][queue][queue-guard]") {
  auto const arena = seeded_arena("qc_guard");

  for (auto const name : k_launchers) {
    // A real, runnable program of that name is on PATH: only the guard can
    // stop it, and a run would leave the marker behind.
    auto const program = make_program(arena, name);

    std::string upper{name};
    std::ranges::transform(upper, upper.begin(),
                           [](char c) { return static_cast<char>(std::toupper(static_cast<unsigned char>(c))); });

    std::vector<std::pair<std::string, std::vector<std::string>>> forms{
        {"bare name", {std::string{name}}},
        {"upper-case name", {upper}},
        {"directory prefix", {program.string()}},
        {"relative directory prefix", {std::format("../fakebin/{}", name)}},
        {"env assignment", {"env", "VAR=value", std::string{name}}},
        {"env -i", {"env", "-i", std::string{name}}},
        {"env -u NAME", {"env", "-u", "NAME", std::string{name}}},
        {"env with its own path", {"/usr/bin/env", std::string{name}}},
        {"bare assignment", {"VAR=value", std::string{name}}},
        {"arguments after the launcher", {std::string{name}, "-p", "hello"}},
    };
    for (auto const& [form, command] : forms) {
      INFO(name << " as " << form);
      auto const got = run_queue(arena, "guard", command);
      INFO("stderr:\n" << got.err);
      CHECK(got.code == 2);
      CHECK(got.out.empty());
      CHECK(got.err.find(name) != std::string::npos);
      CHECK_FALSE(present(marker(arena, name)));
      require_nothing_enqueued(arena);
    }
  }
}

TEST_CASE("queue run: env options that take a value do not hide the launcher", "[cmd][agent][queue][queue-guard]") {
  auto const arena = seeded_arena("qc_guard_env");
  make_program(arena, "claude");

  std::vector<std::pair<std::string, std::vector<std::string>>> forms{
      {"env -S", {"env", "-S", "claude -p hi"}},
      {"env --split-string=", {"env", "--split-string=claude -p hi"}},
      {"env -iu NAME", {"env", "-iu", "NAME", "claude"}},
      {"env -uNAME", {"env", "-uNAME", "claude"}},
      {"env --unset=NAME", {"env", "--unset=NAME", "claude"}},
      {"env -C DIR", {"env", "-C", "/tmp", "claude"}},
      {"env -L USER", {"env", "-L", "someuser", "claude"}},
      {"env -U USER", {"env", "-U", "someuser", "claude"}},
      {"env then --", {"env", "-i", "--", "claude"}},
      {"env then assignment", {"env", "-i", "A=1", "B=2", "claude"}},
      {"env twice", {"env", "env", "claude"}},
  };
  for (auto const& [form, command] : forms) {
    INFO(form);
    auto const got = run_queue(arena, "guard_env", command);
    INFO("stderr:\n" << got.err);
    CHECK(got.code == 2);
    CHECK(got.err.find("claude") != std::string::npos);
    CHECK_FALSE(present(marker(arena, "claude")));
    require_nothing_enqueued(arena);
  }
}

// ---------------------------------------------------------------------------
// Scenario: Edge: a program whose name only contains a listed word is allowed
// ---------------------------------------------------------------------------

TEST_CASE("queue run: a program whose name only contains a listed word is queued and runs", "[cmd][agent][queue][queue-guard]") {
  auto const arena = seeded_arena("qc_guard_allowed");

  for (std::string_view name : {"codex-lint-report", "claude_fixture"}) {
    make_program(arena, name);
    auto const got = run_queue(arena, "allowed", {std::string{name}});
    INFO(name << " stderr:\n" << got.err);
    CHECK(got.code == 0);
    CHECK(present(marker(arena, name)));
  }
  // An env that runs a listed-looking non-launcher is queued and runs it.
  make_program(arena, "claudette");
  auto const wrapped = run_queue(arena, "allowed_env_run", {"env", "A=1", "claudette"});
  INFO("stderr:\n" << wrapped.err);
  CHECK(wrapped.code == 0);
  CHECK(present(marker(arena, "claudette")));

  // `-i` empties the environment, `env` then cannot find `claudette` and
  // exits 127. That is the command's own status, recorded as `exited`: the
  // guard did not refuse it (2), and the queue did not refuse it (125/127
  // before the enqueue), it ran `env` and `env` failed.
  std::filesystem::remove(marker(arena, "claudette"));
  auto const emptied = run_queue(arena, "allowed_env_i", {"env", "-i", "A=1", "claudette"});
  INFO("stderr:\n" << emptied.err);
  CHECK(emptied.code == 127);
  CHECK_FALSE(present(marker(arena, "claudette")));

  auto const snap = require_snapshot(arena);
  CHECK(snap.entries.empty());
  auto const exited =
      std::ranges::count_if(snap.history, [](const hq::history_row& r) { return r.outcome == hq::history_outcome::exited; });
  CHECK(exited == 4);
  CHECK(snap.history.size() == 4);
}

// ---------------------------------------------------------------------------
// Scenarios: Error: a missing command exits 127; one that cannot be
// executed exits 126
// ---------------------------------------------------------------------------

TEST_CASE("queue run: a missing command exits 127 and leaves no entry and no history row",
          "[cmd][agent][queue][queue-notstarted]") {
  auto const arena = seeded_arena("qc_missing");

  SECTION("a bare name found nowhere on PATH") {
    auto const got = run_queue(arena, "bare", {"no-such-program-xyz"});
    INFO("stderr:\n" << got.err);
    CHECK(got.code == 127);
    CHECK(got.out.empty());
    CHECK(got.err.find("no-such-program-xyz") != std::string::npos);
  }
  SECTION("a path that does not exist") {
    auto const got = run_queue(arena, "path", {"/nonexistent-dir-xyz/tool"});
    INFO("stderr:\n" << got.err);
    CHECK(got.code == 127);
    CHECK(got.err.find("/nonexistent-dir-xyz/tool") != std::string::npos);
  }
  require_nothing_enqueued(arena);
}

TEST_CASE("queue run: a command that cannot be executed exits 126 and leaves no entry and no history row",
          "[cmd][agent][queue][queue-notstarted]") {
  auto const arena = seeded_arena("qc_notexec");
  std::filesystem::create_directories(fakebin(arena));
  auto const plain = fakebin(arena) / "plainfile";
  {
    std::ofstream out(plain, std::ios::binary | std::ios::trunc);
    out << "#!/bin/sh\nexit 0\n";
  }
  std::filesystem::permissions(plain, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);

  SECTION("a path to a file without the execute bit") {
    auto const got = run_queue(arena, "path", {plain.string()});
    INFO("stderr:\n" << got.err);
    CHECK(got.code == 126);
    CHECK(got.out.empty());
    CHECK(got.err.find("plainfile") != std::string::npos);
  }
  SECTION("a bare name that matches only a file without the execute bit") {
    auto const got = run_queue(arena, "bare", {"plainfile"});
    INFO("stderr:\n" << got.err);
    CHECK(got.code == 126);
  }
  SECTION("a directory") {
    auto const got = run_queue(arena, "dir", {fakebin(arena).string()});
    INFO("stderr:\n" << got.err);
    CHECK(got.code == 126);
  }
  require_nothing_enqueued(arena);
}

TEST_CASE("queue run: a file with the execute bit and no interpreter line ends as not started with 126",
          "[cmd][agent][queue][queue-notstarted]") {
  // It passes the check made before the enqueue (it is executable) and fails
  // at exec, so it is enqueued, gets its turn, and ends `not_started`.
  auto const arena = seeded_arena("qc_noshebang");
  std::filesystem::create_directories(fakebin(arena));
  auto const program = fakebin(arena) / "script-without-shebang";
  {
    std::ofstream out(program, std::ios::binary | std::ios::trunc);
    out << "echo this is not a binary\n";
  }
  std::filesystem::permissions(program, std::filesystem::perms::owner_all);

  auto const got = run_queue(arena, "noshebang", {program.string()});
  INFO("stderr:\n" << got.err);
  CHECK(got.code == 126);
  CHECK(got.out.empty());

  auto const snap = require_snapshot(arena);
  CHECK(snap.entries.empty());
  REQUIRE(snap.history.size() == 1);
  auto const& row = snap.history.front();
  CHECK(row.outcome == hq::history_outcome::not_started);
  CHECK(row.exit_code == 126);
  CHECK_FALSE(row.signal.has_value());
  CHECK(row.argv == std::vector<std::string>{program.string()});
}

// ---------------------------------------------------------------------------
// Scenario: Edge: a program that vanishes while queued ends as not started
// ---------------------------------------------------------------------------

TEST_CASE("queue run: a program that vanishes while queued ends as not started and the next entry starts",
          "[cmd][agent][queue][queue-notstarted]") {
  auto const arena = seeded_arena("qc_vanish");
  write_config(arena, "[queue]\npoll_interval = \"100ms\"\n");
  gate       release(arena.cpp_root / "release.fifo");
  auto const started = arena.cpp_root / "started_first";
  auto const vanish  = make_program(arena, "vanishing");
  make_program(arena, "afterwards");

  auto const first = spawn_queue(
      arena, "first", {"sh", "-c", "echo up > \"$1\"; read x < \"$2\"; exit 0", "sh", started.string(), release.path.string()});
  REQUIRE(await([&] { return present(started); }));

  auto const second = spawn_queue(arena, "second", {"vanishing"});
  await_entry(arena, 2, hq::entry_state::waiting);
  auto const third = spawn_queue(arena, "third", {"afterwards"});
  await_entry(arena, 3, hq::entry_state::waiting);

  // The program is gone by the time entry 2's turn comes.
  REQUIRE(std::filesystem::remove(vanish));
  release.release();

  auto const a = finish(first);
  auto const b = finish(second);
  auto const c = finish(third);
  INFO("second stderr:\n" << b.err << "third stderr:\n" << c.err);
  CHECK(a.code == 0);
  CHECK(b.code == 127);
  CHECK(b.err.find("vanishing") != std::string::npos);
  CHECK(c.code == 0);
  CHECK_FALSE(present(marker(arena, "vanishing")));
  CHECK(present(marker(arena, "afterwards")));

  auto const snap = require_snapshot(arena);
  CHECK(snap.entries.empty());
  REQUIRE(snap.history.size() == 3);
  auto const not_started =
      std::ranges::count_if(snap.history, [](const hq::history_row& r) { return r.outcome == hq::history_outcome::not_started; });
  CHECK(not_started == 1);
  auto const row =
      std::ranges::find_if(snap.history, [](const hq::history_row& r) { return r.seq == planar::cmd::qfix::seq_of(2); });
  REQUIRE(row != snap.history.end());
  CHECK(row->outcome == hq::history_outcome::not_started);
  CHECK(row->exit_code == 127);
  auto const next =
      std::ranges::find_if(snap.history, [](const hq::history_row& r) { return r.seq == planar::cmd::qfix::seq_of(3); });
  REQUIRE(next != snap.history.end());
  CHECK(next->outcome == hq::history_outcome::exited);
}
