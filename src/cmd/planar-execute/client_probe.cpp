/// @file client_probe.cpp
/// @brief The Centurion client seam: a bounded liveness probe (plan 1033 M2, task 6502).
///
/// This is the ONLY translation unit in Planar that talks to a Centurion
/// daemon, and `centurion::client` is the only Centurion target the build
/// links (`cmake/architecture.cmake` FATALs on any other edge). A plain TU
/// rather than a module unit: the client's value types come from headers that
/// do not compile inside a module purview on the pinned toolchain.
#include "client_probe.hpp"

import std;
import centurion.client;

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

} // namespace planar::cmd::execute
