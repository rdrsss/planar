/// @file command.cppm
/// @brief CLI declaration for `planar-watch diagnose`.
module;
export module planar.cmd.planar_watch.handlers.diagnose.command;
import std;
import cli11;
import planar.cliapp.args;
import planar.cliapp.surface;
import planar.cmd.planar_watch.handlers.shared.cli;
namespace planar::cmd::watch::handlers::diagnose_cli {
/// @brief Register this CLI declaration.
/// @param root Input root.
export auto add(CLI::App& root) -> void {
  // --- diagnose -------------------------------------------------------
  CLI::App* diagnose = root.add_subcommand(
      "diagnose",
      "Evaluates recorded orchestration state against Planar's documented orchestration rules, read-only.\n\n  --plan scopes "
      "the run to a plan, its descendant plans and the tasks attached to it, over the plan's lifetime\n  (its creation to "
      "now). --days overrides that window. Without --plan the window is --days, default 7.\n  --check <id> (repeatable) "
      "selects checks; the default is every check.\n\n  Text: a header (scope, window, outcome), one line per input that "
      "was unavailable or disabled, then one line per\n  finding, `<severity> <check-id> <entity> -> <recovery>`, ordered "
      "by severity, check id, entity and earliest\n  evidence time. --json: one `planar.diagnose/1` object.\n\n  The outcome "
      "is `ok`, or `partial` when an input a selected check needs could not be read (no finding then\n  does not mean "
      "clean). Findings never change the exit status: 0 whenever the run completed. A database that\n  stayed locked past "
      "250 ms, failed a query or lacks the planning tables prints `diagnose: unavailable "
      "(<reason>)`\n  and exits 1. Bad input (an unknown check or plan, --days below 1, an unknown flag) exits 2.");
  shared::add_int(*diagnose, "--plan", "Scope the run to this plan id, its descendant plans and their tasks");
  shared::add_int(*diagnose, "--days", "Window in days; overrides the plan-lifetime window (default 7 without --plan)");
  diagnose->add_option("--check")->description("Run only this check id; repeatable (default: every check)")->expected(1, -1);
  shared::add_json(*diagnose, "Emit machine-readable JSON (planar.diagnose/1) instead of text");
}
} // namespace planar::cmd::watch::handlers::diagnose_cli
