/// @file packet.cppm
/// @brief CLI declaration for `task packet`.
export module planar.cmd.planar.handlers.task.packet;

import std;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers::task_cli {
export auto attach_packet(CLI::App& task) -> CLI::App* {
  CLI::App* packet = task.add_subcommand("packet", "Compile the authoritative current routing packet for a task.");
  add_json(*packet);
  add_positional(*packet, "task-id");
  return packet;
}
} // namespace planar::cmd::handlers::task_cli
