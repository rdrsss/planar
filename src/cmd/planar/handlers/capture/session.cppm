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
  add_string(*session, "--vendor", k_undocumented);
  add_string(*session, "--vendor-session-id", k_undocumented);
  add_string(*session, "--model", k_undocumented);
  add_int(*session, "--task", k_undocumented);
  add_json(*session, k_undocumented);
  return session;
}
} // namespace planar::cmd::handlers::capture_cli
