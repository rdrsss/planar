// queue_cancel_seams.t.cpp: `queue cancel` driven in-process through its
// seams (plan 1080, task hq-queue-cancel).
//
// The black-box cases in queue_cancel.t.cpp show what an operator sees, but
// they cannot say WHEN a signal was sent relative to the marker's commit, or
// prove a bound on a group that never empties. Here the real tree and the
// real handler run in this process against a scratch planar.db, with a
// clock, a process probe and a signaller the test controls. Nothing here
// signals a real process: every group id is a made-up number that reaches no
// process, and the recording signaller delivers nothing.

#include <catch2/catch_test_macros.hpp>

#include <signal.h>
#include <unistd.h>

import std;
import planar.cliapp.args;
import planar.cmd.planar_agent.context;
import planar.cmd.planar_agent.exit;
import planar.cmd.planar_agent.handler;
import planar.cmd.planar_agent.handlers.queue;
import planar.db;
import planar.db.migrate;
import planar.db.migrations;
import planar.engine.config.queue;
import planar.engine.hostqueue;
import planar.process.identity;

#include "queue_test_store.hpp"

namespace {

namespace hq    = planar::engine::hostqueue;
namespace ident = planar::process::identity;
namespace agent = planar::cmd::agent;

constexpr std::int64_t k_group   = 4'242'424; // A made-up group id; no such process is ever signalled.
constexpr std::int64_t k_started = 777;

struct scratch {
  std::filesystem::path root;

  scratch()
      : root(std::filesystem::temp_directory_path() /
             std::format("planar_queue_cancel_seams_{}", std::chrono::steady_clock::now().time_since_epoch().count())) {
    std::filesystem::create_directories(root / "proj");
    std::filesystem::create_directories(root / "fakehome");
    // The queue verbs open `planar.db` and never create it.
    planar::cmd::qfix::head_store(root / "planar.db");
  }
  scratch(const scratch&)            = delete;
  scratch& operator=(const scratch&) = delete;
  ~scratch() {
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
  }
};

struct fixture {
  std::ostringstream out;
  std::ostringstream err;
  agent::context     ctx;

  explicit fixture(const scratch& sc, std::map<std::string, std::string, std::less<>> extra = {})
      : ctx({"planar-agent", "queue", "cancel"}, agent::map_env(env(sc, std::move(extra))), sc.root / "proj",
            std::make_shared<agent::database>(sc.root / "planar.db", err), out, err) {
  }

private:
  static auto env(const scratch& sc, std::map<std::string, std::string, std::less<>> extra)
      -> std::map<std::string, std::string, std::less<>> {
    std::map<std::string, std::string, std::less<>> vars{{"HOME", (sc.root / "fakehome").string()},
                                                         {"PWD", (sc.root / "proj").string()},
                                                         {"PLANAR_CONFIG_PATH", (sc.root / "config.toml").string()},
                                                         {"PLANAR_DB", (sc.root / "planar.db").string()}};
    for (auto& [name, value] : extra) {
      vars.insert_or_assign(name, std::move(value));
    }
    return vars;
  }
};

/// @brief A clock the test moves by hand; the seam's sleep advances it.
class hand_clock final : public ident::clock {
public:
  std::int64_t mono = 10'000'000;
  std::int64_t wall = 1'759'000'000'000;

  [[nodiscard]] auto monotonic_ms() -> std::expected<std::int64_t, ident::error> override {
    return mono;
  }
  [[nodiscard]] auto wall_ms() -> std::int64_t override {
    return wall;
  }
};

/// @brief The world the handler sees: which groups have members, what each
/// signal does to them, and every signal that was sent with the clock at that
/// moment.
struct world {
  std::shared_ptr<hand_clock> clock = std::make_shared<hand_clock>();
  std::set<std::int64_t>      live;
  bool                        honours_term = true; // SIGTERM empties the group.
  bool                        killable     = true; // SIGKILL empties the group.
  struct sent {
    std::int64_t pgid;
    int          sig;
    std::int64_t mono;
    bool         marker_committed = false; // Another connection saw `terminating_since_mono`.
    bool         lock_free        = false; // Another connection could take the write lock.
  };
  std::vector<sent>     signals;
  std::filesystem::path store;
  std::int64_t          seq = 0;
  std::function<void()> on_sleep;
  int                   sleeps = 0;

  [[nodiscard]] auto probe() const -> hq::process_probe {
    return hq::process_probe{
        .process_exists     = [](std::int64_t) -> std::expected<bool, ident::error> { return true; },
        .process_start_time = [](std::int64_t) -> std::expected<std::optional<ident::start_time>, ident::error> {
          return std::optional<ident::start_time>{static_cast<ident::start_time>(k_started)};
        },
        .group_has_members = [this](std::int64_t pgid) -> std::expected<bool, ident::error> { return live.contains(pgid); },
    };
  }

  [[nodiscard]] auto signaller() -> hq::group_signaller {
    return [this](std::int64_t pgid, int sig) -> std::expected<void, ident::error> {
      sent record{.pgid = pgid, .sig = sig, .mono = clock->mono};
      if (auto observer = planar::cmd::qfix::open_store(store); observer) {
        static_cast<void>(observer->execute("pragma busy_timeout = 0;"));
        auto const begun = observer->execute("begin immediate;");
        record.lock_free = begun.has_value();
        if (auto found = hq::find(*observer, seq); found && found->has_value()) {
          record.marker_committed = (*found)->terminating_since_mono.has_value();
        }
        if (begun) {
          static_cast<void>(observer->execute("rollback;"));
        }
      }
      signals.push_back(record);
      if ((sig == SIGTERM && honours_term) || (sig == SIGKILL && killable)) {
        live.erase(pgid);
      }
      return {};
    };
  }

  /// @brief The handler's seams: this world's clock, probe, signaller, a
  /// sleep that advances the clock, and a settings loader with `grace_ms`.
  [[nodiscard]] auto deps(std::int64_t grace_ms, std::int64_t tick_ms = 100) -> agent::handlers::queue_run_deps {
    agent::handlers::queue_run_deps d;
    d.clock     = clock;
    d.probe     = probe();
    d.signaller = signaller();
    d.sleep     = [this, tick_ms](std::chrono::milliseconds) {
      clock->mono += tick_ms;
      if (++sleeps > 10'000) {
        throw std::runtime_error("queue cancel waited past the case's bound");
      }
      if (on_sleep) {
        on_sleep();
      }
    };
    d.load_settings = [grace_ms] {
      return std::expected<planar::engine::config::queue_settings, planar::engine::config::queue_load_error>{
          planar::engine::config::queue_settings{
              .slots = 1, .poll_interval_ms = 1'000, .stale_after_ms = 3'600'000, .grace_ms = grace_ms, .history_days = 30}};
    };
    return d;
  }
};

/// @brief Seeds a RUNNING entry whose submitter is this test process, with its
/// command's group recorded (or not). `host` overrides the entry's host
/// identity, to seed an entry that belongs to another host.
auto seed_running(const scratch& sc, world& w, bool record_group = true, std::optional<std::string> host = std::nullopt)
    -> std::int64_t {
  w.store     = sc.root / "planar.db";
  auto opened = planar::cmd::qfix::open_store(w.store);
  REQUIRE(opened.has_value());
  auto const entry_host = host ? *host : ident::host_identity(ident::native_identity_source());
  auto const pid        = static_cast<std::int64_t>(::getpid());
  auto const seq        = hq::enqueue(*opened, hq::enqueue_request{.host_id        = entry_host,
                                                                   .pid            = pid,
                                                                   .pid_started    = k_started,
                                                                   .cwd            = "/seeded",
                                                                   .argv           = {"make", "test"},
                                                                   .enqueued_at    = w.clock->wall,
                                                                   .refreshed_mono = w.clock->mono});
  REQUIRE(seq.has_value());
  auto const polled = hq::poll(
      *opened,
      hq::poll_request{.seq = *seq, .host_id = entry_host, .slots = 8, .stale_after_ms = 3'600'000, .run_limit_ms = 3'600'000},
      *w.clock, w.probe());
  REQUIRE(polled.has_value());
  REQUIRE(polled->running);
  if (record_group) {
    REQUIRE(hq::record_child(*opened, *seq, k_group, k_started).has_value());
    w.live.insert(k_group);
  }
  w.seq = *seq;
  return *seq;
}

/// @brief Marks `seq` terminating as another process already did: SIGTERM is
/// sent through the world's signaller, and the signal record is then cleared,
/// so a case sees only what the cancel under test sends.
auto pre_mark(const scratch& sc, world& w, std::int64_t seq, hq::stop_reason reason, std::optional<hq::canceller> who,
              const std::string& host_id) -> void {
  seq         = planar::cmd::qfix::seq_of(seq);
  auto opened = planar::cmd::qfix::open_store(sc.root / "planar.db");
  REQUIRE(opened.has_value());
  auto const begun = hq::begin_terminate(
      *opened, hq::begin_terminate_request{.seq = seq, .reason = reason, .cancelled_by = who, .host_id = host_id}, *w.clock,
      w.probe(), w.signaller());
  REQUIRE(begun.has_value());
  REQUIRE(begun->status == hq::begin_status::marked);
  w.signals.clear();
}

auto cancel_args(std::int64_t seq) -> planar::cliapp::parsed_args {
  seq = planar::cmd::qfix::seq_of(seq);
  planar::cliapp::parsed_args args;
  args.path                    = {"queue", "cancel"};
  args.positional_lists["seq"] = {std::to_string(seq)};
  args.positionals["seq"]      = std::to_string(seq);
  return args;
}

struct invocation {
  int         code = -1;
  std::string out;
  std::string err;
};

auto run_cancel(const scratch& sc, std::int64_t seq, agent::handlers::queue_run_deps deps) -> invocation {
  seq = planar::cmd::qfix::seq_of(seq);
  fixture     fx{sc};
  auto const  outcome = agent::handlers::queue_cancel_with(fx.ctx, cancel_args(seq), std::move(deps));
  auto const* status  = std::get_if<agent::exit_status>(&outcome);
  REQUIRE(status != nullptr);
  return invocation{.code = status->code, .out = fx.out.str(), .err = fx.err.str()};
}

auto entry_now(const scratch& sc, std::int64_t seq) -> std::optional<hq::entry> {
  seq         = planar::cmd::qfix::seq_of(seq);
  auto opened = planar::cmd::qfix::open_store(sc.root / "planar.db");
  REQUIRE(opened.has_value());
  auto found = hq::find(*opened, seq);
  REQUIRE(found.has_value());
  return *found;
}

auto history_now(const scratch& sc, std::int64_t seq) -> std::optional<hq::history_row> {
  seq         = planar::cmd::qfix::seq_of(seq);
  auto opened = planar::cmd::qfix::open_store(sc.root / "planar.db");
  REQUIRE(opened.has_value());
  auto found = hq::find_history(*opened, seq);
  REQUIRE(found.has_value());
  return *found;
}

} // namespace

TEST_CASE("queue cancel: SIGTERM is sent only after the marker has committed and the write lock is free",
          "[cmd][agent][queue][hq-queue-cancel]") {
  scratch    sc;
  world      w;
  auto const seq = seed_running(sc, w);

  auto const got = run_cancel(sc, seq, w.deps(10'000));
  INFO("stderr:\n" << got.err);
  CHECK(got.code == 0);

  REQUIRE(w.signals.size() == 1);
  CHECK(w.signals[0].sig == SIGTERM);
  CHECK(w.signals[0].pgid == k_group);
  CHECK(w.signals[0].marker_committed);
  CHECK(w.signals[0].lock_free);

  // The command honoured SIGTERM, so cancel ended the entry itself.
  CHECK_FALSE(entry_now(sc, seq).has_value());
  auto const row = history_now(sc, seq);
  REQUIRE(row.has_value());
  CHECK(row->outcome == hq::history_outcome::cancelled);
  REQUIRE(row->cancelled_by.has_value());
  CHECK(row->cancelled_by->pid == static_cast<std::int64_t>(::getpid()));
}

TEST_CASE("queue cancel: SIGKILL is sent only once the grace period has passed, then the entry ends",
          "[cmd][agent][queue][hq-queue-cancel]") {
  scratch sc;
  world   w;
  w.honours_term = false;
  auto const seq = seed_running(sc, w);

  auto const marked_from = w.clock->mono;
  auto const got         = run_cancel(sc, seq, w.deps(1'000, 250));
  INFO("stderr:\n" << got.err);
  CHECK(got.code == 0);
  CHECK(got.out.find("SIGKILL") != std::string::npos);

  // TERM first, KILL later and only after the grace period, and no more
  // signals once the group is empty.
  REQUIRE(w.signals.size() >= 2);
  CHECK(w.signals.front().sig == SIGTERM);
  auto const kill = std::ranges::find_if(w.signals, [](const auto& s) { return s.sig == SIGKILL; });
  REQUIRE(kill != w.signals.end());
  CHECK(kill->mono - marked_from > 1'000);
  CHECK(kill->marker_committed);
  CHECK(w.signals.back().sig == SIGKILL);
  CHECK_FALSE(w.live.contains(k_group));

  CHECK_FALSE(entry_now(sc, seq).has_value());
  REQUIRE(history_now(sc, seq).has_value());
  CHECK(history_now(sc, seq)->outcome == hq::history_outcome::cancelled);
}

TEST_CASE("queue cancel: an entry is never removed while its group has members, and the wait is bounded",
          "[cmd][agent][queue][hq-queue-cancel]") {
  scratch sc;
  world   w;
  w.honours_term = false;
  w.killable     = false; // Survives SIGKILL, as an uninterruptible process does.
  auto const seq = seed_running(sc, w);

  auto const started = w.clock->mono;
  auto const got     = run_cancel(sc, seq, w.deps(1'000, 500));
  INFO("stderr:\n" << got.err);
  CHECK(got.code == 125);
  CHECK(got.err.find("still has members") != std::string::npos);

  // The bound is the grace period plus the drain slack (15s), not for ever.
  CHECK(w.clock->mono - started <= 1'000 + 15'000 + 1'000);
  CHECK(w.sleeps < 200);

  // The entry stays, marked terminating and keeping its slot; no history row.
  auto const stored = entry_now(sc, seq);
  REQUIRE(stored.has_value());
  CHECK(stored->terminating_since_mono.has_value());
  CHECK(stored->terminate_reason == "cancelled");
  CHECK_FALSE(history_now(sc, seq).has_value());
  CHECK(w.live.contains(k_group));
}

TEST_CASE("queue cancel: a command whose group is recorded after the cancel is still sent SIGTERM",
          "[cmd][agent][queue][hq-queue-cancel]") {
  scratch    sc;
  world      w;
  auto const seq = seed_running(sc, w, /*record_group=*/false);

  // The submitter records the command's group a moment after the cancel
  // marked the entry; nothing else will send this SIGTERM.
  w.on_sleep = [&] {
    if (w.sleeps == 2) {
      auto opened = planar::cmd::qfix::open_store(sc.root / "planar.db");
      REQUIRE(opened.has_value());
      REQUIRE(hq::record_child(*opened, seq, k_group, k_started).has_value());
      w.live.insert(k_group);
    }
  };
  auto const got = run_cancel(sc, seq, w.deps(10'000));
  INFO("stderr:\n" << got.err);
  CHECK(got.code == 0);
  REQUIRE(w.signals.size() == 1);
  CHECK(w.signals[0].sig == SIGTERM);
  CHECK(w.signals[0].pgid == k_group);
  CHECK(history_now(sc, seq).has_value());
}

TEST_CASE("queue cancel: the canceller's vendor and role come from the flags, then the environment",
          "[cmd][agent][queue][hq-queue-cancel][hq-vendor-role]") {
  struct variant {
    std::vector<std::pair<std::string, std::string>> flags;
    std::map<std::string, std::string, std::less<>>  env;
    std::optional<std::string>                       vendor;
    std::optional<std::string>                       role;
  };
  std::vector<variant> const variants{
      {{{"--vendor", "fv"}, {"--role", "fr"}}, {}, "fv", "fr"},
      {{}, {{"PLANAR_VENDOR", "ev"}, {"PLANAR_ROLE", "er"}}, "ev", "er"},
      {{{"--vendor", "fv"}, {"--role", "fr"}}, {{"PLANAR_VENDOR", "ev"}, {"PLANAR_ROLE", "er"}}, "fv", "fr"},
      {{}, {}, std::nullopt, std::nullopt},
      {{{"--vendor", ""}, {"--role", ""}}, {{"PLANAR_VENDOR", "ev"}, {"PLANAR_ROLE", "er"}}, "ev", "er"},
  };
  for (auto const& v : variants) {
    scratch    sc;
    world      w;
    auto const seq = seed_running(sc, w);

    fixture                     fx{sc, v.env};
    planar::cliapp::parsed_args args = cancel_args(seq);
    for (auto const& [name, value] : v.flags) {
      args.flags[name] = {value};
    }
    auto const outcome = agent::handlers::queue_cancel_with(fx.ctx, args, w.deps(10'000));
    REQUIRE(std::get_if<agent::exit_status>(&outcome) != nullptr);
    CHECK(std::get<agent::exit_status>(outcome).code == 0);
    auto const row = history_now(sc, seq);
    REQUIRE(row.has_value());
    REQUIRE(row->cancelled_by.has_value());
    CHECK(row->cancelled_by->vendor == v.vendor);
    CHECK(row->cancelled_by->role == v.role);
  }
}

TEST_CASE("queue cancel: an entry already stopping by a cancel keeps its first canceller and gets no second SIGTERM",
          "[cmd][agent][queue][hq-queue-cancel]") {
  scratch sc;
  world   w;
  w.honours_term          = false; // The first cancel's SIGTERM did nothing.
  auto const          seq = seed_running(sc, w);
  hq::canceller const first{.vendor = "first", .role = "one", .pid = 111};
  pre_mark(sc, w, seq, hq::stop_reason::cancelled, first, ident::host_identity(ident::native_identity_source()));

  auto    deps = w.deps(1'000, 250);
  fixture fx{sc};
  auto    args           = cancel_args(seq);
  args.flags["--vendor"] = {"second"};
  args.flags["--role"]   = {"two"};
  auto const outcome     = agent::handlers::queue_cancel_with(fx.ctx, args, std::move(deps));
  REQUIRE(std::get_if<agent::exit_status>(&outcome) != nullptr);
  CHECK(std::get<agent::exit_status>(outcome).code == 0);

  // Only the overdue SIGKILL was sent: no second SIGTERM.
  REQUIRE_FALSE(w.signals.empty());
  CHECK(std::ranges::none_of(w.signals, [](const auto& s) { return s.sig == SIGTERM; }));
  CHECK(w.signals.front().sig == SIGKILL);

  // The row names the first canceller; the second was not recorded.
  auto const row = history_now(sc, seq);
  REQUIRE(row.has_value());
  CHECK(row->outcome == hq::history_outcome::cancelled);
  REQUIRE(row->cancelled_by.has_value());
  CHECK(*row->cancelled_by == first);
}

TEST_CASE("queue cancel: SIGKILL is sent at once when the grace period has already passed",
          "[cmd][agent][queue][hq-queue-cancel]") {
  scratch sc;
  world   w;
  w.honours_term = false;
  auto const seq = seed_running(sc, w);
  pre_mark(sc, w, seq, hq::stop_reason::cancelled, hq::canceller{.pid = 111},
           ident::host_identity(ident::native_identity_source()));

  w.clock->mono += 5'000; // Well past the one-second grace.
  auto const before = w.clock->mono;
  auto const got    = run_cancel(sc, seq, w.deps(1'000, 250));
  INFO("stderr:\n" << got.err);
  CHECK(got.code == 0);
  REQUIRE_FALSE(w.signals.empty());
  CHECK(w.signals.front().sig == SIGKILL);
  CHECK(w.signals.front().mono == before); // No time passed before it.
  CHECK(w.sleeps <= 1);
}

TEST_CASE("queue cancel: an entry already stopping at its run limit ends as timeout and cancel exits 0",
          "[cmd][agent][queue][hq-queue-cancel]") {
  scratch sc;
  world   w;
  w.honours_term = false;
  auto const seq = seed_running(sc, w);
  pre_mark(sc, w, seq, hq::stop_reason::timeout, std::nullopt, ident::host_identity(ident::native_identity_source()));

  w.clock->mono += 5'000;
  auto const got = run_cancel(sc, seq, w.deps(1'000, 250));
  INFO("stderr:\n" << got.err);
  CHECK(got.code == 0);
  CHECK(got.out.find("already stopping and ended as timeout") != std::string::npos);
  auto const row = history_now(sc, seq);
  REQUIRE(row.has_value());
  CHECK(row->outcome == hq::history_outcome::timeout);
  CHECK_FALSE(row->cancelled_by.has_value());
}

TEST_CASE("queue cancel: an entry of another host is marked, nothing is signalled and cancel exits 125 at once",
          "[cmd][agent][queue][hq-queue-cancel]") {
  scratch    sc;
  world      w;
  auto const seq = seed_running(sc, w, true, "other-host");

  auto const before = w.clock->mono;
  auto const got    = run_cancel(sc, seq, w.deps(1'000, 250));
  INFO("stderr:\n" << got.err);
  CHECK(got.code == 125);
  CHECK(got.err.find("another host identity") != std::string::npos);
  CHECK(got.err.find("now marked") != std::string::npos);
  CHECK(got.err.find("after SIGKILL") == std::string::npos);
  CHECK(w.signals.empty());
  CHECK(w.sleeps == 0);
  CHECK(w.clock->mono == before);

  auto const stored = entry_now(sc, seq);
  REQUIRE(stored.has_value());
  CHECK(stored->terminate_reason == "cancelled");
  CHECK(stored->terminating_since_mono.has_value());
  CHECK_FALSE(history_now(sc, seq).has_value());
}

TEST_CASE("queue cancel: an entry of another host that is already terminating is refused at once, not after a false wait",
          "[cmd][agent][queue][hq-queue-cancel]") {
  scratch    sc;
  world      w;
  auto const seq = seed_running(sc, w, true, "other-host");
  pre_mark(sc, w, seq, hq::stop_reason::timeout, std::nullopt, "other-host");

  auto const before = w.clock->mono;
  auto const got    = run_cancel(sc, seq, w.deps(1'000, 250));
  INFO("stderr:\n" << got.err);
  CHECK(got.code == 125);
  CHECK(got.err.find("another host identity") != std::string::npos);
  CHECK(got.err.find("already marked") != std::string::npos);
  CHECK(got.err.find("after SIGKILL") == std::string::npos);
  CHECK(w.signals.empty());
  CHECK(w.sleeps == 0);
  CHECK(w.clock->mono == before);
  CHECK(entry_now(sc, seq).has_value());
}

TEST_CASE("queue cancel: a group that was never recorded is reported as such, not as surviving SIGKILL",
          "[cmd][agent][queue][hq-queue-cancel]") {
  scratch    sc;
  world      w;
  auto const seq = seed_running(sc, w, /*record_group=*/false);

  auto const got = run_cancel(sc, seq, w.deps(1'000, 500));
  INFO("stderr:\n" << got.err);
  CHECK(got.code == 125);
  CHECK(got.err.find("never recorded") != std::string::npos);
  CHECK(got.err.find("after SIGKILL") == std::string::npos);
  CHECK(w.signals.empty()); // Nothing could be signalled.
  auto const stored = entry_now(sc, seq);
  REQUIRE(stored.has_value());
  CHECK(stored->terminating_since_mono.has_value());
}

TEST_CASE("queue cancel: cancelling a nested entry stops its own group and leaves the parent untouched",
          "[cmd][agent][queue][hq-queue-cancel]") {
  scratch    sc;
  world      w;
  auto const parent = seed_running(sc, w);

  auto opened = planar::cmd::qfix::open_store(sc.root / "planar.db");
  REQUIRE(opened.has_value());
  auto const host = ident::host_identity(ident::native_identity_source());
  auto const nested =
      hq::enqueue_nested(*opened, parent,
                         hq::enqueue_request{.host_id        = host,
                                             .pid            = static_cast<std::int64_t>(::getpid()),
                                             .pid_started    = k_started,
                                             .cwd            = "/seeded",
                                             .argv           = {"make", "inner"},
                                             .enqueued_at    = w.clock->wall,
                                             .refreshed_mono = w.clock->mono},
                         hq::nested_limits{.stale_after_ms = 3'600'000, .run_limit_ms = 3'600'000}, *w.clock, w.probe());
  REQUIRE(nested.has_value());
  REQUIRE(nested->status == hq::nested_status::inserted);
  auto const             child_seq      = nested->seq;
  constexpr std::int64_t k_nested_group = k_group + 1;
  REQUIRE(hq::record_child(*opened, child_seq, k_nested_group, k_started).has_value());
  w.live.insert(k_nested_group);
  w.seq = child_seq;

  auto const got = run_cancel(sc, child_seq, w.deps(10'000));
  INFO("stderr:\n" << got.err);
  CHECK(got.code == 0);

  // Only the nested entry's group was signalled, and only it ended.
  REQUIRE(w.signals.size() == 1);
  CHECK(w.signals[0].pgid == k_nested_group);
  CHECK(w.live.contains(k_group));
  auto const row = history_now(sc, child_seq);
  REQUIRE(row.has_value());
  CHECK(row->outcome == hq::history_outcome::cancelled);
  CHECK(row->nested);
  CHECK(row->parent_seq == parent);

  auto const kept = entry_now(sc, parent);
  REQUIRE(kept.has_value());
  CHECK(kept->state == hq::entry_state::running);
  CHECK_FALSE(kept->terminating_since_mono.has_value());
  CHECK_FALSE(history_now(sc, parent).has_value());
}

TEST_CASE("queue cancel: an entry its own submitter ends as exited after the mark is reported as ended, exit 6, not as cancelled",
          "[cmd][agent][queue][hq-queue-cancel]") {
  scratch sc;
  world   w;
  w.honours_term = false; // The command is already on its way out, so SIGTERM changes nothing here.
  auto const seq = seed_running(sc, w);

  // After cancel has marked the entry, the submitter ends it with what it
  // observed of its command, having read the entry before the mark.
  w.on_sleep = [&] {
    if (w.sleeps == 1) {
      auto opened = planar::cmd::qfix::open_store(sc.root / "planar.db");
      REQUIRE(opened.has_value());
      auto const ended =
          hq::end_entry(*opened, seq, hq::end_request{.outcome = hq::history_outcome::exited, .exit_code = 0, .ended_at = 1});
      REQUIRE(ended.has_value());
      w.live.erase(k_group);
    }
  };
  auto const got = run_cancel(sc, seq, w.deps(10'000));
  INFO("stderr:\n" << got.err);
  CHECK(got.code == 6);
  CHECK(got.err.find("ended as exited before the cancellation took effect") != std::string::npos);
  CHECK(got.out.empty());
  auto const row = history_now(sc, seq);
  REQUIRE(row.has_value());
  CHECK(row->outcome == hq::history_outcome::exited);
}

TEST_CASE("queue cancel: a group that was already empty is reported as an exited command, not as stopped",
          "[cmd][agent][queue][hq-queue-cancel]") {
  scratch    sc;
  world      w;
  auto const seq = seed_running(sc, w);
  w.live.clear(); // The command exited on its own; its entry has not been ended yet.

  auto const got = run_cancel(sc, seq, w.deps(10'000));
  INFO("stderr:\n" << got.err);
  CHECK(got.code == 0);
  CHECK(w.signals.empty());
  CHECK(got.out.find("already exited") != std::string::npos);
  CHECK(got.out.find("was stopped") == std::string::npos);
  auto const row = history_now(sc, seq);
  REQUIRE(row.has_value());
  CHECK(row->outcome == hq::history_outcome::cancelled);
}

namespace {

/// @brief A clock that runs a hook at every wall-clock read, so a case can
/// act at the exact moment the submitter writes the end of its entry.
class hooked_clock final : public ident::clock {
public:
  ident::system_clock   inner;
  std::function<void()> on_wall;

  [[nodiscard]] auto monotonic_ms() -> std::expected<std::int64_t, ident::error> override {
    return inner.monotonic_ms();
  }
  [[nodiscard]] auto wall_ms() -> std::int64_t override {
    if (on_wall) {
      on_wall();
    }
    return inner.wall_ms();
  }
};

/// @brief What `queue run -- true --notices` reports when another process ends
/// its entry with `outcome` after the command exited and before the submitter
/// writes its own end.
auto run_and_lose_the_end(hq::history_outcome outcome) -> invocation {
  scratch sc;
  {
    auto opened = planar::cmd::qfix::open_store(sc.root / "planar.db");
    REQUIRE(opened.has_value()); // Create the store before the submitter races anything.
  }
  auto clock = std::make_shared<hooked_clock>();
  bool fired = false;
  // The poll interval is a minute, so the only wall-clock read once the entry
  // is running with its group recorded is the one that stamps the final end.
  clock->on_wall = [&] {
    if (fired) {
      return;
    }
    auto opened = planar::cmd::qfix::open_store(sc.root / "planar.db");
    if (!opened) {
      return;
    }
    auto const entry = hq::find(*opened, planar::cmd::qfix::seq_of(1));
    if (!entry || !entry->has_value() || (*entry)->state != hq::entry_state::running || !(*entry)->child_pgid) {
      return;
    }
    fired = true;
    hq::end_request request{.outcome = outcome, .ended_at = 1};
    if (outcome == hq::history_outcome::cancelled) {
      request.cancelled_by = hq::canceller{.vendor = "other", .role = "canceller", .pid = 4242};
    }
    REQUIRE(hq::end_entry(*opened, planar::cmd::qfix::seq_of(1), request).has_value());
  };

  agent::handlers::queue_run_deps deps;
  deps.clock         = clock;
  deps.sleep         = [](std::chrono::milliseconds) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); };
  deps.load_settings = [] {
    return std::expected<planar::engine::config::queue_settings, planar::engine::config::queue_load_error>{
        planar::engine::config::queue_settings{
            .slots = 1, .poll_interval_ms = 60'000, .stale_after_ms = 3'600'000, .grace_ms = 10'000, .history_days = 30}};
  };
  planar::cliapp::parsed_args args;
  args.path                        = {"queue", "run"};
  args.positional_lists["command"] = {"true"};
  args.positionals["command"]      = "true";
  args.flags["--notices"]          = {"true"};

  std::ostringstream out;
  std::ostringstream err;
  agent::context     ctx({"planar-agent", "queue", "run"},
                         agent::map_env({{"HOME", (sc.root / "fakehome").string()},
                                         {"PWD", (sc.root / "proj").string()},
                                         {"PATH", "/usr/bin:/bin"},
                                         {"PLANAR_CONFIG_PATH", (sc.root / "config.toml").string()},
                                         {"PLANAR_DB", (sc.root / "planar.db").string()}}),
                         sc.root / "proj", std::make_shared<agent::database>(sc.root / "planar.db", err), out, err);
  auto const         outcome_of_run = agent::handlers::queue_run_with(ctx, args, std::move(deps));
  auto const*        status         = std::get_if<agent::exit_status>(&outcome_of_run);
  REQUIRE(status != nullptr);
  REQUIRE(fired);
  return invocation{.code = status->code, .out = out.str(), .err = err.str()};
}

} // namespace

TEST_CASE("queue run: a command that exited while a cancel ended its entry reports the cancel, exit 125",
          "[cmd][agent][queue][hq-queue-cancel]") {
  auto const got = run_and_lose_the_end(hq::history_outcome::cancelled);
  INFO("stderr:\n" << got.err);
  CHECK(got.code == 125);
  CHECK(got.err.find("queue: entry 1000001 cancelled\n") != std::string::npos);
  CHECK(got.err.find("exited with code 0") == std::string::npos);
}

TEST_CASE("queue run: a command that exited while its entry was ended at its run limit reports the limit, exit 124",
          "[cmd][agent][queue][hq-queue-cancel]") {
  auto const got = run_and_lose_the_end(hq::history_outcome::timeout);
  INFO("stderr:\n" << got.err);
  CHECK(got.code == 124);
  CHECK(got.err.find("queue: entry 1000001 stopped at its run limit\n") != std::string::npos);
}

TEST_CASE("queue run: a command that exited while its entry was reaped keeps the documented exit and notice",
          "[cmd][agent][queue][hq-queue-cancel]") {
  auto const got = run_and_lose_the_end(hq::history_outcome::abandoned);
  INFO("stderr:\n" << got.err);
  CHECK(got.code == 0);
  CHECK(got.err.find("queue: entry 1000001 exited with code 0\n") != std::string::npos);
}
