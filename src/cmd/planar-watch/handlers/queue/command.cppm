/// @file command.cppm
/// @brief CLI declarations for the planar-watch queue view.
module;
export module planar.cmd.planar_watch.handlers.queue.command;
import std;
import cli11;
import planar.cliapp.args;
import planar.cliapp.surface;
import planar.cmd.planar_watch.handlers.shared.cli;
namespace planar::cmd::watch::handlers::queue_cli {
/// @brief Register this CLI declaration.
/// @param root Input root.
export auto add(CLI::App& root) -> void {
  // --- queue ----------------------------------------------------------
  CLI::App* queue = root.add_subcommand(
      "queue", "Lists every running and waiting entry of the host build and test queue, read-only.\n\n  Running entries come "
               "first, then waiting entries in queue order (POS is the place among the\n  waiting entries). An entry that "
               "fails the liveness rules is marked NOT-LIVE and is left in\n  place: this view never reaps, refreshes or "
               "writes. Nested entries are marked nested:<parent>.\n  A missing agent database is an empty queue.\n\n  "
               "Text: one line per entry, columns SEQ STATE POS NOTES WAITED RAN VENDOR ROLE LABEL DIRECTORY\n  COMMAND, "
               "the command shell-quoted. --json: an array of objects, one per entry.");
  shared::add_json(*queue);
}
} // namespace planar::cmd::watch::handlers::queue_cli
