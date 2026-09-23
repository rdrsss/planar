/// @file runflow.t.cpp
/// @brief Submitting, replaying and following a run (plan 1033 M2, task 6504).
///
/// The property that matters most here is the one a green run would never
/// show: an uncertain submission must be replayed under the SAME request id.
/// A fresh id would start a second run for work that may already be running,
/// so every retry case asserts the id the daemon actually saw.

import std;
import planar.cmd.planar_execute.runflow;

#include <catch2/catch_test_macros.hpp>

namespace {

using planar::cmd::execute::daemon_answer;
using planar::cmd::execute::daemon_outcome;
using planar::cmd::execute::exit_code_for;
using planar::cmd::execute::flow_result;
using planar::cmd::execute::make_request_id;
using planar::cmd::execute::result_payload;
using planar::cmd::execute::run_client;
using planar::cmd::execute::run_projection;
using planar::cmd::execute::submit_and_follow;

/// @brief A run projection in one line.
auto projected(std::string_view status, bool terminal, std::string_view result = {}, std::string_view error = {})
    -> run_projection {
  return run_projection{.run_id_      = "run-1",
                        .status_      = std::string(status),
                        .result_json_ = std::string(result),
                        .error_json_  = std::string(error),
                        .terminal_    = terminal};
}

/// @brief A scripted daemon: queued submit answers, then queued fetch answers.
struct scripted_daemon {
  std::vector<daemon_answer>            submits;
  std::vector<daemon_answer>            fetches;
  std::vector<std::string>              submitted_ids; ///< Every request id the daemon was asked with.
  std::size_t                           submit_index = 0;
  std::size_t                           fetch_index  = 0;
  std::chrono::steady_clock::time_point clock{};

  [[nodiscard]] auto client() -> run_client {
    return run_client{
        .submit_ =
            [this](std::string_view, std::string_view, std::string_view request_id) {
              submitted_ids.emplace_back(request_id);
              const auto index = std::min(submit_index++, submits.empty() ? 0 : submits.size() - 1);
              return submits.empty() ? daemon_answer{} : submits[index];
            },
        .fetch_ =
            [this](std::string_view) {
              const auto index = std::min(fetch_index++, fetches.empty() ? 0 : fetches.size() - 1);
              return fetches.empty() ? daemon_answer{} : fetches[index];
            },
        .sleep_ = [this](std::chrono::milliseconds span) { clock += span; },
        .now_   = [this] { return clock; },
    };
  }
};

constexpr std::string_view request_id = "01930000-0000-7000-8000-00000000000a";

} // namespace

TEST_CASE("a run that completes leaves its result on stdout and exits zero", "[execute][runflow]") {
  scripted_daemon daemon;
  daemon.submits = {daemon_answer{.outcome_ = daemon_outcome::ok, .run_ = projected("RUN_STATUS_RUNNING", false)}};
  daemon.fetches = {
      daemon_answer{.outcome_ = daemon_outcome::ok, .run_ = projected("RUN_STATUS_COMPLETED", true, R"({"ok":true})")}};

  const auto outcome = submit_and_follow(daemon.client(), "planar.supervision", "{}", request_id);
  CHECK(outcome.result_ == flow_result::completed);
  CHECK(result_payload(outcome.run_) == R"({"ok":true})");
  CHECK(exit_code_for(outcome.result_) == 0);
  CHECK(outcome.attempts_ == 1);
}

TEST_CASE("a completed run with no result still renders an object", "[execute][runflow]") {
  CHECK(result_payload(projected("RUN_STATUS_COMPLETED", true)) == "{}");
}

TEST_CASE("an uncertain submission is replayed under the same request id", "[execute][runflow]") {
  // The whole point of a caller-generated id. A fresh one would start a
  // second run for work that may already be running.
  scripted_daemon daemon;
  daemon.submits = {
      daemon_answer{.outcome_ = daemon_outcome::uncertain, .message_ = "transport closed"},
      daemon_answer{.outcome_ = daemon_outcome::ok, .run_ = projected("RUN_STATUS_COMPLETED", true, R"({"replayed":true})")}};

  const auto outcome = submit_and_follow(daemon.client(), "planar.supervision", "{}", request_id);
  CHECK(outcome.result_ == flow_result::completed);
  CHECK(outcome.attempts_ == 2);
  REQUIRE(daemon.submitted_ids.size() == 2);
  CHECK(daemon.submitted_ids[0] == request_id);
  CHECK(daemon.submitted_ids[1] == request_id); // the SAME id, not a fresh one
}

TEST_CASE("an endlessly uncertain submission stops replaying and says so", "[execute][runflow]") {
  // Found live, not by unit test: a permanently failing submit classified as
  // uncertain replayed silently until the follow budget expired. The bound is
  // what turns that into a diagnostic a caller can read.
  scripted_daemon daemon;
  daemon.submits = {daemon_answer{.outcome_ = daemon_outcome::uncertain, .message_ = "contract is unusable"}};

  const auto outcome = submit_and_follow(daemon.client(), "planar.supervision", "{}", request_id);
  CHECK(outcome.result_ == flow_result::retry_later);
  CHECK(outcome.attempts_ == planar::cmd::execute::max_submit_attempts);
  CHECK(outcome.message_.contains("contract is unusable"));
  CHECK(outcome.message_.contains("attempt"));
  // Every attempt reused the one request id.
  for (const auto& seen : daemon.submitted_ids) {
    CHECK(seen == request_id);
  }
}

TEST_CASE("a retryable refusal is its own outcome, not a failure of the work", "[execute][runflow]") {
  scripted_daemon daemon;
  daemon.submits = {daemon_answer{.outcome_ = daemon_outcome::retryable, .message_ = "host is draining"}};

  const auto outcome = submit_and_follow(daemon.client(), "planar.supervision", "{}", request_id);
  CHECK(outcome.result_ == flow_result::retry_later);
  CHECK(outcome.message_ == "host is draining");
  // A distinct exit code: a caller that treats every non-zero exit as "the
  // work failed" would otherwise record a failure that never happened.
  CHECK(exit_code_for(outcome.result_) == 75);
  CHECK(exit_code_for(flow_result::failed) == 1);
  // Reported rather than retried here: the caller decides whether to wait.
  CHECK(outcome.attempts_ == 1);
}

TEST_CASE("a durable refusal is not replayed", "[execute][runflow]") {
  // Centurion commits a ledger row for a deterministic refusal, so replaying
  // it returns the same answer. Retrying would be pure noise.
  scripted_daemon daemon;
  daemon.submits = {daemon_answer{.outcome_ = daemon_outcome::refused, .message_ = "bundle not found"}};

  const auto outcome = submit_and_follow(daemon.client(), "planar.supervision", "{}", request_id);
  CHECK(outcome.result_ == flow_result::refused);
  CHECK(outcome.attempts_ == 1);
  CHECK(exit_code_for(outcome.result_) == 1);
}

TEST_CASE("a run that ends badly reports its status and error", "[execute][runflow]") {
  scripted_daemon daemon;
  daemon.submits = {daemon_answer{.outcome_ = daemon_outcome::ok, .run_ = projected("RUN_STATUS_RUNNING", false)}};
  daemon.fetches = {daemon_answer{.outcome_ = daemon_outcome::ok,
                                  .run_     = projected("RUN_STATUS_FAILED", true, {}, R"({"reason":"no_evidence"})")}};

  const auto outcome = submit_and_follow(daemon.client(), "planar.supervision", "{}", request_id);
  CHECK(outcome.result_ == flow_result::failed);
  CHECK(outcome.message_.contains("RUN_STATUS_FAILED"));
  CHECK(outcome.message_.contains("no_evidence"));
  CHECK(exit_code_for(outcome.result_) == 1);
}

TEST_CASE("a transient read failure does not end the follow", "[execute][runflow]") {
  // A read that fails says nothing about what the run is doing. Declaring an
  // outcome from a transport hiccup would report a failure the run never had.
  scripted_daemon daemon;
  daemon.submits = {daemon_answer{.outcome_ = daemon_outcome::ok, .run_ = projected("RUN_STATUS_RUNNING", false)}};
  daemon.fetches = {
      daemon_answer{.outcome_ = daemon_outcome::retryable, .message_ = "unavailable"},
      daemon_answer{.outcome_ = daemon_outcome::uncertain, .message_ = "deadline"},
      daemon_answer{.outcome_ = daemon_outcome::ok, .run_ = projected("RUN_STATUS_COMPLETED", true, R"({"late":true})")}};

  const auto outcome = submit_and_follow(daemon.client(), "planar.supervision", "{}", request_id);
  CHECK(outcome.result_ == flow_result::completed);
  CHECK(result_payload(outcome.run_) == R"({"late":true})");
}

TEST_CASE("a run still going when the budget expires is not called failed", "[execute][runflow]") {
  scripted_daemon daemon;
  daemon.submits = {daemon_answer{.outcome_ = daemon_outcome::ok, .run_ = projected("RUN_STATUS_RUNNING", false)}};
  daemon.fetches = {daemon_answer{.outcome_ = daemon_outcome::ok, .run_ = projected("RUN_STATUS_RUNNING", false)}};

  const auto outcome = submit_and_follow(daemon.client(), "planar.supervision", "{}", request_id, std::chrono::milliseconds{200});
  CHECK(outcome.result_ == flow_result::never_terminal);
  CHECK(outcome.message_.contains("RUN_STATUS_RUNNING"));
  CHECK(outcome.run_.run_id_ == "run-1");
}

TEST_CASE("a follow cursor survives the client that wrote it", "[execute][runflow][cursor]") {
  using planar::cmd::execute::cursor_path;
  using planar::cmd::execute::read_cursor;
  using planar::cmd::execute::write_cursor;

  const auto root = std::filesystem::temp_directory_path() /
                    std::format("planar-cursor-{}", std::chrono::steady_clock::now().time_since_epoch().count());
  std::filesystem::create_directories(root);
  const auto path = cursor_path(root, "01930000-0000-7000-8000-00000000000a");

  SECTION("an unseen run resumes from the beginning") {
    // 0 means "replay everything". Re-reading committed history is free;
    // guessing forward would skip events the caller never saw.
    CHECK(read_cursor(path) == 0);
  }

  SECTION("what was accepted is what is resumed after") {
    REQUIRE(write_cursor(path, 7).has_value());
    CHECK(read_cursor(path) == 7);
    REQUIRE(write_cursor(path, 12).has_value());
    CHECK(read_cursor(path) == 12);
  }

  SECTION("a damaged cursor replays rather than skipping") {
    // The safe direction, in both damaged shapes. Non-numeric content reads as
    // 0 (a failed extraction writes 0), and digits followed by garbage read
    // the digits — a SMALLER cursor than was written, which replays. Neither
    // can produce a larger one, which is the only value that would skip events
    // the daemon never re-delivers.
    REQUIRE(write_cursor(path, 9).has_value());
    std::ofstream(path, std::ios::binary | std::ios::trunc) << "not-a-number";
    CHECK(read_cursor(path) == 0);

    std::ofstream(path, std::ios::binary | std::ios::trunc) << "4garbage";
    CHECK(read_cursor(path) == 4);
  }

  SECTION("an event is delivered even when its cursor cannot be advanced") {
    // The ordering claim, made falsifiable. If the cursor were written first,
    // a failure there would swallow the event — and the daemon never
    // re-delivers it. So the failure must arrive AFTER the event is out.
    using planar::cmd::execute::accept_follow_event;
    const auto blocked = root / "cursors";
    std::filesystem::create_directories(root);
    // A FILE where the cursor directory must be: creating the directory fails.
    std::filesystem::remove_all(blocked);
    std::ofstream(blocked) << "not a directory";

    std::ostringstream out;
    auto               accepted =
        accept_follow_event(out, blocked / "run.cursor", 3, "workflow.completed", "RUN_STATUS_COMPLETED", R"({"ok":true})");
    CHECK_FALSE(accepted.has_value());
    // The event still reached the caller.
    CHECK(out.str().contains(R"("sequence":3)"));
    CHECK(out.str().contains("workflow.completed"));
    std::filesystem::remove(blocked);
  }

  SECTION("a delivered event advances the cursor to its sequence") {
    using planar::cmd::execute::accept_follow_event;
    std::ostringstream out;
    REQUIRE(accept_follow_event(out, path, 5, "workflow.running", "RUN_STATUS_RUNNING", {}).has_value());
    CHECK(read_cursor(path) == 5);
    CHECK(out.str().contains(R"("status":"RUN_STATUS_RUNNING")"));
    // An absent payload renders as null rather than as empty text.
    CHECK(out.str().contains(R"("payload":null)"));
  }

  SECTION("a run id that is not a filename cannot escape the cursor directory") {
    const auto escaped = cursor_path(root, "../../etc/passwd");
    CHECK(escaped.parent_path() == root / "cursors");
    CHECK(escaped.filename().string().find('/') == std::string::npos);
  }

  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
}

TEST_CASE("a generated request id is UUIDv7-shaped and time-ordered", "[execute][runflow]") {
  const auto base  = std::chrono::system_clock::time_point{std::chrono::milliseconds{1'790'000'000'000}};
  const auto first = make_request_id(base, 0x0123456789abcdefULL);

  CHECK(first.size() == 36);
  CHECK(first[14] == '7');                                                               // version nibble
  CHECK((first[19] == '8' || first[19] == '9' || first[19] == 'a' || first[19] == 'b')); // variant
  CHECK(make_request_id(base, 0x0123456789abcdefULL) == first);

  SECTION("a later submission sorts after an earlier one") {
    const auto later = make_request_id(base + std::chrono::seconds{1}, 0x0123456789abcdefULL);
    CHECK(later > first);
  }

  SECTION("two clients in the same millisecond do not collide") {
    CHECK(make_request_id(base, 0xfedcba9876543210ULL) != first);
  }
}
