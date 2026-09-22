/// @file client_probe.hpp
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

} // namespace planar::cmd::execute
