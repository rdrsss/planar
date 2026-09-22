/// @file session.cppm
/// @brief CLI declaration for `audit session`.
export module planar.cmd.planar.handlers.audit.session;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::audit_cli {
export auto attach_session(CLI::App* audit) -> CLI::App* {
  CLI::App* session = audit->add_subcommand("session", "Show the timeline for a session.");
  add_json(*session);
  add_positional(*session, "session-id");
  return session;
}
} // namespace planar::cmd::handlers::audit_cli
