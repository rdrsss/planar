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
  add_int(*commits, "--session", "Record into this session, ended ones allowed (default: the active session)");
  add_string(*commits, "--repo", "Directory of the repo to read commits from (default: .)");
  add_string(*commits, "--since", "Record every commit in <ref>..HEAD; exclusive with positional SHAs");
  add_json(*commits, "Emit machine-readable JSON instead of text");
  // Hidden variadic "rest" positional -- see `declare_capture`'s header
  // in capture.cppm.
  commits->add_option("shas")->expected(0, -1)->group("");
  return commits;
}
} // namespace planar::cmd::handlers::capture_cli
