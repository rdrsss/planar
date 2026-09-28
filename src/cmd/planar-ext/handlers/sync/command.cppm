/// @file command.cppm
/// @brief CLI declarations for the planar-ext sync family.
module;
export module planar.cmd.planar_ext.handlers.sync.command;
import std;
import cli11;
import planar.cliapp.args;
import planar.cliapp.surface;
namespace planar::cmd::ext::handlers::sync_cli {
namespace {
auto add_json(CLI::App& app) -> void {
  cliapp::add_bool_flag(app, "--json");
}
} // namespace
/// @brief Register this CLI declaration.
/// @param root Input root.
export auto add(CLI::App& root) -> void {
  CLI::App* sync = root.add_subcommand("sync", "Pull, push, and reconcile drift between local entities and their\n"
                                               "  registered external counterparts.");
  sync->require_subcommand(0);

  CLI::App* pull = sync->add_subcommand("pull", "Pull remote state for one or more external links.");
  cliapp::add_bool_flag(*pull, "--all");
  pull->add_option("--system");
  pull->add_option("--scope");
  add_json(*pull);
  pull->add_option("ref")->description("<link-id | kind:id>");

  CLI::App* push = sync->add_subcommand("push", "Push local changes for one or more external links.");
  cliapp::add_bool_flag(*push, "--all");
  push->add_option("--system");
  push->add_option("--scope");
  add_json(*push);
  push->add_option("ref")->description("<link-id | kind:id>");

  CLI::App* status = sync->add_subcommand("status", "Report sync status for links.");
  status->add_option("--entity")->description("Filter by entity, e.g. task:42");
  status->add_option("--system")->description("Filter by system slug");
  add_json(*status);

  CLI::App* resolve = sync->add_subcommand("resolve", "Settle a sync conflict on a link.");
  resolve->add_option("--keep")->description("Which side to keep (local|remote)")->required();
  resolve->add_option("--evidence-token")->description("Exact token from the approved conflict evidence")->required();
  resolve->add_option("--expected-local-updated-at")->description("Approved local entity updated_at version")->required();
  resolve->add_option("--scope");
  add_json(*resolve);
  resolve->add_option("event-id")->required();
}
} // namespace planar::cmd::ext::handlers::sync_cli
