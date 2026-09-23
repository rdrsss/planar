/// @file runflow.cppm
/// @brief Submit a run, follow it to a terminal state, and decide what the
///        process exit says about it (plan 1033 M2, task 6504; tech-spec D7).
///
/// The client generates the request id, so a submission whose answer is lost
/// can be replayed under the SAME identity and get the same run back rather
/// than starting a second one. Centurion's ledger is what makes that true
/// (its ADR-0005); this module's job is to never break the property by
/// minting a fresh id on retry.
///
/// The daemon calls are injected, so the whole decision sequence — when to
/// replay, when to give up, what exit code an outcome earns — is testable
/// without a daemon.
export module planar.cmd.planar_execute.runflow;

import std;

export namespace planar::cmd::execute {

/// @brief How one call to the daemon ended. Mirrors `client_bridge.hpp`'s
/// `call_outcome`, which the module side cannot import (it is a header).
enum class daemon_outcome : std::uint8_t {
  ok,        ///< The daemon answered.
  retryable, ///< Refused in a way that leaves the same request id replayable.
  refused,   ///< Refused durably; retrying replays this same answer.
  uncertain, ///< The transport broke without an answer; the start may or may not have committed.
};

/// @brief One run as the daemon projects it.
struct run_projection {
  std::string run_id_;      ///< Durable run identifier.
  std::string status_;      ///< Centurion status name.
  std::string result_json_; ///< Terminal result payload, when present.
  std::string error_json_;  ///< Terminal error payload, when present.
  bool        terminal_{};  ///< Whether the run has reached a state it never leaves.
};

/// @brief The answer to one injected call.
struct daemon_answer {
  daemon_outcome outcome_{}; ///< How it ended.
  run_projection run_;       ///< The run, when it answered.
  std::string    message_;   ///< Why, when it did not.
};

/// @brief The daemon calls this flow needs, injected.
struct run_client {
  /// Start `bundle` with `input_json` under `request_id`.
  std::function<daemon_answer(std::string_view bundle, std::string_view input_json, std::string_view request_id)> submit_;
  /// Read one run's current projection.
  std::function<daemon_answer(std::string_view run_id)> fetch_;
  /// Wait between polls.
  std::function<void(std::chrono::milliseconds)> sleep_{};
  /// Now, for the follow deadline.
  std::function<std::chrono::steady_clock::time_point()> now_{};
};

/// @brief What the whole submit-and-follow attempt produced.
enum class flow_result : std::uint8_t {
  completed,     ///< The run completed; its result is the payload.
  failed,        ///< The run reached a terminal state that is not completion.
  retry_later,   ///< The daemon refused in a way that stays replayable — not a failure of the work.
  refused,       ///< The daemon refused durably; replaying gives this same answer.
  never_terminal ///< The follow budget expired with the run still running.
};

/// @brief The outcome of one `submit_and_follow`.
struct flow_outcome {
  flow_result    result_{};   ///< What happened.
  run_projection run_;        ///< The run, when one was started.
  std::string    message_;    ///< Diagnostic for every non-completed result.
  int            attempts_{}; ///< How many submissions were made (a replay is a second attempt).
};

/// @brief Map a flow outcome to this binary's process exit code.
///
/// `completed` is 0 and a run that ended badly is 1, matching the embedded
/// runner's mapping so a caller's branching does not change with the engine.
/// `retry_later` gets its OWN code (75, `EX_TEMPFAIL`): a caller that retries
/// on it is right to, and a caller that treats every non-zero exit as "the
/// work failed" would otherwise record a failure that never happened.
/// @param result The flow outcome.
/// @return The process exit code.
[[nodiscard]] auto exit_code_for(flow_result result) -> int;

/// @brief Generate a fresh durable request id (a UUIDv7-shaped identifier).
///
/// Time-ordered so ids sort by submission, and random in its tail so two
/// clients submitting in the same millisecond do not collide.
/// @param now Submission time.
/// @param entropy A random 64-bit value.
/// @return The identifier text.
[[nodiscard]] auto make_request_id(std::chrono::system_clock::time_point now, std::uint64_t entropy) -> std::string;

/// @brief How many times one submission may be replayed under its request id.
///
/// A BOUND, not a tuning knob. An uncertain answer is replayed because the
/// ledger can resolve it, but a misclassified permanent failure would
/// otherwise replay until the follow budget expired — silently, since the
/// diagnostic is only printed at the end. A live submit against a bundle
/// Centurion could not use did exactly that before this bound existed.
inline constexpr int max_submit_attempts = 3;

/// @brief Submit one run and follow it until it is terminal or the budget expires.
///
/// An UNCERTAIN submission is replayed under the same request id — never a
/// fresh one — because that is the only way to learn whether the first attempt
/// committed. A `retryable` refusal is reported rather than retried here: the
/// caller decides whether to wait, and burying a wait inside this call would
/// hide it from the exit code.
/// @param client The injected daemon calls.
/// @param bundle The bundle to start.
/// @param input_json Canonical JSON input.
/// @param request_id The durable identity for this submission.
/// @param budget How long to follow before giving up.
/// @return The outcome.
[[nodiscard]] auto submit_and_follow(const run_client& client, std::string_view bundle, std::string_view input_json,
                                     std::string_view request_id, std::chrono::milliseconds budget = std::chrono::minutes{30})
    -> flow_outcome;

/// @brief Render what a completed run leaves on stdout.
/// @param run The terminal run.
/// @return The payload: its result JSON, or `{}` when it carried none.
[[nodiscard]] auto result_payload(const run_projection& run) -> std::string;

} // namespace planar::cmd::execute
