/// @file session.cppm
/// @brief CLI declaration for `capture session`.
export module planar.cmd.planar.handlers.capture.session;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::capture_cli {
/// @brief Register the session CLI node.
/// @param capture Input capture.
/// @return Registered CLI node.
export auto attach_session(CLI::App* capture) -> CLI::App* {
  CLI::App* session =
      capture->add_subcommand("session", "Open or reuse a session for the current (vendor, vendor-session-id) tuple.");
  add_string(*session, "--vendor", "Vendor identity (default: $PLANAR_VENDOR, else cli)");
  add_string(*session, "--vendor-session-id", "Vendor-specific session id (default: $PLANAR_VENDOR_SESSION_ID)");
  add_string(*session, "--model", "Free-form model identifier for the session");
  add_int(*session, "--task", "Task id to associate with the session");
  add_json(*session, "Emit machine-readable JSON instead of text");
  return session;
}
} // namespace planar::cmd::handlers::capture_cli
