/// @file commits.cppm
/// @brief CLI declaration for `capture commits`.
export module planar.cmd.planar.handlers.capture.commits;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::capture_cli {
/// @brief Register the commits CLI node.
/// @param capture Input capture.
/// @return Registered CLI node.
export auto attach_commits(CLI::App* capture) -> CLI::App* {
  CLI::App* commits = capture->add_subcommand("commits", "Record explicit git commits into a session.");
  add_int(*commits, "--session");
  add_string(*commits, "--repo");
  add_string(*commits, "--since");
  add_json(*commits);
  // Hidden variadic "rest" positional -- see `declare_capture`'s header
  // in capture.cppm.
  commits->add_option("shas")->expected(0, -1)->group("");
  return commits;
}
} // namespace planar::cmd::handlers::capture_cli
