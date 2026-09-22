/// @file add.cppm
/// @brief CLI declaration for `annotate add`.
export module planar.cmd.planar.handlers.annotate.add;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::annotate_cli {
export auto attach_add(CLI::App& annotate) -> CLI::App* {
  CLI::App* add = annotate.add_subcommand("add", "Create a new annotation.");
  add_string(*add, "--anchor-path");
  add_int(*add, "--line-start");
  add_int(*add, "--line-end");
  add_string(*add, "--commit-sha");
  add_string(*add, "--text-hash");
  add_string(*add, "--text");
  add_string(*add, "--title");
  add_string(*add, "--slug");
  add_string(*add, "--body");
  add_string(*add, "--vendor");
  add_int(*add, "--plan");
  add_int(*add, "--task");
  add_string(*add, "--tags");
  add_string(*add, "--scope");
  add_json(*add);
  return add;
}
} // namespace planar::cmd::handlers::annotate_cli
