/// @file list.cppm
/// @brief CLI declaration for `annotate list`.
export module planar.cmd.planar.handlers.annotate.list;

import std;
import cli11;
import planar.cmd.planar.declare;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::handlers::annotate_cli {
/// @brief Register the list CLI node.
/// @param annotate Input annotate.
/// @return Registered CLI node.
export auto attach_list(CLI::App& annotate) -> CLI::App* {
  CLI::App* list = annotate.add_subcommand("list", "List annotations.");
  add_string(*list, "--anchor-path", "Restrict to this anchor path");
  add_string(*list, "--anchor-kind", "Restrict to an anchor kind: file, entity");
  add_string(*list, "--target-kind", "Restrict to a target kind: plan, task (needs --target-id)");
  add_int(*list, "--target-id", "Restrict to this target id (needs --target-kind)");
  add_string(*list, "--status", "Restrict to a status: active, resolved, dismissed, archived");
  add_int(*list, "--plan", "Restrict to annotations on this plan id");
  add_int(*list, "--task", "Restrict to annotations on this task id");
  add_string(*list, "--vendor", "Restrict to annotations by this vendor");
  add_string(*list, "--tag", "Restrict to annotations carrying this tag");
  add_string(*list, "--scope", "Restrict to this scope slug");
  add_json(*list, "Emit machine-readable JSON instead of text");
  return list;
}
} // namespace planar::cmd::handlers::annotate_cli
