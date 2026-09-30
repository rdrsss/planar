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
/// counts the polls, and once `limit` polls have gone by it makes the settings
/// loader return a slot count nothing can lack, so a submitter that should
/// have started at once but is waiting on a regression starts anyway and the
/// case FAILS on `polls` instead of hanging.
struct poll_bound {
  static constexpr int limit = 300;
  std::shared_ptr<int> polls = std::make_shared<int>(0);

  void bind(agent::handlers::queue_run_deps& deps, planar::engine::config::queue_settings settings) const {
    auto const count = polls;
    deps.sleep       = [count](std::chrono::milliseconds) {
      ++*count;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    };
    deps.load_settings = [count, settings]() mutable {
      auto now = settings;
      if (*count >= limit) {
        now.slots = 1000;
      }
      return std::expected<planar::engine::config::queue_settings, planar::engine::config::queue_load_error>{now};
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
  fixture     fx{sc, (sc.root / "blocker" / "agent.db").string()};
  auto const  outcome = agent::handlers::queue_run(fx.ctx, queue_args({"touch", (sc.root / "marker").string()}));
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
