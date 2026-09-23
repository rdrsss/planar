/// @file client_bridge.cpp
/// @brief The Centurion client seam: a bounded liveness probe (plan 1033 M2, task 6502).
///
/// This is the ONLY translation unit in Planar that talks to a Centurion
/// daemon, and `centurion::client` is the only Centurion target the build
/// links (`cmake/architecture.cmake` FATALs on any other edge). A plain TU
/// rather than a module unit: the client's value types come from headers that
/// do not compile inside a module purview on the pinned toolchain.
import std;
import centurion.client;

// AFTER the imports: the header declares std:: types but includes no standard
// header, so the includer's `import std;` is what makes them visible (the same
// contract src/cmd/parity_harness.hpp states).
#include "client_bridge.hpp"

namespace planar::cmd::execute {

auto probe_socket(const char* socket_path) -> bool {
  if (socket_path == nullptr || *socket_path == '\0') {
    return false;
  }
  // Short deadline on purpose: `ensure_host` polls this while it owns the
  // overall readiness budget, so a probe that blocked for its own sake would
  // make that budget meaningless.
  const centurion::client::endpoint target{.target_ = std::format("unix:{}", socket_path), .deadline_ = std::chrono::seconds{2}};

  // A REAL question, not a connect: the Unix listener routes no health check,
  // and a daemon whose socket exists but whose services are not armed would
  // pass a connect-only probe and then fail the first real call.
  return centurion::client::probe_bundle_capability(target).has_value();
}

namespace {

/// @brief Whether a run status is one a run never leaves.
auto is_terminal(std::string_view status) -> bool {
  return status == "RUN_STATUS_COMPLETED" || status == "RUN_STATUS_FAILED" || status == "RUN_STATUS_CANCELLED" ||
         status == "RUN_STATUS_NONDETERMINISTIC";
}

/// @brief Project a Centurion snapshot onto the view the module side reads.
auto view_of(const centurion::client::run_snapshot& snapshot) -> run_view {
  return run_view{.run_id_      = snapshot.run_id_,
                  .status_      = snapshot.status_,
                  .result_json_ = snapshot.result_json_.value_or(std::string{}),
                  .error_json_  = snapshot.error_json_.value_or(std::string{}),
                  .terminal_    = is_terminal(snapshot.status_)};
}

/// @brief Classify a client failure into what the caller may do about it.
///
/// Centurion's own rules, not a guess: `conflict` is explicitly "never
/// retryable with the same input", while `aborted`, `unavailable` and
/// `resource_exhausted` leave the request id replayable. A broken transport
/// or an exceeded deadline is UNCERTAIN — the start may or may not have
/// committed — and the only safe move there is to replay the same request id
/// and let the ledger answer.
auto classify(const centurion::client::error& failure) -> call_outcome {
  using centurion::client::error_code;
  switch (failure.code_) {
  case error_code::aborted:
  case error_code::unavailable:
  case error_code::resource_exhausted:
    return call_outcome::retryable;
  case error_code::deadline_exceeded:
  case error_code::cancelled:
    return call_outcome::uncertain;
  case error_code::internal:
    // NOT uncertain. `internal` is Centurion's *unclassified* failure, and it
    // is what a permanently malformed request gets — a live submit against a
    // bundle whose contract the host could not use returned `internal` on
    // every attempt. Treating it as uncertain made the client replay a call
    // that could never succeed, silently, until its follow budget expired.
    // Centurion names everything genuinely replayable (`aborted`,
    // `unavailable`, `resource_exhausted`), so anything else is reported.
    return call_outcome::refused;
  default:
    return call_outcome::refused;
  }
}

} // namespace

auto submit_bundle_run(const char* socket_path, const char* bundle_name, const char* input_json, const char* request_id)
    -> call_result {
  const centurion::client::endpoint target{.target_ = std::format("unix:{}", socket_path), .deadline_ = std::chrono::seconds{30}};
  auto started = centurion::client::start_bundle_run(target, centurion::client::start_bundle_run_input{
                                                                 .bundle_name_    = bundle_name,
                                                                 .bundle_version_ = 0,
                                                                 // The host decides which version is current; a client that
                                                                 // pinned one would keep starting a retired bundle.
                                                                 .use_published_version_ = true,
                                                                 .input_json_            = input_json,
                                                                 .request_id_            = std::string(request_id),
                                                             });
  if (!started) {
    return call_result{.outcome_ = classify(started.error()), .run_ = {}, .message_ = started.error().message_};
  }
  return call_result{.outcome_ = call_outcome::ok, .run_ = view_of(started->run_), .message_ = {}};
}

auto fetch_run(const char* socket_path, const char* run_id) -> call_result {
  const centurion::client::endpoint target{.target_ = std::format("unix:{}", socket_path), .deadline_ = std::chrono::seconds{10}};
  auto                              current = centurion::client::get_run(target, run_id);
  if (!current) {
    return call_result{.outcome_ = classify(current.error()), .run_ = {}, .message_ = current.error().message_};
  }
  return call_result{.outcome_ = call_outcome::ok, .run_ = view_of(*current), .message_ = {}};
}

} // namespace planar::cmd::execute
