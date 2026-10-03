/// @file client_bridge.cpp
/// @brief The engine-less Centurion client seam: every call refuses.
///
/// This build links no Centurion target, so there is no daemon to talk to.
/// Each `client_bridge.hpp` function answers the way a durable refusal does
/// (`call_outcome::refused`: retrying replays the same answer, because a
/// rebuilt binary is the only thing that changes it) and carries one message,
/// `planar-execute was built without the Centurion engine`. The engine verbs
/// check `engine_unavailable_reason()` first and refuse before resolving a
/// profile, so none of them reports a merely absent daemon or writes host
/// state. The Centurion-enabled bridge lives on the dev/centurion-integration
/// branch.
import std;

// AFTER the imports: the header declares std:: types but includes no standard
// header, so the includer's `import std;` is what makes them visible (the same
// contract src/cmd/parity_harness.hpp states).
#include "client_bridge.hpp"

namespace planar::cmd::execute {

namespace {

/// @brief The single refusal every entry point reports.
constexpr const char* engine_absent = "planar-execute was built without the Centurion engine";

/// @brief A durable refusal carrying the build's reason.
auto refused() -> call_result {
  return call_result{.outcome_ = call_outcome::refused, .run_ = {}, .message_ = engine_absent};
}

} // namespace

/// @brief Definition of the engine-availability query; see client_bridge.hpp for the contract.
/// @return Always the refusal text: this build has no engine.
auto engine_unavailable_reason() -> const char* {
  return engine_absent;
}

/// @brief Definition of the liveness probe; see client_bridge.hpp for the contract.
/// @return Always false: no daemon can answer a build without a client.
auto probe_socket(const char* /*socket_path*/) -> bool {
  return false;
}

/// @brief Definition of the bundle-run submission; see client_bridge.hpp for the contract.
/// @return A durable refusal.
auto submit_bundle_run(const char* /*socket_path*/, const char* /*bundle_name*/, const char* /*input_json*/,
                       const char* /*request_id*/) -> call_result {
  return refused();
}

/// @brief Definition of the cursor-based follow; see client_bridge.hpp for the contract.
/// @return A durable refusal; the sink is never invoked.
auto follow_run(const char* /*socket_path*/, const char* /*run_id*/, std::uint64_t /*after_sequence*/,
                const follow_sink& /*sink*/) -> call_result {
  return refused();
}

/// @brief Definition of the console-less cancel; see client_bridge.hpp for the contract.
/// @return A durable refusal.
auto cancel_run(const char* /*socket_path*/, const char* /*run_id*/, std::uint64_t /*expected_sequence*/) -> call_result {
  return refused();
}

/// @brief Definition of the run projection read; see client_bridge.hpp for the contract.
/// @return A durable refusal.
auto fetch_run(const char* /*socket_path*/, const char* /*run_id*/) -> call_result {
  return refused();
}

} // namespace planar::cmd::execute
