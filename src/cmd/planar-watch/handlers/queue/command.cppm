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
      "queue",
      "Lists every running and waiting entry of the host build and test queue, read-only.\n\n  Entries are in "
      "sequence order, running and waiting alike (POS is the place among the\n  waiting entries). An entry that "
      "fails the liveness rules is marked NOT-LIVE and is left in\n  place: this view never reaps, refreshes or "
      "writes. Nested entries are marked nested:<parent>.\n  A missing planar.db is an error, as for every viewer verb.\n\n  "
      "Text: one line per entry, columns SEQ STATE POS NOTES WAITED RAN VENDOR ROLE LABEL DIRECTORY\n  COMMAND, "
      "the command shell-quoted. --json: an array of objects, one per entry.\n\n  `queue history` lists the entries that "
      "have ended.");
  shared::add_json(*queue);

  // --- queue history --------------------------------------------------
  CLI::App* history = queue->add_subcommand(
      "history",
      "Lists the entries of the host build and test queue that have ended, read-only.\n\n  Oldest first (by end time). Each "
      "row gives the outcome (exited, signaled, timeout, cancelled, wait_timeout,\n  not_started or abandoned), the exit "
      "code or signal, how long it waited and ran, who submitted it, and\n  who cancelled it or which entry replaced "
      "it. --since <duration> keeps only rows that ended within that\n  long (an integer and a unit ms, s, m, h or d, "
      "at most 36500d). A missing planar.db is an error, as for every viewer verb.\n\n  Text: one line per row, columns SEQ "
      "OUTCOME "
      "RESULT ENDED WAITED RAN NOTES VENDOR ROLE LABEL DIRECTORY\n  COMMAND. --json: an array of objects, one per row.");
  history->add_option("--since")->description(
      "Only rows that ended within this long: an integer and a unit (ms, s, m, h, d), at most 36500d");
  shared::add_json(*history);
}
} // namespace planar::cmd::watch::handlers::queue_cli
