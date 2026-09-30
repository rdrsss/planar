// queue_run_seams.t.cpp: `queue run` driven in-process through its seams
// (plan 1080, task hq-queue-run-verb, folded task 7055).
//
// The black-box cases in queue_run.t.cpp cannot make a signal fail: whether
// `kill(2)` refuses depends on someone else's process group. Here the real
// tree and the real handler run in this process, with a process probe and a
// signaller the test controls, so a failure the handler must surface can be
// produced on demand. The store and the command are real: the command is
// `true`, run in a scratch directory, and the store is a scratch agent
// database.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

import std;
import planar.cliapp.args;
import planar.cmd.planar_agent.context;
import planar.cmd.planar_agent.exit;
import planar.cmd.planar_agent.handler;
import planar.cmd.planar_agent.handlers.queue;
import planar.db;
import planar.db.agentdb;
import planar.engine.config.queue;
import planar.engine.hostqueue;
import planar.process.identity;

namespace {

namespace hq    = planar::engine::hostqueue;
namespace ident = planar::process::identity;
namespace agent = planar::cmd::agent;

/// @brief A scratch root with a working directory and an agent database
/// path, removed on destruction.
struct scratch {
  std::filesystem::path root;

  scratch()
      : root(std::filesystem::temp_directory_path() /
             std::format("planar_queue_seams_{}", std::chrono::steady_clock::now().time_since_epoch().count())) {
    std::filesystem::create_directories(root / "proj");
    std::filesystem::create_directories(root / "fakehome");
  }
  scratch(const scratch&)            = delete;
  scratch& operator=(const scratch&) = delete;
  ~scratch() {
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
  }
};

struct invocation {
  int         code = -1;
  std::string out;
  std::string err;
};

/// @brief The last line of `text`, without its newline; empty when there is none.
auto last_line(std::string_view text) -> std::string {
  while (text.ends_with('\n')) {
    text.remove_suffix(1);
  }
  auto const start = text.rfind('\n');
  return std::string{start == std::string_view::npos ? text : text.substr(start + 1)};
}

/// @brief The handler's parsed arguments for `queue run -- <command>`, as the
/// tree's `command` positional would harvest them.
auto queue_args(const std::vector<std::string>& command) -> planar::cliapp::parsed_args {
  planar::cliapp::parsed_args args;
  args.path                        = {"queue", "run"};
  args.positional_lists["command"] = command;
  args.positionals["command"]      = command.back();
  // Every seam case runs with `--notices`, so the last line on standard error
  // is the outcome the submitter reports on the path under test.
  args.flags["--notices"] = {"true"};
  return args;
}

/// @brief Builds the invocation context of a case: a scratch environment, its
/// own streams.
struct fixture {
  std::ostringstream out;
  std::ostringstream err;
  agent::context     ctx;

  explicit fixture(const scratch& sc, std::string agent_db, std::map<std::string, std::string, std::less<>> extra = {})
      : ctx({"planar-agent", "queue", "run"}, agent::map_env(base_env(sc, std::move(agent_db), std::move(extra))),
            sc.root / "proj", std::make_shared<agent::database>(sc.root / "planar.db", err), out, err) {
  }

private:
  /// @brief The scratch environment, plus `extra` (a case's `PLANAR_QUEUE_SLOT`).
  static auto base_env(const scratch& sc, std::string agent_db, std::map<std::string, std::string, std::less<>> extra)
      -> std::map<std::string, std::string, std::less<>> {
    std::map<std::string, std::string, std::less<>> vars{{"PLANAR_AGENT_DB", std::move(agent_db)},
                                                         {"HOME", (sc.root / "fakehome").string()},
                                                         {"PWD", (sc.root / "proj").string()},
                                                         {"PLANAR_CONFIG_PATH", (sc.root / "config.toml").string()},
                                                         {"PLANAR_DB", (sc.root / "planar.db").string()}};
    for (auto& [name, value] : extra) {
      vars.insert_or_assign(name, std::move(value));
    }
    return vars;
  }
};

/// @brief Runs the handler for `command` with its seams replaced by `deps`.
auto run_queue(const scratch& sc, const std::vector<std::string>& command, agent::handlers::queue_run_deps deps,
               std::map<std::string, std::string, std::less<>> extra_env = {}) -> invocation {
  fixture     fx{sc, (sc.root / "agent.db").string(), std::move(extra_env)};
  auto const  outcome = agent::handlers::queue_run_with(fx.ctx, queue_args(command), std::move(deps));
  auto const* status  = std::get_if<agent::exit_status>(&outcome);
  REQUIRE(status != nullptr);
  return invocation{.code = status->code, .out = fx.out.str(), .err = fx.err.str()};
}

/// @brief The settings every case uses: a short poll, a long staleness
/// window so seeded entries stay fresh, and two slots so a seeded running
/// entry does not hold this submitter back.
auto fast_settings() -> planar::engine::config::queue_settings {
  return planar::engine::config::queue_settings{
      .slots = 2, .poll_interval_ms = 10, .stale_after_ms = 3'600'000, .grace_ms = 10'000, .history_days = 30};
}

/// @brief A sleep seam that bounds a case: it really sleeps a millisecond,
/// counts the polls, and once `limit` polls have gone by it THROWS, which
/// Catch2 reports as a failure of the case. A submitter that waits on a
/// regression (however the regression is spelled) therefore fails the case
/// after a bounded number of polls instead of looping forever.
struct poll_bound {
  static constexpr int limit = 300;
  std::shared_ptr<int> polls = std::make_shared<int>(0);

  /// @brief Installs the seam and a fixed settings loader.
  void bind(agent::handlers::queue_run_deps& deps, planar::engine::config::queue_settings settings) const {
    auto const count = polls;
    deps.sleep       = [count](std::chrono::milliseconds) {
      if (++*count >= limit) {
        throw std::runtime_error("queue run polled past the case's bound: it is waiting on something that should not block it");
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    };
    deps.load_settings = [settings] {
      return std::expected<planar::engine::config::queue_settings, planar::engine::config::queue_load_error>{settings};
    };
  }
};

/// @brief A monotonic clock that stands still until the test moves it. It
/// starts at the system clock's reading and adds only what `advance` gives it,
/// so a case whose outcome depends on a window elapsing (a staleness window, a
/// run limit) decides WHEN by calling `advance`, not by how loaded the machine
/// is. Wall time is the system's; only the monotonic clock is steered.
class steered_clock final : public ident::clock {
public:
  /// @brief Moves the monotonic clock forward.
  void advance(std::int64_t ms) {
    _offset += ms;
  }

  [[nodiscard]] auto monotonic_ms() -> std::expected<std::int64_t, ident::error> override {
    return _base + _offset;
  }
  [[nodiscard]] auto wall_ms() -> std::int64_t override {
    return _system.wall_ms();
  }

private:
  ident::system_clock _system;
  std::int64_t        _base   = _system.monotonic_ms().value_or(0);
  std::int64_t        _offset = 0;
};

/// @brief Seeds the scratch store with a RUNNING entry that is live: its
/// submitter is this test process, so its existence and start time hold, and
/// it was just refreshed. It is the parent a nested run can name.
/// @param steered When set, the clock the entry's refresh time is read from, so a case that steers the handler's clock seeds on
/// the same timeline; the system clock otherwise.
/// @return The entry's sequence number.
auto seed_live_parent(const scratch& sc, ident::clock* steered = nullptr) -> std::int64_t {
  auto opened = planar::db::agent::open_agent_db_at(sc.root / "agent.db");
  REQUIRE(opened.has_value());
  ident::system_clock system;
  ident::clock&       clock   = steered != nullptr ? *steered : static_cast<ident::clock&>(system);
  auto const          pid     = static_cast<std::int64_t>(::getpid());
  auto const          started = ident::process_start_time(pid);
  REQUIRE((started && started->has_value()));
  auto const now = clock.monotonic_ms();
  REQUIRE(now.has_value());
  auto const seq = hq::enqueue(*opened, hq::enqueue_request{.host_id     = ident::host_identity(ident::native_identity_source()),
                                                            .pid         = pid,
                                                            .pid_started = static_cast<std::int64_t>(**started),
                                                            .cwd         = "/",
                                                            .argv        = {"parent"},
                                                            .enqueued_at = clock.wall_ms(),
                                                            .refreshed_mono = *now});
  REQUIRE(seq.has_value());
  REQUIRE(opened
              ->execute(std::format("update queue_entries set state = 'running', started_at = {} where seq = {}", clock.wall_ms(),
                                    *seq))
              .has_value());
  return *seq;
}

/// @brief The failure a store that stays busy past its timeout reports.
auto busy_failure(int code) -> hq::queue_error {
  return hq::queue_error{.kind        = hq::queue_error_kind::query_failed,
                         .sqlite_code = code,
                         .message     = std::format("hostqueue: begin nested enqueue: database is locked (sqlite {})", code)};
}

} // namespace

TEST_CASE("queue run: a SIGTERM that fails on an overdue orphan is reported on stderr and the command still runs",
          "[cmd][agent][queue]") {
  scratch sc;
  {
    // Create and migrate the store, then seed a running entry that is past its
    // deadline, whose submitter is gone and whose group (as the probe below
    // reports) has members.
    auto opened = planar::db::agent::open_agent_db_at(sc.root / "agent.db");
    REQUIRE(opened.has_value());
    ident::system_clock clock;
    auto const          now = clock.monotonic_ms();
    REQUIRE(now.has_value());
    auto const seq = hq::enqueue(*opened, hq::enqueue_request{
                                              .host_id        = ident::host_identity(ident::native_identity_source()),
                                              .pid            = 4242,
                                              .pid_started    = 1,
                                              .cwd            = "/seeded",
                                              .argv           = {"orphan"},
                                              .enqueued_at    = clock.wall_ms(),
                                              .refreshed_mono = *now,
                                          });
    REQUIRE(seq.has_value());
    REQUIRE(*seq == 1);
    REQUIRE(opened
                ->execute(std::format("update queue_entries set state = 'running', started_at = {}, child_pgid = 4242424, "
                                      "child_started = 7, deadline_mono = 1 where seq = 1;",
                                      clock.wall_ms()))
                .has_value());
  }

  auto const real      = hq::system_process_probe();
  auto       probe     = real;
  probe.process_exists = [real](std::int64_t pid) -> std::expected<bool, ident::error> {
    return pid == 4242 ? std::expected<bool, ident::error>{false} : real.process_exists(pid);
  };
  probe.process_start_time = [real](std::int64_t pid) -> std::expected<std::optional<ident::start_time>, ident::error> {
    return (pid == 4242 || pid == 4242424) ? std::expected<std::optional<ident::start_time>, ident::error>{std::nullopt}
                                           : real.process_start_time(pid);
  };
  probe.group_has_members = [real](std::int64_t pgid) -> std::expected<bool, ident::error> {
    return pgid == 4242424 ? std::expected<bool, ident::error>{true} : real.group_has_members(pgid);
  };
  int sigterms = 0;

  agent::handlers::queue_run_deps deps;
  deps.probe = probe;
  poll_bound bound;
  bound.bind(deps, fast_settings());
  deps.signaller = [&sigterms](std::int64_t, int) -> std::expected<void, ident::error> {
    ++sigterms;
    return std::unexpected(ident::error::not_permitted);
  };

  auto const got = run_queue(sc, {"/usr/bin/true"}, deps);
  INFO("stderr:\n" << got.err);
  CHECK(got.code == 0);
  // Bounded: the submitter started on its first polls, not after the bound.
  CHECK(*bound.polls < poll_bound::limit);
  // The engine tried the signal exactly once, and the handler said so: the
  // entry number, the signal, and why.
  CHECK(sigterms == 1);
  CHECK(got.err.contains("SIGTERM"));
  CHECK(got.err.contains("entry 1"));
  CHECK(got.err.contains("not permitted"));
  // Said once, not once per poll, though the command may span several.
  auto count = std::size_t{0};
  for (auto at = got.err.find("SIGTERM"); at != std::string::npos; at = got.err.find("SIGTERM", at + 1)) {
    ++count;
  }
  CHECK(count == 1);
}

TEST_CASE("queue run: a configuration loader that fails before the enqueue refuses at 125 and touches no store",
          "[cmd][agent][queue]") {
  scratch                         sc;
  agent::handlers::queue_run_deps deps;
  deps.load_settings = [] {
    return std::expected<planar::engine::config::queue_settings, planar::engine::config::queue_load_error>{
        std::unexpected(planar::engine::config::queue_load_error{
            .kind_ = planar::engine::config::queue_load_error::kind::unreadable, .message = "config is unreadable"})};
  };
  auto const got = run_queue(sc, {"/usr/bin/true"}, deps);
  CHECK(got.code == 125);
  CHECK(got.out.empty());
  CHECK(got.err == "error: queue: config is unreadable\n");
  CHECK_FALSE(std::filesystem::exists(sc.root / "agent.db"));
}

TEST_CASE("queue run: an unopenable agent database refuses at 125 and does not run the command", "[cmd][agent][queue]") {
  scratch sc;
  // A regular file where the store's parent directory would have to be.
  {
    std::ofstream(sc.root / "blocker") << "not a directory";
  }
  fixture fx{sc, (sc.root / "blocker" / "agent.db").string()};
  // An absolute path: this environment has no PATH, and a program that
  // cannot be resolved is refused (127) before the store is ever opened, which
  // would make this case pass for the wrong reason.
  auto const  outcome = agent::handlers::queue_run(fx.ctx, queue_args({"/usr/bin/touch", (sc.root / "marker").string()}));
  auto const* status  = std::get_if<agent::exit_status>(&outcome);
  REQUIRE(status != nullptr);
  INFO("stderr:\n" << fx.err.str());
  CHECK(status->code == 125);
  CHECK(fx.out.str().empty());
  CHECK(fx.err.str().contains("blocker"));
  CHECK_FALSE(std::filesystem::exists(sc.root / "marker"));
}

TEST_CASE("queue run: giving up after a poll cannot complete ends the entry as abandoned", "[cmd][agent][queue]") {
  scratch sc;
  {
    // A store whose refresh always fails but whose delete and history insert
    // work: the poll's update of entry 1 is refused by a trigger.
    auto opened = planar::db::agent::open_agent_db_at(sc.root / "agent.db");
    REQUIRE(opened.has_value());
    REQUIRE(opened
                ->execute("create trigger refuse_refresh before update on queue_entries "
                          "begin select raise(abort, 'refresh refused'); end;")
                .has_value());
  }
  agent::handlers::queue_run_deps deps;
  // A short staleness window so the give-up is reached in a few polls.
  poll_bound bound;
  bound.bind(deps, planar::engine::config::queue_settings{
                       .slots = 1, .poll_interval_ms = 5, .stale_after_ms = 50, .grace_ms = 10'000, .history_days = 30});

  auto const got = run_queue(sc, {"/usr/bin/true"}, deps);
  INFO("stderr:\n" << got.err);
  CHECK(got.code == 125);
  CHECK(last_line(got.err) == "queue: entry 1 abandoned, could not be polled");
  CHECK(got.err.contains("giving up"));
  CHECK(*bound.polls < poll_bound::limit);

  auto opened = planar::db::agent::open_agent_db_at(sc.root / "agent.db");
  REQUIRE(opened.has_value());
  // The entry was not left behind for a reaper: it ended, once, as abandoned.
  CHECK(hq::list(*opened).value().empty());
  auto const history = hq::list_history(*opened).value();
  REQUIRE(history.size() == 1);
  CHECK(history.front().outcome == hq::history_outcome::abandoned);
}

TEST_CASE("queue run: an entry another process already ended is still mapped to its stop reason, never 128 plus the signal",
          "[cmd][agent][queue][hq-timeouts]") {
  struct reason_case {
    hq::stop_reason     reason;
    hq::history_outcome outcome;
    int                 code;
  };
  for (auto const& one : {reason_case{hq::stop_reason::timeout, hq::history_outcome::timeout, 124},
                          reason_case{hq::stop_reason::cancelled, hq::history_outcome::cancelled, 125}}) {
    INFO("reason " << hq::to_string(one.reason));
    scratch sc;
    bool    acted = false;

    agent::handlers::queue_run_deps deps;
    auto const                      settings = fast_settings();
    deps.load_settings                       = [settings] {
      return std::expected<planar::engine::config::queue_settings, planar::engine::config::queue_load_error>{settings};
    };
    auto polls = std::make_shared<int>(0);
    // At the first tick after the command has started, play "another
    // process": mark the entry with the reason (which SIGTERMs the group), and
    // then, seeing the group empty, end it. The entry is therefore gone, with
    // its history row, before the submitter reaps its child.
    deps.sleep = [&, polls](std::chrono::milliseconds) {
      if (++*polls >= 300) {
        throw std::runtime_error("queue run polled past the case's bound");
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      if (acted) {
        return;
      }
      auto opened = planar::db::agent::open_agent_db_at(sc.root / "agent.db");
      REQUIRE(opened.has_value());
      auto const stored = hq::find(*opened, 1);
      if (!stored || !stored->has_value() || !(*stored)->child_pgid) {
        return;
      }
      acted = true;
      ident::system_clock clock;
      auto const          host = ident::host_identity(ident::native_identity_source());
      hq::canceller const who{.vendor = "claude", .role = "operator", .pid = 1234};
      auto const          real = hq::system_process_probe();
      auto const          begun =
          hq::begin_terminate(*opened,
                              hq::begin_terminate_request{
                                  .seq          = 1,
                                  .reason       = one.reason,
                                  .cancelled_by = one.reason == hq::stop_reason::cancelled ? std::optional{who} : std::nullopt,
                                  .host_id      = host},
                              clock, real, hq::system_group_signaller());
      REQUIRE(begun.has_value());
      REQUIRE(begun->status == hq::begin_status::marked);
      auto empty              = real;
      empty.group_has_members = [](std::int64_t) -> std::expected<bool, ident::error> { return false; };
      auto const ended = hq::advance_terminations(*opened, hq::advance_request{.host_id = host, .grace_ms = 10'000, .seq = 1},
                                                  clock, empty, hq::system_group_signaller());
      REQUIRE(ended.has_value());
      REQUIRE(ended->ended.size() == 1);
    };

    auto const got = run_queue(sc, {"/bin/sleep", "30"}, deps);
    INFO("stderr:\n" << got.err);
    CHECK(acted);
    CHECK(got.code == one.code);

    auto opened = planar::db::agent::open_agent_db_at(sc.root / "agent.db");
    REQUIRE(opened.has_value());
    CHECK(hq::list(*opened).value().empty());
    auto const history = hq::list_history(*opened).value();
    REQUIRE(history.size() == 1);
    CHECK(history.front().outcome == one.outcome);
    CHECK_FALSE(history.front().signal.has_value());
  }
}

namespace {

/// @brief The system clock, counting how often the monotonic clock is read.
class counting_clock final : public ident::clock {
public:
  ident::system_clock inner;
  int                 reads = 0;

  [[nodiscard]] auto monotonic_ms() -> std::expected<std::int64_t, ident::error> override {
    ++reads;
    return inner.monotonic_ms();
  }
  [[nodiscard]] auto wall_ms() -> std::int64_t override {
    return inner.wall_ms();
  }
};

} // namespace

TEST_CASE("queue run: a run limit that cannot mark a missing entry retries at the poll interval, not at every tick",
          "[cmd][agent][queue][hq-timeouts]") {
  scratch sc;
  auto    clock = std::make_shared<counting_clock>();
  bool    acted = false;
  int     ticks = 0;

  agent::handlers::queue_run_deps deps;
  deps.clock = clock;
  // A poll interval far longer than the case: the regular poll never runs, so
  // only the run-limit marking reads the store.
  auto const settings = planar::engine::config::queue_settings{
      .slots = 1, .poll_interval_ms = 60'000, .stale_after_ms = 3'600'000, .grace_ms = 10'000, .history_days = 30};
  deps.load_settings = [settings] {
    return std::expected<planar::engine::config::queue_settings, planar::engine::config::queue_load_error>{settings};
  };
  deps.sleep = [&](std::chrono::milliseconds) {
    if (++ticks >= 2000) {
      throw std::runtime_error("queue run ticked past the case's bound");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (acted) {
      return;
    }
    auto opened = planar::db::agent::open_agent_db_at(sc.root / "agent.db");
    REQUIRE(opened.has_value());
    auto const stored = hq::find(*opened, 1);
    if (!stored || !stored->has_value() || !(*stored)->child_pgid) {
      return;
    }
    // The entry disappears from under the running submitter; its run limit
    // (300 ms) passes while the command still runs, so every tick would try to mark it and find it missing.
    acted = true;
    REQUIRE(opened->execute("delete from queue_entries where seq = 1;").has_value());
  };

  fixture fx{sc, (sc.root / "agent.db").string()};
  auto    args            = queue_args({"/bin/sleep", "1"});
  args.flags["--timeout"] = {"300ms"};
  auto const  outcome     = agent::handlers::queue_run_with(fx.ctx, args, deps);
  auto const* status      = std::get_if<agent::exit_status>(&outcome);
  REQUIRE(status != nullptr);
  INFO("stderr:\n" << fx.err.str());
  CHECK(acted);
  CHECK(status->code == 124); // task 7080: with the entry gone the submitter stops the command itself at the limit
  REQUIRE(ticks > 50);
  // Two clock reads per tick are the loop's own (the run-limit check and the
  // poll schedule). A mark attempted at every tick reads a third, inside
  // `begin_terminate`.
  CHECK(clock->reads <= 2 * ticks + 20);
}

TEST_CASE("queue run: a signal that arrives while the turn is being taken runs nothing", "[cmd][agent][queue][hq-signals]") {
  // The signal is raised from inside the poll that grants the turn (the
  // settings reload at the start of that poll), after the wait loop's own
  // check and before the command starts. The handler is the submitter's own,
  // so `raise` reaches it synchronously.
  scratch                         sc;
  auto const                      marker   = sc.root / "ran";
  int                             reloads  = 0;
  auto                            settings = fast_settings();
  agent::handlers::queue_run_deps deps;
  deps.load_settings = [&] {
    if (++reloads == 2) {
      ::raise(SIGTERM);
    }
    return std::expected<planar::engine::config::queue_settings, planar::engine::config::queue_load_error>{settings};
  };
  auto const got = run_queue(sc, {"/usr/bin/touch", marker.string()}, deps);
  INFO("stderr:\n" << got.err);
  CHECK(reloads == 2);
  CHECK(got.code == 125);
  CHECK(got.err.contains("not run"));
  CHECK_FALSE(std::filesystem::exists(marker));

  auto opened = planar::db::agent::open_agent_db_at(sc.root / "agent.db");
  REQUIRE(opened.has_value());
  auto const entries = hq::list(*opened);
  REQUIRE(entries.has_value());
  CHECK(entries->empty());
  auto const row = hq::find_history(*opened, 1);
  REQUIRE(row.has_value());
  REQUIRE(row->has_value());
  CHECK((*row)->outcome == hq::history_outcome::cancelled);
  // The poll had already marked the entry running; the command still did not start.
}

// ---------------------------------------------------------------------------
// Task 7052: a nested insert that finds the store busy
// ---------------------------------------------------------------------------

TEST_CASE("queue run: a nested insert that finds the store busy is retried and then succeeds",
          "[cmd][agent][queue][hq-nested-run]") {
  scratch    sc;
  auto const parent = seed_live_parent(sc);
  auto const ran    = sc.root / "ran";

  agent::handlers::queue_run_deps deps;
  poll_bound                      bound;
  bound.bind(deps, fast_settings());
  auto const calls = std::make_shared<int>(0);
  // Plain SQLITE_BUSY, then an EXTENDED busy code (its low byte is 5), then
  // the real insert.
  deps.enqueue_nested = [calls](planar::db::connection& conn, std::int64_t seq, const hq::enqueue_request& request,
                                const hq::nested_limits& limits, ident::clock& clock,
                                const hq::process_probe& probe) -> std::expected<hq::nested_result, hq::queue_error> {
    ++*calls;
    if (*calls == 1) {
      return std::unexpected(busy_failure(5));
    }
    if (*calls == 2) {
      return std::unexpected(busy_failure(261));
    }
    return hq::enqueue_nested(conn, seq, request, limits, clock, probe);
  };

  auto const got = run_queue(sc, {"/bin/sh", "-c", std::format("echo x > '{}'", ran.string())}, deps,
                             {{"PLANAR_QUEUE_SLOT", std::to_string(parent)}});
  INFO("stderr:\n" << got.err);
  CHECK(got.code == 0);
  CHECK(*calls == 3);
  CHECK(std::filesystem::exists(ran));

  auto opened = planar::db::agent::open_agent_db_at(sc.root / "agent.db");
  REQUIRE(opened.has_value());
  // Only the seeded parent is left; the command ran as its nested entry.
  auto const entries = hq::list(*opened).value();
  REQUIRE(entries.size() == 1);
  CHECK(entries.front().seq == parent);
  auto const history = hq::list_history(*opened).value();
  REQUIRE(history.size() == 1);
  CHECK(history.front().nested);
  CHECK(history.front().parent_seq == parent);
  CHECK(history.front().outcome == hq::history_outcome::exited);
}

TEST_CASE("queue run: a nested insert that keeps finding the store busy refuses at 125 and runs nothing",
          "[cmd][agent][queue][hq-nested-run]") {
  scratch    sc;
  auto const parent = seed_live_parent(sc);
  auto const ran    = sc.root / "ran";

  agent::handlers::queue_run_deps deps;
  poll_bound                      bound;
  // A short staleness window, so the bound is reached in a few polls.
  bound.bind(deps, planar::engine::config::queue_settings{
                       .slots = 1, .poll_interval_ms = 5, .stale_after_ms = 50, .grace_ms = 10'000, .history_days = 30});
  auto const calls    = std::make_shared<int>(0);
  deps.enqueue_nested = [calls](planar::db::connection&, std::int64_t, const hq::enqueue_request&, const hq::nested_limits&,
                                ident::clock&, const hq::process_probe&) -> std::expected<hq::nested_result, hq::queue_error> {
    ++*calls;
    return std::unexpected(busy_failure(5));
  };

  auto const got = run_queue(sc, {"/bin/sh", "-c", std::format("echo x > '{}'", ran.string())}, deps,
                             {{"PLANAR_QUEUE_SLOT", std::to_string(parent)}});
  INFO("stderr:\n" << got.err);
  CHECK(got.code == 125);
  CHECK(got.err.contains("busy"));
  CHECK(got.err.contains("the command was not run"));
  CHECK(*bound.polls < poll_bound::limit);
  CHECK(*calls > 1); // it retried, rather than giving up on the first busy
  CHECK_FALSE(std::filesystem::exists(ran));

  // It neither queued normally nor left anything behind: the store holds the
  // seeded parent and no history.
  auto opened = planar::db::agent::open_agent_db_at(sc.root / "agent.db");
  REQUIRE(opened.has_value());
  auto const entries = hq::list(*opened).value();
  REQUIRE(entries.size() == 1);
  CHECK(entries.front().seq == parent);
  CHECK(hq::list_history(*opened).value().empty());
}

TEST_CASE("queue run: a nested insert that fails for any other reason refuses at 125 at once",
          "[cmd][agent][queue][hq-nested-run]") {
  scratch    sc;
  auto const parent = seed_live_parent(sc);

  agent::handlers::queue_run_deps deps;
  poll_bound                      bound;
  bound.bind(deps, fast_settings());
  auto const calls    = std::make_shared<int>(0);
  deps.enqueue_nested = [calls](planar::db::connection&, std::int64_t, const hq::enqueue_request&, const hq::nested_limits&,
                                ident::clock&, const hq::process_probe&) -> std::expected<hq::nested_result, hq::queue_error> {
    ++*calls;
    return std::unexpected(hq::queue_error{
        .kind = hq::queue_error_kind::query_failed, .sqlite_code = 19, .message = "hostqueue: insert refused (sqlite 19)"});
  };

  auto const got = run_queue(sc, {"/usr/bin/true"}, deps, {{"PLANAR_QUEUE_SLOT", std::to_string(parent)}});
  INFO("stderr:\n" << got.err);
  CHECK(got.code == 125);
  CHECK(got.err.contains("insert refused"));
  CHECK(*calls == 1);
  auto opened = planar::db::agent::open_agent_db_at(sc.root / "agent.db");
  REQUIRE(opened.has_value());
  CHECK(hq::list(*opened).value().size() == 1);
  CHECK(hq::list_history(*opened).value().empty());
}

// ---------------------------------------------------------------------------
// Task 7017 (hq-missing-entry): a submitter that finds its own entry missing
// reads the history row (tech spec 647 § Waiting and claiming a turn, table
// under the poll steps). Task 7065 (hq-giveup-history-fields): the history row
// a submitter's give-up leaves.
// ---------------------------------------------------------------------------

namespace {

/// @brief The settings the missing-entry cases use: ONE slot, so a seeded
/// running holder keeps the submitter waiting, a short poll, and a staleness
/// window so long that no seeded entry is ever reaped by the engine on its own.
auto one_slot_settings() -> planar::engine::config::queue_settings {
  return planar::engine::config::queue_settings{
      .slots = 1, .poll_interval_ms = 10, .stale_after_ms = 3'600'000, .grace_ms = 10'000, .history_days = 30};
}

/// @brief Installs a sleep seam that calls `act` with a fresh connection to the
/// scratch store at every tick, and throws (failing the case) after `limit`
/// ticks, so a submitter that waits forever fails instead of hanging.
void script(agent::handlers::queue_run_deps& deps, const scratch& sc, planar::engine::config::queue_settings settings,
            std::function<void(planar::db::connection&)> act, int limit = 600) {
  auto const ticks   = std::make_shared<int>(0);
  auto const root    = sc.root;
  deps.load_settings = [settings] {
    return std::expected<planar::engine::config::queue_settings, planar::engine::config::queue_load_error>{settings};
  };
  deps.sleep = [ticks, root, act = std::move(act), limit](std::chrono::milliseconds) {
    if (++*ticks >= limit) {
      throw std::runtime_error("queue run ticked past the case's bound");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    auto opened = planar::db::agent::open_agent_db_at(root / "agent.db");
    REQUIRE(opened.has_value());
    act(*opened);
  };
}

/// @brief Ends `seq` the way `outcome` says another process would, and
/// requires that this call was the one that removed it.
void end_as(planar::db::connection& conn, std::int64_t seq, hq::history_outcome outcome) {
  ident::system_clock clock;
  hq::end_request     request{.outcome = outcome, .ended_at = clock.wall_ms()};
  if (outcome == hq::history_outcome::exited) {
    request.exit_code = 0;
  } else if (outcome == hq::history_outcome::signaled) {
    request.signal = 9;
  } else if (outcome == hq::history_outcome::cancelled) {
    request.cancelled_by = hq::canceller{.vendor = "claude", .role = "operator", .pid = 4321};
  }
  auto const ended = hq::end_entry(conn, seq, request);
  REQUIRE(ended.has_value());
  REQUIRE(*ended == hq::end_result::ended);
}

/// @brief A live waiting entry submitted by this test process: a later
/// arrival the submitter under test has to queue behind.
auto arrive(planar::db::connection& conn) -> std::int64_t {
  ident::system_clock clock;
  auto const          pid     = static_cast<std::int64_t>(::getpid());
  auto const          started = ident::process_start_time(pid);
  REQUIRE((started && started->has_value()));
  auto const now = clock.monotonic_ms();
  REQUIRE(now.has_value());
  auto const seq = hq::enqueue(conn, hq::enqueue_request{.host_id        = ident::host_identity(ident::native_identity_source()),
                                                         .pid            = pid,
                                                         .pid_started    = static_cast<std::int64_t>(**started),
                                                         .cwd            = "/",
                                                         .argv           = {"arrival"},
                                                         .enqueued_at    = clock.wall_ms(),
                                                         .refreshed_mono = *now});
  REQUIRE(seq.has_value());
  return *seq;
}

auto history_of(planar::db::connection& conn, std::int64_t seq) -> std::optional<hq::history_row> {
  auto const row = hq::find_history(conn, seq);
  REQUIRE(row.has_value());
  return *row;
}

auto marker_lines(const std::filesystem::path& path) -> int {
  std::ifstream in(path);
  int           lines = 0;
  for (std::string line; std::getline(in, line);) {
    ++lines;
  }
  return lines;
}

auto touch_command(const std::filesystem::path& marker) -> std::vector<std::string> {
  return {"/bin/sh", "-c", std::format("echo ran >> '{}'", marker.string())};
}

} // namespace

TEST_CASE("queue run: a waiting submitter reaped as abandoned rejoins behind later arrivals, records its successor and runs once",
          "[cmd][agent][queue][hq-missing-entry]") {
  scratch    sc;
  auto const holder = seed_live_parent(sc); // seq 1, running, holds the only slot
  auto const marker = sc.root / "ran";

  int                         stage   = 0;
  int                         watch   = 0;
  std::int64_t                arrival = 0;
  std::optional<std::int64_t> original_wait_deadline;
  bool                        deadline_carried = false;
  bool                        stayed_behind    = true;

  agent::handlers::queue_run_deps deps;
  script(deps, sc, one_slot_settings(), [&](planar::db::connection& conn) {
    if (stage == 0) {
      auto const mine = hq::find(conn, 2).value();
      if (!mine || mine->state != hq::entry_state::waiting) {
        return;
      }
      original_wait_deadline = mine->wait_deadline_mono;
      end_as(conn, 2, hq::history_outcome::abandoned); // a reaper removes it
      arrival = arrive(conn);                          // and someone arrives meanwhile
      stage   = 1;
    } else if (stage == 1) {
      auto const rejoined = hq::find(conn, 4).value();
      if (!rejoined) {
        return;
      }
      deadline_carried = rejoined->wait_deadline_mono == original_wait_deadline;
      stage            = 2;
    } else if (stage == 2) {
      // Slot free, but the arrival is ahead: the rejoined entry must not start.
      if (++watch == 1) {
        end_as(conn, holder, hq::history_outcome::exited);
      }
      auto const rejoined = hq::find(conn, 4).value();
      stayed_behind       = stayed_behind && rejoined && rejoined->state == hq::entry_state::waiting;
      if (watch >= 8) {
        end_as(conn, arrival, hq::history_outcome::exited);
        stage = 3;
      }
    }
  });

  fixture fx{sc, (sc.root / "agent.db").string()};
  auto    args                 = queue_args(touch_command(marker));
  args.flags["--wait-timeout"] = {"1h"};
  auto const  outcome          = agent::handlers::queue_run_with(fx.ctx, args, deps);
  auto const* status           = std::get_if<agent::exit_status>(&outcome);
  REQUIRE(status != nullptr);
  INFO("stderr:\n" << fx.err.str());
  CHECK(status->code == 0);
  CHECK(marker_lines(marker) == 1);
  CHECK(stayed_behind);
  CHECK(deadline_carried);

  auto opened = planar::db::agent::open_agent_db_at(sc.root / "agent.db");
  REQUIRE(opened.has_value());
  CHECK(hq::list(*opened).value().empty());
  auto const old_row = history_of(*opened, 2);
  REQUIRE(old_row.has_value());
  CHECK(old_row->outcome == hq::history_outcome::abandoned);
  CHECK(old_row->successor_seq == 4);
  auto const new_row = history_of(*opened, 4);
  REQUIRE(new_row.has_value());
  CHECK(new_row->outcome == hq::history_outcome::exited);
  CHECK(new_row->exit_code == 0);
  CHECK_FALSE(new_row->successor_seq.has_value());
  auto const ahead = history_of(*opened, 3);
  REQUIRE(ahead.has_value());
  REQUIRE(new_row->started_at.has_value());
  CHECK(*new_row->started_at >= ahead->ended_at); // arrival order: it ran after the arrival ended
  CHECK(hq::list_history(*opened).value().size() == 4);
}

TEST_CASE("queue run: a waiting submitter whose entry was cancelled exits 125, runs nothing and does not rejoin",
          "[cmd][agent][queue][hq-missing-entry]") {
  scratch    sc;
  auto const holder = seed_live_parent(sc);
  auto const marker = sc.root / "ran";

  agent::handlers::queue_run_deps deps;
  bool                            done = false;
  script(deps, sc, one_slot_settings(), [&](planar::db::connection& conn) {
    if (!done && hq::find(conn, 2).value()) {
      end_as(conn, 2, hq::history_outcome::cancelled);
      done = true;
    }
  });
  auto const got = run_queue(sc, touch_command(marker), deps);
  INFO("stderr:\n" << got.err);
  CHECK(got.code == 125);
  CHECK(last_line(got.err) == "queue: entry 2 cancelled");
  CHECK(got.err.contains("cancelled"));
  CHECK(got.err.contains("the command was not run"));
  CHECK_FALSE(std::filesystem::exists(marker));

  auto opened = planar::db::agent::open_agent_db_at(sc.root / "agent.db");
  REQUIRE(opened.has_value());
  auto const entries = hq::list(*opened).value();
  REQUIRE(entries.size() == 1); // only the holder: nothing with a higher number appeared
  CHECK(entries.front().seq == holder);
  auto const row = history_of(*opened, 2);
  REQUIRE(row.has_value());
  CHECK(row->outcome == hq::history_outcome::cancelled);
  CHECK_FALSE(row->successor_seq.has_value());
  CHECK(hq::list_history(*opened).value().size() == 1);
}

TEST_CASE("queue run: a waiting submitter whose entry ended any other way exits 125 and does not rejoin",
          "[cmd][agent][queue][hq-missing-entry]") {
  for (auto const outcome : {hq::history_outcome::exited, hq::history_outcome::signaled, hq::history_outcome::timeout,
                             hq::history_outcome::wait_timeout}) {
    INFO("outcome " << hq::to_string(outcome));
    scratch    sc;
    auto const holder = seed_live_parent(sc);
    auto const marker = sc.root / "ran";

    agent::handlers::queue_run_deps deps;
    bool                            done = false;
    script(deps, sc, one_slot_settings(), [&](planar::db::connection& conn) {
      if (!done && hq::find(conn, 2).value()) {
        end_as(conn, 2, outcome);
        done = true;
      }
    });
    auto const got = run_queue(sc, touch_command(marker), deps);
    INFO("stderr:\n" << got.err);
    CHECK(got.code == 125);
    CHECK(last_line(got.err) == std::format("queue: entry 2 ended as {} without this submitter", hq::to_string(outcome)));
    CHECK(got.err.contains("the command was not run"));
    CHECK_FALSE(std::filesystem::exists(marker));

    auto opened = planar::db::agent::open_agent_db_at(sc.root / "agent.db");
    REQUIRE(opened.has_value());
    auto const entries = hq::list(*opened).value();
    REQUIRE(entries.size() == 1);
    CHECK(entries.front().seq == holder);
    CHECK(hq::list_history(*opened).value().size() == 1);
  }
}

TEST_CASE("queue run: a waiting submitter whose entry is missing and left no history row exits 125 and runs nothing",
          "[cmd][agent][queue][hq-missing-entry]") {
  scratch    sc;
  auto const holder = seed_live_parent(sc);
  auto const marker = sc.root / "ran";

  agent::handlers::queue_run_deps deps;
  bool                            done = false;
  script(deps, sc, one_slot_settings(), [&](planar::db::connection& conn) {
    if (!done && hq::find(conn, 2).value()) {
      REQUIRE(conn.execute("delete from queue_entries where seq = 2;").has_value()); // no history row written
      done = true;
    }
  });
  auto const got = run_queue(sc, touch_command(marker), deps);
  INFO("stderr:\n" << got.err);
  CHECK(got.code == 125);
  CHECK(last_line(got.err) == "queue: entry 2 ended without a history row");
  CHECK(got.err.contains("no history"));
  CHECK(got.err.contains("the command was not run"));
  CHECK_FALSE(std::filesystem::exists(marker));

  auto opened = planar::db::agent::open_agent_db_at(sc.root / "agent.db");
  REQUIRE(opened.has_value());
  auto const entries = hq::list(*opened).value();
  REQUIRE(entries.size() == 1);
  CHECK(entries.front().seq == holder);
  CHECK(hq::list_history(*opened).value().empty());
}

TEST_CASE("queue run: a submitter reaped again and again stops rejoining after three rejoins",
          "[cmd][agent][queue][hq-missing-entry]") {
  // The bound is this task's decision (queue.cppm, "Missing entry"): the spec
  // says a reaped waiter rejoins and names no limit, and a submitter reaped
  // every time it rejoined would otherwise queue forever.
  constexpr int k_rejoins = 3;

  scratch    sc;
  auto const holder = seed_live_parent(sc);
  auto const marker = sc.root / "ran";

  agent::handlers::queue_run_deps deps;
  int                             reaped = 0;
  script(deps, sc, one_slot_settings(), [&](planar::db::connection& conn) {
    for (auto const& e : hq::list(conn).value()) {
      if (e.seq != holder && e.state == hq::entry_state::waiting) {
        end_as(conn, e.seq, hq::history_outcome::abandoned);
        ++reaped;
      }
    }
  });
  auto const got = run_queue(sc, touch_command(marker), deps);
  INFO("stderr:\n" << got.err);
  CHECK(got.code == 125);
  CHECK(last_line(got.err) == "queue: entry 5 abandoned, rejoin limit reached");
  CHECK(got.err.contains("the command was not run"));
  CHECK_FALSE(std::filesystem::exists(marker));
  // The original entry and each of the three rejoins was reaped once; nothing
  // was inserted after the last.
  CHECK(reaped == k_rejoins + 1);

  auto opened = planar::db::agent::open_agent_db_at(sc.root / "agent.db");
  REQUIRE(opened.has_value());
  auto const entries = hq::list(*opened).value();
  REQUIRE(entries.size() == 1);
  CHECK(entries.front().seq == holder);
  auto const history = hq::list_history(*opened).value();
  REQUIRE(history.size() == static_cast<std::size_t>(k_rejoins + 1));
  // The successors chain 2 -> 3 -> 4 -> 5, and the last names none.
  for (std::int64_t seq = 2; seq <= 4; ++seq) {
    auto const row = history_of(*opened, seq);
    REQUIRE(row.has_value());
    CHECK(row->outcome == hq::history_outcome::abandoned);
    CHECK(row->successor_seq == seq + 1);
  }
  auto const last = history_of(*opened, 5);
  REQUIRE(last.has_value());
  CHECK(last->outcome == hq::history_outcome::abandoned);
  CHECK_FALSE(last->successor_seq.has_value());
}

TEST_CASE("queue run: a running submitter whose entry was reaped keeps supervising, runs once and never rejoins",
          "[cmd][agent][queue][hq-missing-entry]") {
  scratch    sc;
  auto const marker = sc.root / "ran";

  agent::handlers::queue_run_deps deps;
  bool                            done = false;
  script(deps, sc, one_slot_settings(), [&](planar::db::connection& conn) {
    auto const mine = hq::find(conn, 1).value();
    if (!done && mine && mine->child_pgid) {
      end_as(conn, 1, hq::history_outcome::abandoned); // reaped while its command runs
      done = true;
    }
  });
  auto const got = run_queue(sc, {"/bin/sh", "-c", std::format("sleep 0.3; echo ran >> '{}'; exit 7", marker.string())}, deps);
  INFO("stderr:\n" << got.err);
  CHECK(done);
  CHECK(got.code == 7); // what it observed of its child
  CHECK(marker_lines(marker) == 1);
  // Said once, not once per poll.
  auto const notice = std::string_view{"no longer in the queue"};
  auto       count  = std::size_t{0};
  for (auto at = got.err.find(notice); at != std::string::npos; at = got.err.find(notice, at + 1)) {
    ++count;
  }
  CHECK(count == 1);

  auto opened = planar::db::agent::open_agent_db_at(sc.root / "agent.db");
  REQUIRE(opened.has_value());
  CHECK(hq::list(*opened).value().empty()); // no new entry
  auto const history = hq::list_history(*opened).value();
  REQUIRE(history.size() == 1);
  CHECK(history.front().seq == 1);
  CHECK(history.front().outcome == hq::history_outcome::abandoned);
  CHECK_FALSE(history.front().successor_seq.has_value());
}

TEST_CASE("queue run: a running submitter whose entry is missing still stops its command at the run limit and says so",
          "[cmd][agent][queue][hq-missing-entry][hq-vanished-entry-run-limit]") {
  // Task 7080 (superseding the 7017 reading that the limit went with the
  // entry): the submitter keeps supervising a command whose entry is gone
  // (tech spec 647 § Waiting and claiming a turn), and supervising includes the
  // limit it was given. The limit (100 ms) is read on a steered clock that
  // stands still until the entry has been removed and is then moved past the
  // limit, so the order "entry gone, THEN the limit passes" is fixed by the case.
  scratch    sc;
  auto const marker = sc.root / "ran";
  auto const clock  = std::make_shared<steered_clock>();

  agent::handlers::queue_run_deps deps;
  bool                            done = false;
  deps.clock                           = clock;
  script(deps, sc, one_slot_settings(), [&](planar::db::connection& conn) {
    if (!done && hq::find(conn, 1).value() && hq::find(conn, 1).value()->child_pgid) {
      end_as(conn, 1, hq::history_outcome::abandoned);
      done = true;
      clock->advance(1'000);
    }
  });
  fixture fx{sc, (sc.root / "agent.db").string()};
  auto    args            = queue_args({"/bin/sh", "-c", std::format("sleep 0.5; echo ran >> '{}'", marker.string())});
  args.flags["--timeout"] = {"100ms"};
  auto const  outcome     = agent::handlers::queue_run_with(fx.ctx, args, deps);
  auto const* status      = std::get_if<agent::exit_status>(&outcome);
  REQUIRE(status != nullptr);
  INFO("stderr:\n" << fx.err.str());
  CHECK(done);
  CHECK(status->code == 124);
  CHECK(marker_lines(marker) == 0); // the command was stopped, not left to finish
  CHECK(fx.err.str().contains("run limit"));
  CHECK(last_line(fx.err.str()) == "queue: entry 1 stopped at its run limit");

  // The other process that removed the entry wrote its row; the submitter wrote none.
  auto opened = planar::db::agent::open_agent_db_at(sc.root / "agent.db");
  REQUIRE(opened.has_value());
  auto const history = hq::list_history(*opened).value();
  REQUIRE(history.size() == 1);
  CHECK(history.front().outcome == hq::history_outcome::abandoned);
}

TEST_CASE("queue run: a give-up exit leaves an abandoned row with the fields of an entry that never started",
          "[cmd][agent][queue][hq-giveup-history-fields]") {
  // Task 7065's decision: the submitter that cannot poll for longer than the
  // staleness window ends its own waiting entry as `abandoned`, the outcome the
  // spec gives an entry that is no longer live. The row is that of an entry
  // that never ran: no exit code, no signal, no start, no run time, no
  // successor (it does not rejoin) and no canceller; it keeps the command as
  // submitted and the time it waited.
  scratch sc;
  {
    auto opened = planar::db::agent::open_agent_db_at(sc.root / "agent.db");
    REQUIRE(opened.has_value());
    REQUIRE(opened
                ->execute("create trigger refuse_refresh before update on queue_entries "
                          "begin select raise(abort, 'refresh refused'); end;")
                .has_value());
  }
  agent::handlers::queue_run_deps deps;
  poll_bound                      bound;
  bound.bind(deps, planar::engine::config::queue_settings{
                       .slots = 1, .poll_interval_ms = 5, .stale_after_ms = 50, .grace_ms = 10'000, .history_days = 30});
  fixture fx{sc, (sc.root / "agent.db").string()};
  auto    args          = queue_args({"/usr/bin/true"});
  args.flags["--label"] = {"give-up probe"};
  auto const  outcome   = agent::handlers::queue_run_with(fx.ctx, args, deps);
  auto const* status    = std::get_if<agent::exit_status>(&outcome);
  REQUIRE(status != nullptr);
  CHECK(status->code == 125);
  CHECK(last_line(fx.err.str()) == "queue: entry 1 abandoned, could not be polled");

  auto opened = planar::db::agent::open_agent_db_at(sc.root / "agent.db");
  REQUIRE(opened.has_value());
  auto const row = history_of(*opened, 1);
  REQUIRE(row.has_value());
  CHECK(row->outcome == hq::history_outcome::abandoned);
  CHECK_FALSE(row->exit_code.has_value());
  CHECK_FALSE(row->signal.has_value());
  CHECK_FALSE(row->started_at.has_value());
  CHECK_FALSE(row->ran_ms.has_value());
  CHECK_FALSE(row->successor_seq.has_value());
  CHECK_FALSE(row->cancelled_by.has_value());
  CHECK_FALSE(row->nested);
  CHECK(row->argv == std::vector<std::string>{"/usr/bin/true"});
  CHECK(row->label == "give-up probe");
  CHECK(row->waited_ms >= 0);
  CHECK(row->ended_at >= row->enqueued_at);
  CHECK(row->waited_ms == row->ended_at - row->enqueued_at); // never started: waited until the end
}

// ---------------------------------------------------------------------------
// Task 7017, iteration 2: a store failure while rejoining is bounded like the
// nested insert's (tech spec 647 § Waiting: a submitter that cannot complete a
// poll for longer than the staleness window exits 125).
// ---------------------------------------------------------------------------

namespace {

/// @brief Ends entry 2 as abandoned once it exists, as a reaper would.
auto reap_second(bool& done) -> std::function<void(planar::db::connection&)> {
  return [&done](planar::db::connection& conn) {
    if (!done && hq::find(conn, 2).value()) {
      end_as(conn, 2, hq::history_outcome::abandoned);
      done = true;
    }
  };
}

} // namespace

TEST_CASE("queue run: a rejoin that keeps finding the store busy exits 125 within the staleness window and runs nothing",
          "[cmd][agent][queue][hq-missing-entry]") {
  // The staleness window is measured on a steered clock: it moves 20 ms at each
  // sleep and not at all otherwise, so the window (50 ms) closes after a fixed
  // number of polls whatever the machine is doing. On the system clock the
  // seeded holder could go stale, and free the slot, before this case had
  // reaped entry 2 (measured under load: the submitter then ran its command).
  scratch    sc;
  auto const clock  = std::make_shared<steered_clock>();
  auto const holder = seed_live_parent(sc, clock.get());
  auto const marker = sc.root / "ran";

  agent::handlers::queue_run_deps deps;
  bool                            done = false;
  deps.clock                           = clock;
  script(deps, sc,
         planar::engine::config::queue_settings{
             .slots = 1, .poll_interval_ms = 5, .stale_after_ms = 50, .grace_ms = 10'000, .history_days = 30},
         [&done, clock, reap = reap_second(done)](planar::db::connection& conn) {
           reap(conn);
           clock->advance(20);
         });
  auto const calls = std::make_shared<int>(0);
  deps.rejoin      = [calls](planar::db::connection&, std::int64_t,
                             const hq::enqueue_request&) -> std::expected<hq::rejoin_result, hq::queue_error> {
    ++*calls;
    return std::unexpected(busy_failure(5));
  };
  auto const got = run_queue(sc, touch_command(marker), deps);
  INFO("stderr:\n" << got.err);
  CHECK(got.code == 125);
  CHECK(last_line(got.err) == "queue: entry 2 ended: store busy while rejoining");
  CHECK(got.err.contains("busy"));
  CHECK(got.err.contains("the command was not run"));
  CHECK(*calls > 1); // retried rather than given up on at the first busy
  CHECK_FALSE(std::filesystem::exists(marker));

  auto opened = planar::db::agent::open_agent_db_at(sc.root / "agent.db");
  REQUIRE(opened.has_value());
  // The short window let the seeded holder go stale too; what matters is that
  // nothing was inserted after entry 2.
  for (auto const& e : hq::list(*opened).value()) {
    CHECK(e.seq <= holder);
  }
  auto const row = history_of(*opened, 2);
  REQUIRE(row.has_value());
  CHECK(row->outcome == hq::history_outcome::abandoned);
  CHECK_FALSE(row->successor_seq.has_value());
}

TEST_CASE("queue run: a rejoin that fails for any other reason exits 125 at once and runs nothing",
          "[cmd][agent][queue][hq-missing-entry]") {
  scratch    sc;
  auto const holder = seed_live_parent(sc);
  auto const marker = sc.root / "ran";

  agent::handlers::queue_run_deps deps;
  bool                            done = false;
  script(deps, sc, one_slot_settings(), [&](planar::db::connection& conn) {
    if (!done && hq::find(conn, 2).value()) {
      end_as(conn, 2, hq::history_outcome::abandoned);
      // From here every insert into the queue is refused, with a constraint
      // error rather than a busy one.
      REQUIRE(
          conn.execute(
                  "create trigger refuse_insert before insert on queue_entries begin select raise(abort, 'insert refused'); end;")
              .has_value());
      done = true;
    }
  });
  auto const got = run_queue(sc, touch_command(marker), deps);
  INFO("stderr:\n" << got.err);
  CHECK(got.code == 125);
  CHECK(last_line(got.err) == "queue: entry 2 ended: cannot rejoin the queue");
  CHECK(got.err.contains("insert refused"));
  CHECK(got.err.contains("the command was not run"));
  CHECK_FALSE(got.err.contains("busy"));
  CHECK_FALSE(std::filesystem::exists(marker));

  auto opened = planar::db::agent::open_agent_db_at(sc.root / "agent.db");
  REQUIRE(opened.has_value());
  auto const entries = hq::list(*opened).value();
  REQUIRE(entries.size() == 1);
  CHECK(entries.front().seq == holder);
  CHECK_FALSE(history_of(*opened, 2)->successor_seq.has_value());
}

TEST_CASE("queue run: a rejoin that finds the store busy and then succeeds rejoins normally",
          "[cmd][agent][queue][hq-missing-entry]") {
  scratch    sc;
  auto const holder = seed_live_parent(sc);
  auto const marker = sc.root / "ran";

  agent::handlers::queue_run_deps deps;
  bool                            reaped   = false;
  bool                            released = false;
  auto const                      reap     = reap_second(reaped);
  script(deps, sc, one_slot_settings(), [&](planar::db::connection& conn) {
    reap(conn);
    if (reaped && !released && hq::find(conn, 3).value()) {
      released = true;
      end_as(conn, holder, hq::history_outcome::exited);
    }
  });
  auto const calls = std::make_shared<int>(0);
  deps.rejoin      = [calls](planar::db::connection& conn, std::int64_t seq,
                             const hq::enqueue_request& request) -> std::expected<hq::rejoin_result, hq::queue_error> {
    if (++*calls <= 2) {
      return std::unexpected(busy_failure(*calls == 1 ? 5 : 261));
    }
    return hq::rejoin(conn, seq, request);
  };
  auto const got = run_queue(sc, touch_command(marker), deps);
  INFO("stderr:\n" << got.err);
  CHECK(got.code == 0);
  CHECK(*calls == 3);
  CHECK(marker_lines(marker) == 1);

  auto opened = planar::db::agent::open_agent_db_at(sc.root / "agent.db");
  REQUIRE(opened.has_value());
  CHECK(hq::list(*opened).value().empty());
  CHECK(history_of(*opened, 2)->successor_seq == 3);
  CHECK(history_of(*opened, 3)->outcome == hq::history_outcome::exited);
}

TEST_CASE("queue run: a start failure that is neither 126 nor 127 ends the entry abandoned and the notice says so",
          "[cmd][agent][queue][hq-notices]") {
  // The directory the command was submitted from disappears while the entry
  // waits, so the command passes every check made before the enqueue and then
  // cannot be started at its turn for a reason other than a missing or
  // unexecutable program. `not_started` is the outcome of 126 and 127 only.
  scratch    sc;
  auto const holder = seed_live_parent(sc);
  auto const marker = sc.root / "ran";

  agent::handlers::queue_run_deps deps;
  bool                            done = false;
  script(deps, sc, one_slot_settings(), [&](planar::db::connection& conn) {
    if (!done && hq::find(conn, 2).value()) {
      end_as(conn, holder, hq::history_outcome::exited);
      std::filesystem::remove_all(sc.root / "proj");
      done = true;
    }
  });
  auto const got = run_queue(sc, touch_command(marker), deps);
  INFO("stderr:\n" << got.err);
  CHECK(got.code == 125);
  CHECK(last_line(got.err) == "queue: entry 2 abandoned, could not be started");
  CHECK_FALSE(std::filesystem::exists(marker));

  auto opened = planar::db::agent::open_agent_db_at(sc.root / "agent.db");
  REQUIRE(opened.has_value());
  auto const row = history_of(*opened, 2);
  REQUIRE(row.has_value());
  CHECK(row->outcome == hq::history_outcome::abandoned);
  CHECK_FALSE(row->exit_code.has_value());
  CHECK(hq::list(*opened).value().empty());
}

// ---------------------------------------------------------------------------
// `queue run --detach`: the child's order and its death (plan 1080, task
// hq-detach; tech spec 647 § Submitting). The black-box cases in
// queue_run.t.cpp cannot observe the moment before the fork or make the
// child die on demand; the `detach_hook` seam runs in whichever process
// reaches each stage, so a hook that calls `_exit` is a child that died there.
// ---------------------------------------------------------------------------

namespace {

/// @brief Appends one line to `path`.
void mark(const std::filesystem::path& path, const std::string& line) {
  std::ofstream out(path, std::ios::app);
  out << line << '\n';
}

/// @brief The handler's arguments for a detached `queue run -- true`.
auto detach_args() -> planar::cliapp::parsed_args {
  auto args              = queue_args({"/bin/sh", "-c", "exit 0"});
  args.flags["--detach"] = {"true"};
  return args;
}

} // namespace

TEST_CASE("queue run: --detach forks before it opens the store, and the child is a session leader when it starts",
          "[cmd][agent][queue][hq-detach]") {
  scratch    sc;
  auto const marks = sc.root / "marks";
  auto const store = sc.root / "agent.db";

  agent::handlers::queue_run_deps deps;
  deps.detach_hook = [&](std::string_view stage) {
    auto const state = std::format("{} store={} leader={}", stage, std::filesystem::exists(store), ::getsid(0) == ::getpid());
    mark(marks, state);
    if (stage == "after_setsid") {
      ::_exit(3); // The child ends here, before it opens anything.
    }
  };
  fixture fx{sc, store.string()};
  ::alarm(60); // A parent that waits for a dead child for ever fails the case instead of hanging it.
  auto const outcome = agent::handlers::queue_run_with(fx.ctx, detach_args(), std::move(deps));
  ::alarm(0);
  auto const* status = std::get_if<agent::exit_status>(&outcome);
  REQUIRE(status != nullptr);
  CHECK(status->code == 125);

  std::ifstream            in(marks);
  std::string              line;
  std::vector<std::string> lines;
  while (std::getline(in, line)) {
    lines.push_back(line);
  }
  REQUIRE(lines.size() == 2);
  // In the invoked process the store does not exist yet, and neither does the
  // child's session; in the child the store still does not exist, and it now
  // leads a session of its own.
  CHECK(lines[0] == "before_fork store=false leader=false");
  CHECK(lines[1] == "after_setsid store=false leader=true");
}

TEST_CASE("queue run: --detach whose child dies before it reports exits 125 with no ticket and does not hang",
          "[cmd][agent][queue][hq-detach]") {
  auto const stage = GENERATE(as<std::string>{}, "after_setsid", "after_insert", "before_report");
  INFO("the child dies at " << stage);
  scratch sc;

  agent::handlers::queue_run_deps deps;
  deps.detach_hook = [&](std::string_view at) {
    if (at == stage) {
      ::_exit(9);
    }
  };
  fixture fx{sc, (sc.root / "agent.db").string()};
  ::alarm(60);
  auto const outcome = agent::handlers::queue_run_with(fx.ctx, detach_args(), std::move(deps));
  ::alarm(0);
  auto const* status = std::get_if<agent::exit_status>(&outcome);
  REQUIRE(status != nullptr);
  CHECK(status->code == 125);
  CHECK(fx.out.str().empty());
  CHECK(fx.err.str() == "error: queue: no ticket was issued: the detached submitter ended before it reported\n");
}

TEST_CASE("queue run: --detach aborts before it forks when the process has a second thread", "[cmd][agent][queue][hq-detach]") {
  // The abort cannot be observed in this process, so it is provoked in a
  // forked one: that child starts a thread and then submits detached. The
  // check is at the fork's call site, so the proof is that the child dies of
  // SIGABRT and that no detached submitter ever reached its first stage. The
  // stages come back over a pipe, not a file: Catch2's fatal-signal handler
  // unwinds the dying child's frames, which would delete a scratch directory,
  // so the child takes the default action for SIGABRT instead.
  scratch sc;
  fixture fx{sc, (sc.root / "agent.db").string()};
  int     ends[2]{-1, -1};
  REQUIRE(::pipe(ends) == 0);
  ::alarm(60);
  auto const pid = ::fork();
  REQUIRE(pid >= 0);
  if (pid == 0) {
    ::close(ends[0]);
    ::signal(SIGABRT, SIG_DFL); // Catch2 would report the abort as a failed case.
    std::thread                     lingering([] { std::this_thread::sleep_for(std::chrono::seconds(30)); });
    agent::handlers::queue_run_deps deps;
    deps.detach_hook = [&](std::string_view stage) {
      static_cast<void>(::write(ends[1], std::string{stage}.append("\n").c_str(), stage.size() + 1));
    };
    static_cast<void>(agent::handlers::queue_run_with(fx.ctx, detach_args(), std::move(deps)));
    ::_exit(7); // Not reached when the check holds.
  }
  ::close(ends[1]);
  int status = 0;
  while (::waitpid(pid, &status, 0) < 0) {
    REQUIRE(errno == EINTR);
  }
  ::alarm(0);
  std::string seen;
  char        buffer[128];
  while (auto const n = ::read(ends[0], buffer, sizeof buffer)) {
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n < 0) {
      break;
    }
    seen.append(buffer, static_cast<std::size_t>(n));
  }
  ::close(ends[0]);
  REQUIRE(WIFSIGNALED(status));
  CHECK(WTERMSIG(status) == SIGABRT);
  // The hook ran once, in the invoked process, before the check; a child that
  // had been forked would have reported `after_setsid` as well.
  CHECK(seen == "before_fork\n");
}

// ---------------------------------------------------------------------------
// Task 7068 (hq-unrecorded-group-timeout), task 7080
// (hq-vanished-entry-run-limit) and task 7072 (hq-eperm-zombie-group): the
// submitter's own run-limit enforcement when the entry cannot carry it, and the
// EPERM a macOS kernel answers a group of zombies with.
// ---------------------------------------------------------------------------

namespace {

/// @brief The failure a store that refuses to record the child group reports.
auto record_failure() -> hq::queue_error {
  return hq::queue_error{.kind        = hq::queue_error_kind::query_failed,
                         .sqlite_code = 5,
                         .message     = "hostqueue: record child group: database is locked (sqlite 5)"};
}

/// @brief Every signal the submitter sent, in order, delivered to the real
/// group (the commands under test are this case's own children).
struct signal_log {
  std::shared_ptr<std::vector<int>> sent = std::make_shared<std::vector<int>>();

  [[nodiscard]] auto signaller() const -> hq::group_signaller {
    auto const log = sent;
    return [log](std::int64_t pgid, int sig) {
      log->push_back(sig);
      return ident::signal_group(pgid, sig);
    };
  }
  [[nodiscard]] auto count(int sig) const -> std::ptrdiff_t {
    return std::ranges::count(*sent, sig);
  }
};

/// @brief A command that ignores SIGTERM (and passes the disposition on to its
/// children, as `exec` does), writes `ready` once the trap is in place, and
/// then waits (at most thirty seconds, so a case that fails before it can
/// kill the command does not leave it behind); only SIGKILL ends it early.
auto term_ignoring_command(const std::filesystem::path& ready) -> std::vector<std::string> {
  return {"/bin/sh", "-c",
          std::format("trap '' TERM; echo ready > '{}'; n=0; while [ $n -lt 30 ]; do sleep 1; n=$((n+1)); done", ready.string())};
}

/// @brief Settings with a short grace period, so a SIGKILL follows a SIGTERM
/// after 150 ms on the steered clock.
auto short_grace_settings() -> planar::engine::config::queue_settings {
  return planar::engine::config::queue_settings{
      .slots = 1, .poll_interval_ms = 10, .stale_after_ms = 3'600'000, .grace_ms = 150, .history_days = 30};
}

} // namespace

TEST_CASE("queue run: a child group that could not be recorded is recorded by a retry at the poll interval",
          "[cmd][agent][queue][hq-unrecorded-group-timeout]") {
  scratch                         sc;
  agent::handlers::queue_run_deps deps;
  auto const                      calls    = std::make_shared<int>(0);
  bool                            recorded = false;
  deps.record_child                        = [calls](planar::db::connection& conn, std::int64_t seq, std::int64_t pgid,
                                                     std::int64_t started) -> std::expected<bool, hq::queue_error> {
    if (++*calls == 1) {
      return std::unexpected(record_failure());
    }
    return hq::record_child(conn, seq, pgid, started);
  };
  script(deps, sc, fast_settings(), [&](planar::db::connection& conn) {
    auto const stored = hq::find(conn, 1).value();
    if (stored && stored->child_pgid) {
      recorded = true;
    }
  });
  auto const got = run_queue(sc, {"/bin/sleep", "0.4"}, deps);
  INFO("stderr:\n" << got.err);
  CHECK(got.code == 0);
  CHECK(*calls >= 2);
  CHECK(recorded);                          // the entry carried the group while the command ran
  CHECK(got.err.contains("cannot record")); // the first failure was still said, once
}

TEST_CASE("queue run: a command whose group was never recorded is stopped at its run limit and the entry ends as timeout",
          "[cmd][agent][queue][hq-unrecorded-group-timeout]") {
  // The store refuses to record the group every time. The entry then names no
  // group, so no other process can signal it; the submitter, which holds the
  // child unreaped, signals the group it knows itself, and ends the entry
  // itself, so the history and the exit (124) agree.
  scratch                         sc;
  signal_log                      log;
  agent::handlers::queue_run_deps deps;
  deps.signaller    = log.signaller();
  deps.record_child = [](planar::db::connection&, std::int64_t, std::int64_t,
                         std::int64_t) -> std::expected<bool, hq::queue_error> { return std::unexpected(record_failure()); };
  script(deps, sc, fast_settings(), [](planar::db::connection&) {}, 3000);
  fixture fx{sc, (sc.root / "agent.db").string()};
  auto    args            = queue_args({"/bin/sleep", "30"});
  args.flags["--timeout"] = {"100ms"};
  auto const  outcome     = agent::handlers::queue_run_with(fx.ctx, args, deps);
  auto const* status      = std::get_if<agent::exit_status>(&outcome);
  REQUIRE(status != nullptr);
  INFO("stderr:\n" << fx.err.str());
  CHECK(status->code == 124);
  CHECK(last_line(fx.err.str()) == "queue: entry 1 stopped at its run limit");
  CHECK(log.count(SIGTERM) == 1);

  auto opened = planar::db::agent::open_agent_db_at(sc.root / "agent.db");
  REQUIRE(opened.has_value());
  CHECK(hq::list(*opened).value().empty()); // ended by the submitter, not left for a poll to reap as abandoned
  auto const history = hq::list_history(*opened).value();
  REQUIRE(history.size() == 1);
  CHECK(history.front().outcome == hq::history_outcome::timeout);
}

TEST_CASE("queue run: a command with no recorded group that ignores SIGTERM is killed after the grace period",
          "[cmd][agent][queue][hq-unrecorded-group-timeout]") {
  scratch                         sc;
  signal_log                      log;
  auto const                      clock = std::make_shared<steered_clock>();
  auto const                      ready = sc.root / "ready";
  agent::handlers::queue_run_deps deps;
  deps.clock        = clock;
  deps.signaller    = log.signaller();
  deps.record_child = [](planar::db::connection&, std::int64_t, std::int64_t,
                         std::int64_t) -> std::expected<bool, hq::queue_error> { return std::unexpected(record_failure()); };
  int stage         = 0;
  script(deps, sc, short_grace_settings(), [&](planar::db::connection&) {
    if (stage == 0 && std::filesystem::exists(ready)) {
      stage = 1;
      clock->advance(1'000); // past the run limit: SIGTERM goes next tick
    } else if (stage == 1 && log.count(SIGTERM) == 1) {
      stage = 2;
      clock->advance(151); // past the grace period: SIGKILL goes next tick
    }
  });
  fixture fx{sc, (sc.root / "agent.db").string()};
  auto    args            = queue_args(term_ignoring_command(ready));
  args.flags["--timeout"] = {"100ms"};
  auto const  outcome     = agent::handlers::queue_run_with(fx.ctx, args, deps);
  auto const* status      = std::get_if<agent::exit_status>(&outcome);
  REQUIRE(status != nullptr);
  INFO("stderr:\n" << fx.err.str());
  CHECK(stage == 2);
  CHECK(status->code == 124);
  CHECK(log.count(SIGTERM) == 1);
  CHECK(log.count(SIGKILL) >= 1);
  auto opened = planar::db::agent::open_agent_db_at(sc.root / "agent.db");
  REQUIRE(opened.has_value());
  auto const history = hq::list_history(*opened).value();
  REQUIRE(history.size() == 1);
  CHECK(history.front().outcome == hq::history_outcome::timeout);
}

TEST_CASE("queue run: a submitter never signals a group whose leader is no longer the command it started",
          "[cmd][agent][queue][hq-unrecorded-group-timeout][hq-vanished-entry-run-limit]") {
  // The identity guard: the group id the submitter holds is only signalled
  // while the leader's start time is the one it recorded. A probe that reports
  // another start time for the child stands for a reused id.
  for (bool const vanished : {false, true}) {
    INFO(std::string{vanished ? "entry removed while running" : "group never recorded"});
    scratch                         sc;
    signal_log                      log;
    auto const                      clock = std::make_shared<steered_clock>();
    agent::handlers::queue_run_deps deps;
    deps.clock               = clock;
    deps.signaller           = log.signaller();
    auto       probe         = hq::system_process_probe();
    auto const own           = static_cast<std::int64_t>(::getpid());
    probe.process_start_time = [real = probe.process_start_time,
                                own](std::int64_t pid) -> std::expected<std::optional<ident::start_time>, ident::error> {
      auto found = real(pid);
      if (found && pid != own) {
        // Whoever holds the child's id, alive or not, is not the process that was started.
        return std::optional<ident::start_time>{found->has_value() ? **found + 1 : ident::start_time{1}};
      }
      return found;
    };
    deps.probe = probe;
    if (!vanished) {
      deps.record_child = [](planar::db::connection&, std::int64_t, std::int64_t,
                             std::int64_t) -> std::expected<bool, hq::queue_error> { return std::unexpected(record_failure()); };
    }
    bool       done  = false;
    auto const ready = sc.root / "ready";
    script(deps, sc, one_slot_settings(), [&](planar::db::connection& conn) {
      if (done || !std::filesystem::exists(ready)) {
        return; // the command has not started yet
      }
      auto const stored = hq::find(conn, 1).value();
      if (!stored) {
        return;
      }
      if (vanished) {
        if (!stored->child_pgid) {
          return;
        }
        end_as(conn, 1, hq::history_outcome::abandoned);
      }
      done = true;
      clock->advance(1'000);
    });
    fixture fx{sc, (sc.root / "agent.db").string()};
    auto    args            = queue_args({"/bin/sh", "-c", std::format("echo ready > '{}'; sleep 0.3", ready.string())});
    args.flags["--timeout"] = {"100ms"};
    auto const  outcome     = agent::handlers::queue_run_with(fx.ctx, args, deps);
    auto const* status      = std::get_if<agent::exit_status>(&outcome);
    REQUIRE(status != nullptr);
    INFO("stderr:\n" << fx.err.str());
    CHECK(done);
    CHECK(log.sent->empty());
    CHECK(status->code == 0); // the command ended on its own; nothing was signalled
    CHECK(fx.err.str().contains("reused"));
  }
}

TEST_CASE("queue run: a command whose entry vanished and that ignores SIGTERM is killed after the configured grace period",
          "[cmd][agent][queue][hq-vanished-entry-run-limit]") {
  scratch                         sc;
  signal_log                      log;
  auto const                      clock = std::make_shared<steered_clock>();
  auto const                      ready = sc.root / "ready";
  agent::handlers::queue_run_deps deps;
  deps.clock     = clock;
  deps.signaller = log.signaller();
  int stage      = 0;
  script(deps, sc, short_grace_settings(), [&](planar::db::connection& conn) {
    if (stage == 0 && std::filesystem::exists(ready) && hq::find(conn, 1).value() && hq::find(conn, 1).value()->child_pgid) {
      end_as(conn, 1, hq::history_outcome::abandoned);
      stage = 1;
      clock->advance(1'000);
    } else if (stage == 1 && log.count(SIGTERM) == 1) {
      stage = 2;
      clock->advance(151);
    }
  });
  fixture fx{sc, (sc.root / "agent.db").string()};
  auto    args            = queue_args(term_ignoring_command(ready));
  args.flags["--timeout"] = {"100ms"};
  auto const  outcome     = agent::handlers::queue_run_with(fx.ctx, args, deps);
  auto const* status      = std::get_if<agent::exit_status>(&outcome);
  REQUIRE(status != nullptr);
  INFO("stderr:\n" << fx.err.str());
  CHECK(stage == 2);
  CHECK(status->code == 124);
  REQUIRE(log.sent->size() >= 2);
  CHECK(log.sent->front() == SIGTERM);
  CHECK(log.count(SIGKILL) >= 1);
  CHECK(fx.err.str().contains("run limit"));
}

TEST_CASE("queue run: a forwarded signal that the kernel refuses for a group of zombies is not reported as a failure",
          "[cmd][agent][queue][hq-eperm-zombie-group]") {
  // The signaller stands for a macOS kernel that answers EPERM for a group
  // whose only members are exited processes; the probe says whether that is
  // what the group holds.
  for (bool const zombies : {true, false}) {
    INFO(std::string{zombies ? "the group holds only zombies" : "the group has a live member: the refusal is real"});
    scratch                         sc;
    agent::handlers::queue_run_deps deps;
    deps.signaller = [](std::int64_t, int) -> std::expected<void, ident::error> {
      return std::unexpected(ident::error::not_permitted);
    };
    auto probe               = hq::system_process_probe();
    probe.group_only_zombies = [zombies](std::int64_t) -> std::expected<bool, ident::error> { return zombies; };
    deps.probe               = probe;
    bool raised              = false;
    script(deps, sc, fast_settings(), [&](planar::db::connection& conn) {
      auto const stored = hq::find(conn, 1).value();
      if (!raised && stored && stored->child_pgid) {
        raised = true;
        ::raise(SIGTERM);
      }
    });
    auto const got = run_queue(sc, {"/bin/sleep", "0.3"}, deps);
    INFO("stderr:\n" << got.err);
    CHECK(raised);
    CHECK(got.code == 0); // the fake signaller delivered nothing, so the command ran to its end
    CHECK(got.err.contains("SIGTERM to the command's process group failed") == !zombies);
  }
}

TEST_CASE("queue run --claim: a renewal that keeps failing is reported once across many attempts, and the command runs",
          "[cmd][agent][queue][hq-claim]") {
  // The main database's schema is ahead of the binary, so every attempt fails
  // the same way. Time is steered: each tick moves the monotonic clock a
  // second, so the command's second of real life covers hundreds of seconds of
  // retries (one every five), and a submitter that wrote a warning per attempt
  // would write dozens.
  scratch sc;
  {
    auto opened = planar::db::connection::open((sc.root / "planar.db").string());
    REQUIRE(opened.has_value());
    REQUIRE(
        opened->execute("create table schema_migrations (version integer primary key, description text not null);").has_value());
    REQUIRE(
        opened->execute("insert into schema_migrations (version, description) values (99999, 'from the future');").has_value());
  }
  auto const clock = std::make_shared<steered_clock>();

  agent::handlers::queue_run_deps deps;
  deps.clock = clock;
  script(deps, sc, one_slot_settings(), [clock](planar::db::connection&) { clock->advance(1'000); }, 5'000);
  fixture fx{sc, (sc.root / "agent.db").string()};
  auto    args          = queue_args({"/bin/sh", "-c", "sleep 1; exit 4"});
  args.flags["--claim"] = {"0123456789abcdef0123456789abcdef"};
  auto const  outcome   = agent::handlers::queue_run_with(fx.ctx, args, deps);
  auto const* status    = std::get_if<agent::exit_status>(&outcome);
  REQUIRE(status != nullptr);
  auto const text = fx.err.str();
  INFO("stderr:\n" << text);
  CHECK(status->code == 4);
  std::size_t warnings = 0;
  for (std::size_t at = text.find("warning: queue:"); at != std::string::npos; at = text.find("warning: queue:", at + 1)) {
    ++warnings;
  }
  CHECK(warnings == 1);
  CHECK(last_line(text) == "queue: entry 1 exited with code 4");
}
