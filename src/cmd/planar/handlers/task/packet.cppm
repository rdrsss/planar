/// @file packet.cppm
/// @brief CLI declaration for `task packet`.
export module planar.cmd.planar.handlers.task.packet;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::task_cli {
/// @brief Register the packet CLI node.
/// @param task Input task.
/// @return Registered CLI node.
export auto attach_packet(CLI::App& task) -> CLI::App* {
  CLI::App* packet = task.add_subcommand(
      "packet", "Compile the authoritative current task packet for a task.\n\n  The packet lists each artifact the task cites "
                "as a reference "
                "(ref, digest, label)\n  and carries no cited text. `--expand <ref>` prints one referenced artifact, or one\n  "
                "section of it with `artifact:<id>#<heading>`, and nothing else.");
  add_json(*packet, "Emit machine-readable JSON instead of text");
  add_string(*packet, "--expand",
             "Print one cited artifact (artifact:<id>) or one section of it (artifact:<id>#<heading>) instead of the packet");
  add_positional(*packet, "task-id", "Task id");
  return packet;
}
} // namespace planar::cmd::handlers::task_cli
