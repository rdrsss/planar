// @file status.t.cpp
// @brief Unit tests for `planar.engine.hostqueue.status` (plan 1080, task
// hq-queue-status). The black-box cases in src/cmd/planar-agent/
// queue_status.t.cpp cover the operator-facing scenarios; these cover what a
// binary cannot reach: a fake process probe, a store that names a cycle or a
// pruned successor, the configuration being asked for only when needed, and
// the read-only guarantee at the connection.
//
// Include-before-import is deliberate (see core/version.t.cpp / db.t.cpp).
#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.db.agentdb;
import planar.process.identity;
import planar.engine.hostqueue;

namespace {

namespace hq    = planar::engine::hostqueue;
namespace ident = planar::process::identity;

/// @brief A unique scratch directory, removed when the guard goes out of scope.
struct scratch_dir {
  std::filesystem::path path_;

  scratch_dir()
      : path_(std::filesystem::temp_directory_path() / std::format("planar_hostqueue_status_test_{}_{}",
                                                                   std::chrono::steady_clock::now().time_since_epoch().count(),
                                                                   reinterpret_cast<std::uintptr_t>(this))) {
    std::filesystem::create_directories(path_);
  }

  scratch_dir(const scratch_dir&)            = delete;
  scratch_dir& operator=(const scratch_dir&) = delete;

  ~scratch_dir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
};

auto open_scratch_store(const scratch_dir& scratch) -> planar::db::connection {
  auto opened = planar::db::agent::open_agent_db_at(scratch.path_ / "agent.db");
  REQUIRE(opened.has_value());
  return std::move(*opened);
}

constexpr std::int64_t k_now_mono = 1'000'000;
constexpr std::int64_t k_now_wall = 1'759'000'100'000;

auto request_for(std::string label, std::int64_t refreshed_mono = k_now_mono) -> hq::enqueue_request {
  return hq::enqueue_request{.host_id        = "host-a",
                             .pid            = 4242,
                             .pid_started    = 77,
                             .cwd            = "/work",
                             .argv           = {"make", "test"},
                             .label          = std::move(label),
                             .vendor         = "claude",
                             .role           = "coder",
                             .enqueued_at    = k_now_wall - 5'000,
                             .refreshed_mono = refreshed_mono};
}

/// @brief A probe that answers the same about every process.
auto fixed_probe(bool exists, std::uint64_t started = 77) -> hq::process_probe {
  return hq::process_probe{
      .process_exists     = [exists](std::int64_t) -> std::expected<bool, ident::error> { return exists; },
      .process_start_time = [exists, started](std::int64_t) -> std::expected<std::optional<ident::start_time>, ident::error> {
        if (!exists) {
          return std::optional<ident::start_time>{};
        }
        return std::optional<ident::start_time>{ident::start_time{started}};
      },
      .group_has_members = [](std::int64_t) -> std::expected<bool, ident::error> { return false; }};
}

auto status_request(hq::process_probe probe) -> hq::status_request {
  return hq::status_request{
      .host_id  = "host-a",
      .now_mono = k_now_mono,
      .now_wall = k_now_wall,
      .settings = []() -> std::expected<hq::status_settings, std::string> {
        return hq::status_settings{.slots = 3, .stale_after_ms = 30'000, .grace_ms = 4'000};
      },
      .probe = std::move(probe),
  };
}

auto enqueue_ok(planar::db::connection& conn, const hq::enqueue_request& request) -> std::int64_t {
  auto const seq = hq::enqueue(conn, request);
  REQUIRE(seq.has_value());
  return *seq;
}

void end_abandoned(planar::db::connection& conn, std::int64_t seq) {
  REQUIRE(
      hq::end_entry(conn, seq, hq::end_request{.outcome = hq::history_outcome::abandoned, .ended_at = k_now_wall}).has_value());
}

} // namespace

TEST_CASE("query_status: a number the store never issued is not an error and has no status",
          "[engine][hostqueue][hq-queue-status]") {
  scratch_dir scratch;
  auto        conn = open_scratch_store(scratch);
  static_cast<void>(enqueue_ok(conn, request_for("only")));
  auto const answer = hq::query_status(conn, 500, status_request(fixed_probe(true)));
  REQUIRE(answer.has_value());
  CHECK_FALSE(answer->has_value());
}

TEST_CASE("query_status: liveness is the probe's answer, judged against the staleness window",
          "[engine][hostqueue][hq-queue-status]") {
  scratch_dir scratch;
  auto        conn  = open_scratch_store(scratch);
  auto const  fresh = enqueue_ok(conn, request_for("fresh"));
  auto const  stale = enqueue_ok(conn, request_for("stale", k_now_mono - 30'001));

  auto const alive = hq::query_status(conn, fresh, status_request(fixed_probe(true)));
  REQUIRE((alive.has_value() && alive->has_value()));
  CHECK((*alive)->live == std::optional<bool>{true});
  CHECK((*alive)->slots == 3);
  CHECK((*alive)->grace_ms == 4'000);
  CHECK((*alive)->waited_ms == 5'000);

  auto const gone = hq::query_status(conn, fresh, status_request(fixed_probe(false)));
  REQUIRE((gone.has_value() && gone->has_value()));
  CHECK((*gone)->live == std::optional<bool>{false});

  auto const mismatch = hq::query_status(conn, fresh, status_request(fixed_probe(true, 78)));
  REQUIRE((mismatch.has_value() && mismatch->has_value()));
  CHECK((*mismatch)->live == std::optional<bool>{false});

  auto const old = hq::query_status(conn, stale, status_request(fixed_probe(true)));
  REQUIRE((old.has_value() && old->has_value()));
  CHECK((*old)->live == std::optional<bool>{false});

  // Nothing was reaped by any of those.
  auto const entries = hq::list(conn);
  REQUIRE(entries.has_value());
  CHECK(entries->size() == 2);
  auto const history = hq::list_history(conn);
  REQUIRE(history.has_value());
  CHECK(history->empty());
}

TEST_CASE("query_status: the settings are asked for only when the entry is still in the queue",
          "[engine][hostqueue][hq-queue-status]") {
  scratch_dir scratch;
  auto        conn    = open_scratch_store(scratch);
  auto const  waiting = enqueue_ok(conn, request_for("waiting"));
  auto const  ended   = enqueue_ok(conn, request_for("ended"));
  REQUIRE(
      hq::end_entry(conn, ended, hq::end_request{.outcome = hq::history_outcome::exited, .exit_code = 0, .ended_at = k_now_wall})
          .has_value());

  int  asked       = 0;
  auto request     = status_request(fixed_probe(true));
  request.settings = [&asked]() -> std::expected<hq::status_settings, std::string> {
    ++asked;
    return std::unexpected(std::string{"the configuration is unusable"});
  };

  auto const history = hq::query_status(conn, ended, request);
  REQUIRE((history.has_value() && history->has_value()));
  CHECK(asked == 0);
  CHECK((*history)->state == hq::status_state::ended);
  CHECK_FALSE((*history)->slots.has_value());

  auto const refused = hq::query_status(conn, waiting, request);
  REQUIRE_FALSE(refused.has_value());
  CHECK(refused.error().kind == hq::status_error_kind::settings);
  CHECK(refused.error().message == "the configuration is unusable");
  CHECK(asked == 1);
}

TEST_CASE("query_status: a successor that was pruned ends the chain at the last row that exists",
          "[engine][hostqueue][hq-queue-status]") {
  scratch_dir scratch;
  auto        conn = open_scratch_store(scratch);
  auto const  old  = enqueue_ok(conn, request_for("waiter"));
  end_abandoned(conn, old);
  REQUIRE(hq::record_successor(conn, old, 900).has_value()); // 900 has neither an entry nor a history row

  auto const answer = hq::query_status(conn, old, status_request(fixed_probe(true)));
  REQUIRE((answer.has_value() && answer->has_value()));
  CHECK((*answer)->seq == old);
  CHECK((*answer)->superseded_by == std::optional<std::int64_t>{900});
  CHECK((*answer)->state == hq::status_state::ended);
  CHECK((*answer)->outcome == std::optional<hq::history_outcome>{hq::history_outcome::abandoned});
}

TEST_CASE("query_status: a store whose successors form a cycle ends the chain instead of looping",
          "[engine][hostqueue][hq-queue-status]") {
  scratch_dir scratch;
  auto        conn = open_scratch_store(scratch);
  auto const  a    = enqueue_ok(conn, request_for("a"));
  auto const  b    = enqueue_ok(conn, request_for("b"));
  end_abandoned(conn, a);
  end_abandoned(conn, b);
  REQUIRE(hq::record_successor(conn, a, b).has_value());
  REQUIRE(hq::record_successor(conn, b, a).has_value());

  auto const answer = hq::query_status(conn, a, status_request(fixed_probe(true)));
  REQUIRE((answer.has_value() && answer->has_value()));
  CHECK((*answer)->seq == a);
  CHECK((*answer)->superseded_by == std::optional<std::int64_t>{b});
  CHECK((*answer)->state == hq::status_state::ended);
}

TEST_CASE("query_status: a read-only connection answers, and refuses every write", "[engine][hostqueue][hq-queue-status]") {
  scratch_dir  scratch;
  std::int64_t seq = 0;
  {
    auto conn = open_scratch_store(scratch);
    seq       = enqueue_ok(conn, request_for("waiting"));
  }
  auto ro = planar::db::agent::open_agent_db_read_only_at(scratch.path_ / "agent.db");
  REQUIRE(ro.has_value());
  CHECK(ro->is_read_only());

  auto const answer = hq::query_status(*ro, seq, status_request(fixed_probe(false)));
  REQUIRE((answer.has_value() && answer->has_value()));
  CHECK((*answer)->live == std::optional<bool>{false});
  CHECK((*answer)->position == std::optional<std::int64_t>{1});

  // The store itself refuses what the engine never attempts.
  CHECK_FALSE(ro->execute("delete from queue_entries").has_value());
  CHECK_FALSE(ro->execute("update queue_entries set refreshed_mono = 0").has_value());
  auto const still = hq::find(*ro, seq);
  REQUIRE((still.has_value() && still->has_value()));
  CHECK((*still)->refreshed_mono == k_now_mono);
}

TEST_CASE("query_status: reports the limits the entry recorded, from the entry and then from its history row",
          "[engine][hostqueue][hq-queue-status][hq-queue-limit-columns]") {
  // Task hq-queue-limit-columns. The wait limit is recorded at enqueue, the
  // run limit when the entry starts (here by a real poll), and ending the
  // entry copies both into the row the answer then comes from.
  scratch_dir scratch;
  auto        conn           = open_scratch_store(scratch);
  auto        limited        = request_for("limited");
  limited.wait_deadline_mono = k_now_mono + 600'000;
  limited.wait_limit_ms      = 600'000;
  auto const seq             = enqueue_ok(conn, limited);
  auto const unlimited       = enqueue_ok(conn, request_for("unlimited"));

  auto const waiting = hq::query_status(conn, seq, status_request(fixed_probe(true)));
  REQUIRE((waiting.has_value() && waiting->has_value()));
  CHECK((*waiting)->wait_limit_ms == 600'000);
  CHECK_FALSE((*waiting)->run_limit_ms.has_value());
  auto const none = hq::query_status(conn, unlimited, status_request(fixed_probe(true)));
  REQUIRE((none.has_value() && none->has_value()));
  CHECK_FALSE((*none)->wait_limit_ms.has_value());

  ident::system_clock clock;
  auto const          polled = hq::poll(
      conn, hq::poll_request{.seq = seq, .host_id = "host-a", .slots = 1, .stale_after_ms = 30'000, .run_limit_ms = 300'000},
      clock, fixed_probe(true));
  REQUIRE(polled.has_value());
  REQUIRE(polled->started);
  auto const running = hq::query_status(conn, seq, status_request(fixed_probe(true)));
  REQUIRE((running.has_value() && running->has_value()));
  CHECK((*running)->state == hq::status_state::running);
  CHECK((*running)->run_limit_ms == 300'000);
  CHECK((*running)->wait_limit_ms == 600'000);

  REQUIRE(
      hq::end_entry(conn, seq, hq::end_request{.outcome = hq::history_outcome::exited, .exit_code = 0, .ended_at = k_now_wall})
          .has_value());
  auto const ended = hq::query_status(conn, seq, status_request(fixed_probe(true)));
  REQUIRE((ended.has_value() && ended->has_value()));
  CHECK((*ended)->state == hq::status_state::ended);
  CHECK((*ended)->run_limit_ms == 300'000);
  CHECK((*ended)->wait_limit_ms == 600'000);
}

TEST_CASE("query_status: settings without a slot count or grace period report both as unknown and still judge liveness",
          "[engine][hostqueue][hq-queue-status][hq-status-degrade-config]") {
  // Task hq-status-degrade-config: the degraded settings `queue status`
  // supplies for an unusable configuration. Liveness uses the window given.
  scratch_dir scratch;
  auto        conn  = open_scratch_store(scratch);
  auto const  fresh = enqueue_ok(conn, request_for("fresh", k_now_mono - 20'000));

  auto request     = status_request(fixed_probe(true));
  request.settings = []() -> std::expected<hq::status_settings, std::string> {
    return hq::status_settings{.slots = std::nullopt, .stale_after_ms = 30'000, .grace_ms = std::nullopt};
  };
  auto const answer = hq::query_status(conn, fresh, request);
  REQUIRE((answer.has_value() && answer->has_value()));
  CHECK_FALSE((*answer)->slots.has_value());
  CHECK_FALSE((*answer)->grace_ms.has_value());
  CHECK((*answer)->live == std::optional<bool>{true});

  request.settings = []() -> std::expected<hq::status_settings, std::string> {
    return hq::status_settings{.slots = std::nullopt, .stale_after_ms = 10'000, .grace_ms = std::nullopt};
  };
  auto const tighter = hq::query_status(conn, fresh, request);
  REQUIRE((tighter.has_value() && tighter->has_value()));
  CHECK((*tighter)->live == std::optional<bool>{false});
}
