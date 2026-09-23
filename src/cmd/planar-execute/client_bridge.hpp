/// @file client_bridge.hpp
/// @brief The one declaration the module side needs of the Centurion client
///        (plan 1033 M2, task 6502).
///
/// A HEADER, deliberately, and the only one in this binary. Centurion's
/// generated gRPC/protobuf headers do not compile inside a C++ module unit on
/// the pinned toolchain (its `docs/transport.md` calls that "the invariant
/// that shapes everything"), so the code that talks to a daemon lives in a
/// plain translation unit and the module side reaches it through this
/// declaration — the same shape `src/cmd/parity_harness.hpp` uses.
#pragma once

// This header declares `std::` types but includes NO standard header: it is
// included from translation units that say `import std;` first, and mixing an
// `#include <string>` into them would reintroduce the global-module/std-module
// clash the module build exists to avoid. Include it after the imports.

namespace planar::cmd::execute {

/// @brief Whether a Centurion daemon is accepting on this socket right now.
///
/// A BOUNDED probe: it asks the daemon a real question (Centurion's
/// `probe_bundle_capability`, a bounded `ListWorkflowBundles`, because the
/// owner-only Unix listener routes no health check) and answers false on any
/// transport failure. It never blocks longer than its own deadline, because
/// `ensure_host` calls it in a readiness loop that owns the overall budget.
///
/// @param socket_path Filesystem path of the daemon's Unix socket.
/// @return True when a daemon answered.
[[nodiscard]] auto probe_socket(const char* socket_path) -> bool;

/// @brief How one call to the daemon ended, as the client must treat it.
///
/// The distinction that matters is RETRYABLE versus not. Centurion commits a
/// durable ledger row for every deterministic refusal, so replaying one with
/// the same request id replays the same answer; a transient refusal (a host
/// that is draining, a transport that broke) commits nothing and stays
/// retryable under the same identity. Collapsing the two would either retry
/// something already decided or give up on something that never happened.
enum class call_outcome {
  ok,        ///< The daemon answered.
  retryable, ///< Refused in a way that leaves the same request id replayable.
  refused,   ///< Refused durably; retrying replays this same answer.
  uncertain, ///< The transport broke without an answer; the request may or may not have committed.
};

/// @brief One run as the daemon currently projects it.
struct run_view {
  std::string   run_id_;      ///< Durable run identifier.
  std::string   status_;      ///< Centurion status, e.g. `RUN_STATUS_COMPLETED`.
  std::string   result_json_; ///< Terminal result payload; empty when there is none.
  std::string   error_json_;  ///< Terminal error payload; empty when there is none.
  bool          terminal_{};  ///< Whether the status is one a run never leaves.
  std::uint64_t sequence_{};  ///< Latest committed history sequence; the cursor `cancel` must quote.
};

/// @brief The answer to one call: an outcome, the run when there is one, and why when there is not.
struct call_result {
  call_outcome outcome_{}; ///< How the call ended.
  run_view     run_;       ///< The run, when the daemon answered.
  std::string  message_;   ///< Diagnostic, when it did not.
};

/// @brief Start a bundle run under a caller-generated request id.
/// @param socket_path The daemon's Unix socket.
/// @param bundle_name Bundle to start; the published version is selected.
/// @param input_json Canonical JSON input for the run.
/// @param request_id The durable idempotency key; replaying it replays the start.
/// @return The started run, or why it was refused.
[[nodiscard]] auto submit_bundle_run(const char* socket_path, const char* bundle_name, const char* input_json,
                                     const char* request_id) -> call_result;

/// @brief Ask the daemon to cancel one run, with no console session.
///
/// Centurion authorizes this against the run's admission row (its ADR-0057):
/// the principal that started a bundle run may control it. An empty console
/// session is the request shape that selects that basis, not an omission.
/// @param socket_path The daemon's Unix socket.
/// @param run_id The run to cancel.
/// @param expected_sequence The caller's optimistic cursor over the run's history.
/// @return The run after the control settled, or why it was refused.
[[nodiscard]] auto cancel_run(const char* socket_path, const char* run_id, std::uint64_t expected_sequence) -> call_result;

/// @brief Read one run's current durable projection.
/// @param socket_path The daemon's Unix socket.
/// @param run_id The run to read.
/// @return The run, or why it could not be read.
[[nodiscard]] auto fetch_run(const char* socket_path, const char* run_id) -> call_result;

} // namespace planar::cmd::execute
