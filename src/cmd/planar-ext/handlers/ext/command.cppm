/// @file command.cppm
/// @brief CLI declarations for the planar-ext ext family.
module;
export module planar.cmd.planar_ext.handlers.ext.command;
import std;
import cli11;
import planar.cliapp.args;
import planar.cliapp.surface;
namespace planar::cmd::ext::handlers::ext_cli {
namespace {
auto add_json(CLI::App& app) -> void {
  cliapp::add_bool_flag(app, "--json");
}
} // namespace
export auto add(CLI::App& root) -> void {
  CLI::App* ext = root.add_subcommand("ext", "Register and interact with external systems on the operational plane.\n\n"
                                             "  Sub-commands: register, list, test, create, propagate-one, propagate.\n"
                                             "  Currently supported systems: Jira, GitHub Issues.");
  ext->require_subcommand(0);

  CLI::App* reg = ext->add_subcommand("register", "Register an external system.");
  reg->require_subcommand(0);

  CLI::App* jira = reg->add_subcommand("jira", "Register a Jira instance as an external system.");
  jira->add_option("--base-url")->required();
  jira->add_option("--project")->required();
  jira->add_option("--auth-env")->description("Env var name holding the API token")->required();
  add_json(*jira);
  jira->add_option("slug")->required();

  CLI::App* github = reg->add_subcommand("github", "Register a GitHub Issues repository as an external system.");
  github->add_option("--project")->description("GitHub repository owner/repo")->required();
  github->add_option("--auth-env")->description("Env var name holding the token (uses gh-cli if omitted)");
  add_json(*github);
  github->add_option("slug")->required();

  CLI::App* list = ext->add_subcommand("list", "List registered external systems.");
  add_json(*list);

  CLI::App* test = ext->add_subcommand("test", "Test connection to an external system.");
  add_json(*test);
  test->add_option("slug")->required();

  CLI::App* create = ext->add_subcommand("create", "Create an external counterpart for a local entity.");
  create->add_option("--from")->description("Source local entity ref (kind:id)")->required();
  create->add_option("--type")->description("External issue type, e.g. Epic, Story");
  create->add_option("--role")->description("Link role (default: mirror)");
  create->add_option("--sync")->description("Sync direction (default: two-way)");
  create->add_option("--scope");
  add_json(*create);
  create->add_option("system-slug")->required();

  CLI::App* propagate_one =
      ext->add_subcommand("propagate-one", "Render one entity's template, POST the counterpart to the external system, and\n"
                                           "  record the external_links row. Idempotent: if a mirror link already exists\n"
                                           "  for this entity+system pair, the call is a no-op and returns op=skipped.\n\n"
                                           "  --from <kind:id>  Source local entity ref (plan:N or task:N)\n"
                                           "  --strategy        Override GitHub strategy: parent-issue, projects-v2, "
                                           "tracking-issue");
  propagate_one->add_option("--from")->description("Source local entity ref (kind:id, e.g. plan:42 or task:7)")->required();
  propagate_one->add_option("--strategy")->description("Override GitHub strategy: parent-issue, projects-v2, tracking-issue");
  propagate_one->add_option("--sync")->description("Sync direction for created link: read-only, write-back, two-way");
  cliapp::add_bool_flag(*propagate_one, "--dry-run", "Preview without contacting the remote system");
  add_json(*propagate_one);
  propagate_one->add_option("system")->required();

  // `ext propagate` — the github-parent-issue arm plus the generic
  // per-entity tree walk for every other reachable strategy (plan 996, task
  // 6421/6428; the generic loop and `--github-strategy` landed at task
  // 6451). `--github-strategy projects-v2` is still declared (matching the
  // oracle's accepted-value set) but ALWAYS refuses at run time — decision
  // 1001 permanently cut execution of that strategy; see
  // handlers/ext/propagate.cppm's header.
  CLI::App* propagate =
      ext->add_subcommand("propagate", "Propagate a feature (plan + descendants) to an external system.\n\n"
                                       "  A multi-repo GitHub feature (or an explicit --github-strategy projects-v2)\n"
                                       "  refuses explicitly rather than mis-executing.");
  propagate->add_option("--system")->description("External system slug (defaults to first registered system)");
  cliapp::add_bool_flag(*propagate, "--dry-run", "Preview creation plan without contacting the remote system");
  propagate->add_option("--sync")->description("Sync direction for created links: read-only, write-back, two-way");
  propagate->add_option("--github-strategy")
      ->description("Override GitHub strategy: parent-issue, projects-v2 (always refused), tracking-issue (not ported)");
  cliapp::add_bool_flag(*propagate, "--restrategize", "Abandon prior counterparts and re-propagate under a fresh strategy");
  cliapp::add_bool_flag(*propagate, "--yes", "Skip the --restrategize confirmation prompt");
  cliapp::add_bool_flag(*propagate, "--verify-counterparts",
                        "Probe every existing counterpart and report ones missing on the remote");
  cliapp::add_bool_flag(*propagate, "--unlink", "With --verify-counterparts: delete the link row for a missing counterpart");
  cliapp::add_bool_flag(*propagate, "--recreate",
                        "With --verify-counterparts: delete the link row for a missing counterpart so the next "
                        "propagate recreates it");
  propagate->add_option("--scope");
  add_json(*propagate);
  propagate->add_option("plan-id")->required();
}
} // namespace planar::cmd::ext::handlers::ext_cli
