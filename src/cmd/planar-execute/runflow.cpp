/// @file runflow.cpp
/// @brief Implementation of submit-and-follow (plan 1033 M2, task 6504).
module planar.cmd.planar_execute.runflow;

import std;

namespace planar::cmd::execute {

auto exit_code_for(flow_result result) -> int {
  switch (result) {
  case flow_result::completed:
    return 0;
  case flow_result::retry_later:
    // EX_TEMPFAIL. Its own code because "come back later" is not "the work
    // failed", and a caller that cannot tell them apart records a failure
    // that never happened.
    return 75;
  case flow_result::failed:
  case flow_result::refused:
  case flow_result::never_terminal:
    return 1;
  }
  return 1;
}

auto make_request_id(std::chrono::system_clock::time_point now, std::uint64_t entropy) -> std::string {
  const auto milliseconds =
      static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
  // UUIDv7 layout: 48 bits of big-endian milliseconds, version 7, then
  // randomness. Time-ordered so ids sort by submission.
  const auto high = (milliseconds & 0xFFFFFFFFFFFFULL) << 16U | 0x7000ULL | ((entropy >> 52U) & 0x0FFFULL);
  const auto low  = (0x8000000000000000ULL | (entropy & 0x3FFFFFFFFFFFFFFFULL));
  return std::format("{:08x}-{:04x}-{:04x}-{:04x}-{:012x}", static_cast<std::uint32_t>(high >> 32U),
                     static_cast<std::uint16_t>(high >> 16U), static_cast<std::uint16_t>(high),
                     static_cast<std::uint16_t>(low >> 48U), low & 0xFFFFFFFFFFFFULL);
}

auto result_payload(const run_projection& run) -> std::string {
  return run.result_json_.empty() ? "{}" : run.result_json_;
}

auto submit_and_follow(const run_client& client, std::string_view bundle, std::string_view input_json,
                       std::string_view request_id, std::chrono::milliseconds budget) -> flow_outcome {
  const auto now      = client.now_ ? client.now_ : [] { return std::chrono::steady_clock::now(); };
  const auto rest     = client.sleep_ ? client.sleep_ : [](std::chrono::milliseconds span) { std::this_thread::sleep_for(span); };
  const auto deadline = now() + budget;

  flow_outcome  outcome{};
  daemon_answer answer{};
  // An uncertain submission is REPLAYED under the same id. A fresh id would
  // start a second run for work that may already be running, which is the one
  // mistake the durable request ledger exists to make impossible.
  while (true) {
    ++outcome.attempts_;
    answer = client.submit_(bundle, input_json, request_id);
    if (answer.outcome_ != daemon_outcome::uncertain || now() >= deadline || outcome.attempts_ >= max_submit_attempts) {
      break;
    }
    rest(std::chrono::milliseconds{100});
  }

  switch (answer.outcome_) {
  case daemon_outcome::retryable:
    return flow_outcome{
        .result_ = flow_result::retry_later, .run_ = {}, .message_ = answer.message_, .attempts_ = outcome.attempts_};
  case daemon_outcome::refused:
    return flow_outcome{.result_ = flow_result::refused, .run_ = {}, .message_ = answer.message_, .attempts_ = outcome.attempts_};
  case daemon_outcome::uncertain:
    // Still unknown after every allowed replay, or after the budget expired.
    // Uncertain is not failure: the run may be executing, and the same request
    // id still resolves it later. The attempt count is part of the message
    // because "we asked three times and never learned" is what the operator
    // needs to know.
    return flow_outcome{.result_   = flow_result::retry_later,
                        .run_      = {},
                        .message_  = std::format("{} (after {} attempt(s) under the same request id)",
                                                 answer.message_.empty() ? std::string("the daemon did not answer the submission")
                                                                         : answer.message_,
                                                 outcome.attempts_),
                        .attempts_ = outcome.attempts_};
  case daemon_outcome::ok:
    break;
  }

  auto observed = answer.run_;
  while (!observed.terminal_ && now() < deadline) {
    rest(std::chrono::milliseconds{50});
    auto current = client.fetch_(observed.run_id_);
    if (current.outcome_ == daemon_outcome::ok) {
      observed = current.run_;
      continue;
    }
    // A read that fails does not change what the run is doing; keep following
    // until the budget says otherwise rather than declaring an outcome from a
    // transport hiccup.
    if (current.outcome_ == daemon_outcome::refused) {
      return flow_outcome{
          .result_ = flow_result::refused, .run_ = observed, .message_ = current.message_, .attempts_ = outcome.attempts_};
    }
  }

  if (!observed.terminal_) {
    return flow_outcome{.result_ = flow_result::never_terminal,
                        .run_    = observed,
                        .message_ =
                            std::format("run {} was still {} after {}ms", observed.run_id_, observed.status_, budget.count()),
                        .attempts_ = outcome.attempts_};
  }
  if (observed.status_ == "RUN_STATUS_COMPLETED") {
    return flow_outcome{.result_ = flow_result::completed, .run_ = observed, .message_ = {}, .attempts_ = outcome.attempts_};
  }
  return flow_outcome{.result_ = flow_result::failed,
                      .run_    = observed,
                      .message_ =
                          std::format("run {} ended as {}{}", observed.run_id_, observed.status_,
                                      observed.error_json_.empty() ? std::string{} : std::format(": {}", observed.error_json_)),
                      .attempts_ = outcome.attempts_};
}

} // namespace planar::cmd::execute
