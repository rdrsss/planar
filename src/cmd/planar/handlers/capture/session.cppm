/// @file session.cppm
/// @brief CLI declaration for `capture session`.
export module planar.cmd.planar.handlers.capture.session;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::capture_cli {
export auto attach_session(CLI::App* capture) -> CLI::App* {
  CLI::App* session =
      capture->add_subcommand("session", "Open or reuse a session for the current (vendor, vendor-session-id) tuple.");
  add_string(*session, "--vendor");
  add_string(*session, "--vendor-session-id");
  add_string(*session, "--model");
  add_int(*session, "--task");
  add_json(*session);
  return session;
}
} // namespace planar::cmd::handlers::capture_cli
