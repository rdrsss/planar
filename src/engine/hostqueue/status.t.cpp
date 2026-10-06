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
#include <signal.h>

import std;
import planar.db;
import planar.db.migrate;
import planar.process.identity;
import planar.engine.hostqueue;

#include "scratch_store.hpp"

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
  auto opened = open_main_store_at(scratch.path_ / "planar.db");
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

TEST_CASE("wait observer follows the logical ticket to authoritative history on a read-only store",
          "[engine][hostqueue][hq-wait-observer]") {
  scratch_dir scratch;
  auto        conn   = open_scratch_store(scratch);
  auto const  first  = enqueue_ok(conn, request_for("first"));
  auto const  middle = enqueue_ok(conn, request_for("middle"));
  auto const  next   = enqueue_ok(conn, request_for("next"));
  end_abandoned(conn, first);
  end_abandoned(conn, middle);
  REQUIRE(hq::record_successor(conn, first, middle).has_value());
  REQUIRE(hq::record_successor(conn, middle, next).has_value());
  REQUIRE(
      hq::end_entry(conn, next, hq::end_request{.outcome = hq::history_outcome::exited, .exit_code = 125, .ended_at = k_now_wall})
          .has_value());
  auto ro = open_main_store_read_only_at(scratch.path_ / "planar.db");
  REQUIRE(ro.has_value());

  int  checks = 0;
  auto snapshot =
      hq::query_wait_status(*ro, first, status_request(fixed_probe(true)), [&checks]() -> std::expected<void, hq::status_error> {
        ++checks;
        return {};
      });
  REQUIRE(snapshot.has_value());
  REQUIRE(snapshot->status.has_value());
  CHECK(snapshot->issue == hq::wait_lookup_issue::none);
  CHECK(snapshot->observed_seq == next);
  CHECK(snapshot->status->seq == first);
  CHECK(snapshot->status->superseded_by == next);
  CHECK(snapshot->status->outcome == hq::history_outcome::exited);
  CHECK(checks >= 3);
  hq::wait_observer observer;
  auto const        decision = observer.step(*snapshot, k_now_mono);
  CHECK(decision.reason == hq::wait_reason::completed);
  CHECK(decision.result_exit_code == 125);
  CHECK_FALSE(ro->execute("delete from queue_history").has_value());
}

TEST_CASE("wait observer preserves abandoned and missing-successor uncertainty", "[engine][hostqueue][hq-wait-observer]") {
  scratch_dir scratch;
  auto        conn = open_scratch_store(scratch);
  auto const  old  = enqueue_ok(conn, request_for("old"));
  end_abandoned(conn, old);
  hq::wait_observer observer;

  auto pending = hq::query_wait_status(conn, old, status_request(fixed_probe(true)));
  REQUIRE(pending.has_value());
  CHECK(observer.step(*pending, k_now_mono).reason == hq::wait_reason::pending);

  REQUIRE(hq::record_successor(conn, old, old + 50).has_value());
  auto missing = hq::query_wait_status(conn, old, status_request(fixed_probe(true)));
  REQUIRE(missing.has_value());
  CHECK(missing->issue == hq::wait_lookup_issue::successor_history_unavailable);
  auto const unavailable = observer.step(*missing, k_now_mono + 1);
  CHECK(unavailable.reason == hq::wait_reason::history_unavailable);
  CHECK(unavailable.result_exit_code == 1);
  CHECK(unavailable.tag == "successor_history_unavailable");

  auto absent = hq::query_wait_status(conn, old + 100, status_request(fixed_probe(true)));
  REQUIRE(absent.has_value());
  CHECK(observer.step(*absent, k_now_mono + 2).tag == "not_found");
  REQUIRE(hq::record_successor(conn, old, old).has_value());
  auto cyclic = hq::query_wait_status(conn, old, status_request(fixed_probe(true)));
  REQUIRE(cyclic.has_value());
  CHECK(observer.step(*cyclic, k_now_mono + 3).tag == "invalid_successor");
}

TEST_CASE("wait observer confirms the same dead active sequence after one second", "[engine][hostqueue][hq-wait-observer]") {
  scratch_dir scratch;
  auto        conn = open_scratch_store(scratch);
  auto const  seq  = enqueue_ok(conn, request_for("dead"));
  auto        dead = hq::query_wait_status(conn, seq, status_request(fixed_probe(false)));
  REQUIRE(dead.has_value());
  hq::wait_observer observer;
  CHECK(observer.step(*dead, k_now_mono).reason == hq::wait_reason::pending);
  CHECK(observer.step(*dead, k_now_mono + 999).reason == hq::wait_reason::pending);
  CHECK(observer.step(*dead, k_now_mono + 1'000).reason == hq::wait_reason::stalled);

  auto live = hq::query_wait_status(conn, seq, status_request(fixed_probe(true)));
  REQUIRE(live.has_value());
  CHECK(observer.step(*live, k_now_mono + 1'001).reason == hq::wait_reason::pending);
  CHECK(observer.step(*dead, k_now_mono + 1'002).reason == hq::wait_reason::pending);
  CHECK(hq::find(conn, seq).value().has_value());
}

TEST_CASE("wait observer maps every recorded final outcome separately from its stop reason",
          "[engine][hostqueue][hq-wait-observer]") {
  struct scenario {
    hq::history_outcome         outcome;
    std::optional<std::int64_t> exit_code;
    std::optional<std::int64_t> signal;
    hq::wait_reason             reason;
    std::optional<int>          result_exit_code;
  };
  constexpr std::array cases{
      scenario{hq::history_outcome::exited, 124, {}, hq::wait_reason::completed, 124},
      scenario{hq::history_outcome::signaled, {}, 15, hq::wait_reason::completed, 143},
      scenario{hq::history_outcome::timeout, {}, {}, hq::wait_reason::completed, 124},
      scenario{hq::history_outcome::cancelled, {}, {}, hq::wait_reason::completed, 125},
      scenario{hq::history_outcome::wait_timeout, {}, {}, hq::wait_reason::completed, 125},
      scenario{hq::history_outcome::not_started, 127, {}, hq::wait_reason::completed, 127},
      scenario{hq::history_outcome::abandoned, {}, {}, hq::wait_reason::pending, {}},
  };
  for (auto const& item : cases) {
    INFO(hq::to_string(item.outcome));
    hq::queue_status status;
    status.state     = hq::status_state::ended;
    status.outcome   = item.outcome;
    status.exit_code = item.exit_code;
    status.signal    = item.signal;
    hq::wait_observer observer;
    auto const        decision = observer.step(hq::wait_status_lookup{.status = status, .observed_seq = 1}, k_now_mono);
    CHECK(decision.reason == item.reason);
    CHECK(decision.result_exit_code == item.result_exit_code);
  }
}

TEST_CASE("query_status: a read-only connection answers, and refuses every write", "[engine][hostqueue][hq-queue-status]") {
  scratch_dir  scratch;
  std::int64_t seq = 0;
  {
    auto conn = open_scratch_store(scratch);
    seq       = enqueue_ok(conn, request_for("waiting"));
  }
  auto ro = open_main_store_read_only_at(scratch.path_ / "planar.db");
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

TEST_CASE("bounded wait uses one deadline across repeated reads and preserves the last status",
          "[engine][hostqueue][bqw1123-deadlines]") {
  std::int64_t     now   = 100;
  int              reads = 0;
  hq::wait_runtime runtime{
      .now_ms             = [&] { return std::optional{now}; },
      .interrupted_signal = [] { return std::optional<int>{}; },
      .read               = [&](const std::function<std::expected<int, hq::status_error>()>& budget)
          -> std::expected<hq::wait_status_lookup, hq::status_error> {
        auto remaining = budget();
        REQUIRE(remaining.has_value());
        CHECK(*remaining == 2'500 - reads * 1'000);
        ++reads;
        hq::queue_status status;
        status.state = hq::status_state::waiting;
        status.live  = true;
        return hq::wait_status_lookup{.status = status, .observed_seq = 4};
      },
      .sleep =
          [&](std::chrono::milliseconds span) {
            CHECK(span.count() <= 1'000);
            now += span.count();
          },
  };
  auto result = hq::observe_wait(2'500, runtime, now);
  CHECK(result.reason == hq::wait_reason::timed_out);
  CHECK(result.result_exit_code == 124);
  CHECK(result.elapsed_ms == 2'500);
  CHECK(result.snapshot->status->state == hq::status_state::waiting);
  CHECK(reads == 3);
}

TEST_CASE("bounded wait stops on interruption and a busy read that consumes the deadline",
          "[engine][hostqueue][bqw1123-deadlines]") {
  for (bool interrupt : {false, true}) {
    std::int64_t     now   = 10;
    int              reads = 0;
    hq::wait_runtime runtime{
        .now_ms             = [&] { return std::optional{now}; },
        .interrupted_signal = [&] { return now >= 20 && interrupt ? std::optional<int>{15} : std::optional<int>{}; },
        .read               = [&](const std::function<std::expected<int, hq::status_error>()>& budget)
            -> std::expected<hq::wait_status_lookup, hq::status_error> {
          ++reads;
          REQUIRE(budget().has_value());
          now = 20;
          return std::unexpected(hq::status_error{.message = "database is locked"});
        },
        .sleep = [](std::chrono::milliseconds) { FAIL("a failed read must not sleep"); },
    };
    auto result = hq::observe_wait(10, runtime, now);
    CHECK(result.reason == (interrupt ? hq::wait_reason::interrupted : hq::wait_reason::timed_out));
    CHECK(result.result_exit_code == (interrupt ? 143 : 124));
    CHECK(reads == 1);
  }
}

TEST_CASE("bounded wait reports elapsed monotonic time when interrupted after sleep", "[engine][hostqueue][bqw1123-deadlines]") {
  std::int64_t now = 1'000;
  bool interrupted = false;
  hq::wait_runtime runtime{
      .now_ms = [&] { return std::optional{now}; },
      .interrupted_signal = [&] { return interrupted ? std::optional<int>{SIGTERM} : std::nullopt; },
      .read = [](const std::function<std::expected<int, hq::status_error>()>& budget)
          -> std::expected<hq::wait_status_lookup, hq::status_error> {
        REQUIRE(budget().has_value());
        hq::queue_status status;
        status.state = hq::status_state::waiting;
        status.live = true;
        return hq::wait_status_lookup{.status = status, .observed_seq = 4};
      },
      .sleep = [&](std::chrono::milliseconds) {
        now += 500;
        interrupted = true;
      },
  };
  auto result = hq::observe_wait(5'000, runtime, 1'000);
  CHECK(result.reason == hq::wait_reason::interrupted);
  CHECK(result.result_exit_code == 128 + SIGTERM);
  CHECK(result.elapsed_ms == 500);
}

TEST_CASE("bounded wait rejects an invalid clock before reading", "[engine][hostqueue][bqw1123-deadlines]") {
  CHECK(hq::checked_wait_deadline(1, std::numeric_limits<std::int64_t>::max()).error() == "clock_overflow");
  hq::wait_runtime runtime{
      .now_ms             = [] { return std::optional<std::int64_t>{}; },
      .interrupted_signal = [] { return std::optional<int>{}; },
      .read               = [](const std::function<std::expected<int, hq::status_error>()>&)
          -> std::expected<hq::wait_status_lookup, hq::status_error> {
        FAIL("an unavailable clock must stop before reading");
        return {};
      },
      .sleep = [](std::chrono::milliseconds) { FAIL("an unavailable clock must not sleep"); },
  };
  auto result = hq::observe_wait(100, runtime, 42);
  CHECK(result.reason == hq::wait_reason::error);
  CHECK(result.tag == "clock_unavailable");
}
