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

import std;
import planar.cliapp.args;
import planar.cmd.planar_agent.context;
import planar.cmd.planar_agent.exit;
import planar.cmd.planar_agent.handler;
import planar.cmd.planar_agent.handlers.queue;
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

/// @brief The handler's parsed arguments for `queue run -- <command>`, as the
/// tree's `command` positional would harvest them.
auto queue_args(const std::vector<std::string>& command) -> planar::cliapp::parsed_args {
  planar::cliapp::parsed_args args;
  args.path                        = {"queue", "run"};
  args.positional_lists["command"] = command;
  args.positionals["command"]      = command.back();
  return args;
}

/// @brief Builds the invocation context of a case: a scratch environment, its
/// own streams.
struct fixture {
  std::ostringstream out;
  std::ostringstream err;
  agent::context     ctx;

  explicit fixture(const scratch& sc, std::string agent_db)
      : ctx({"planar-agent", "queue", "run"},
            agent::map_env({{"PLANAR_AGENT_DB", std::move(agent_db)},
                            {"HOME", (sc.root / "fakehome").string()},
                            {"PWD", (sc.root / "proj").string()},
                            {"PLANAR_CONFIG_PATH", (sc.root / "config.toml").string()},
                            {"PLANAR_DB", (sc.root / "planar.db").string()}}),
            sc.root / "proj", std::make_shared<agent::database>(sc.root / "planar.db", err), out, err) {
  }
};

/// @brief Runs the handler for `command` with its seams replaced by `deps`.
auto run_queue(const scratch& sc, const std::vector<std::string>& command, agent::handlers::queue_run_deps deps) -> invocation {
  fixture     fx{sc, (sc.root / "agent.db").string()};
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
  CHECK(status->code == 0);
  REQUIRE(ticks > 50);
  // Two clock reads per tick are the loop's own (the run-limit check and the
  // poll schedule). A mark attempted at every tick reads a third, inside
  // `begin_terminate`.
  CHECK(clock->reads <= 2 * ticks + 20);
}
