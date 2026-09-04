/// @file tree.cpp
/// @brief Implementation of `planar.cmd.planar_ext.tree`.

module planar.cmd.planar_ext.tree;

import std;
import cli11;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::ext {

namespace {

/// @brief The `--json` flag most leaves here carry.
/// @param app The node to declare it on.
auto add_json(CLI::App& app) -> void {
  cliapp::add_bool_flag(app, "--json");
}

/// @brief `planar ext register` and its two children — hand-transcribed
/// verbatim from `planar.cmd.planar.tree`'s `add_ext` at plan 996, task
/// 6419 (the move). See that function's header for the fallthrough()
/// hazard this redeclare-per-child shape avoids.
/// @param root The root app to attach `ext` to.
auto add_ext(CLI::App& root) -> void {
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

  // `ext propagate` — the github-parent-issue arm only (plan 996, task
  // 6421). See handlers/propagate.cppm for exactly which flags the oracle
  // declares that this binary does not yet accept
  // (--restrategize/--yes/--verify-counterparts/--unlink/--recreate/
  // --github-strategy all need the strategy-stickiness cache, which is not
  // ported).
  CLI::App* propagate =
      ext->add_subcommand("propagate", "Propagate a feature (plan + descendants) to an external system.\n\n"
                                       "  This cycle supports the GitHub parent-issue strategy only; a Jira system\n"
                                       "  or a multi-repo GitHub feature refuses explicitly rather than mis-executing.");
  propagate->add_option("--system")->description("External system slug (defaults to first registered system)");
  cliapp::add_bool_flag(*propagate, "--dry-run", "Preview creation plan without contacting the remote system");
  propagate->add_option("--sync")->description("Sync direction for created links: read-only, write-back, two-way");
  add_json(*propagate);
  propagate->add_option("plan-id")->required();
}

/// @brief `planar sync pull|push|status|resolve` — hand-transcribed from
/// `planar.cmd.planar.surface`'s generated `k_path_170..173` nodes at plan
/// 996, task 6419 (the move). See `planar.cmd.planar_ext.handlers.sync`'s
/// header for why `pull`/`push` diverge in exit shape from `resolve`, and
/// why `status` shares nothing with its three siblings but
/// `parse_kind_id_ref`.
/// @param root The root app to attach `sync` to.
auto add_sync(CLI::App& root) -> void {
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

} // namespace

auto root_app() -> std::unique_ptr<CLI::App> {
  // Task 6419 moved the `ext`/`sync` verb family in; this description no
  // longer describes a skeleton. Task 6421 landed `ext propagate` itself —
  // the GitHub parent-issue arm only; see handlers/propagate.cppm for what
  // still refuses explicitly (a Jira system, a multi-repo GitHub feature,
  // and every `--restrategize`-family flag).
  auto app = std::make_unique<CLI::App>("Host of the external-plane propagation and sync verbs (`ext`, `sync`),\n"
                                        "  moved off `planar` at plan 996, task 6419.\n"
                                        "\n"
                                        "  This binary's connection is restricted at the SQLite authorizer level\n"
                                        "  to read-write on exactly external_links, external_systems, and\n"
                                        "  sync_events \xe2\x80\x94 every other table is read-only through this\n"
                                        "  process, enforced by the driver rather than by convention.",
                                        "planar-ext");

  // A bare `planar-ext` renders root help rather than failing.
  app->require_subcommand(0);

  app->add_subcommand("version", "Print the planar-ext version, commit, and compiler.");
  app->add_subcommand("schema", "Print the full command tree as a JSON catalog (flags, aliases, positionals).");
  add_ext(*app);
  add_sync(*app);

  // Help renders the same page it rendered before every bool flag gained
  // its `--no-X` negation.
  cliapp::hide_negations_in_help(*app);
  return app;
}

} // namespace planar::cmd::ext
