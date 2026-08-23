/// @file tree.cpp
/// @brief Implementation of `planar.cmd.planar_agent.tree`.

module planar.cmd.planar_agent.tree;

import std;
import cli11;
import planar.cliapp.args;

namespace planar::cmd::agent {

namespace {

/// @brief The `--json` flag every verb in this binary carries.
///
/// It has NO description, deliberately: the oracle's declaration carries
/// none, so its help line renders bare.
/// @param app The node to declare it on.
auto add_json(CLI::App& app) -> void {
  app.add_flag("--json");
}

/// @brief The `--claim <token>` flag the eight token-addressed verbs carry.
/// @param app The node to declare it on.
/// @param desc The verb's own wording for it — these genuinely differ
/// ("returned by pull/claim", "to refresh", "to force-release"...).
auto add_claim(CLI::App& app, std::string desc) -> void {
  app.add_option("--claim")->description(std::move(desc))->required();
}

/// @brief The closed failure taxonomy shared by `fail`, `abort` and
/// `reconcile` — the same six values in the same order in all three.
///
/// Enforced by CLI11's `IsMember` validator, which also renders the set
/// into the option's type name; `planar.cliapp.schema` reads it back out
/// of there, so the catalog still reports the choice set.
/// @return The choice set.
auto failure_categories() -> std::vector<std::string> {
  return {"usage_limit", "context_limit", "output_limit", "tool_failure", "validation", "unknown"};
}

/// @brief The `--no-locality-probe` flag.
/// @param app The node to declare it on.
/// @param desc The verb's own wording (the terminal verbs mention commit
/// collection; the acquisition verbs do not).
auto add_no_locality_probe(CLI::App& app, std::string desc) -> void {
  app.add_flag("--no-locality-probe")->description(std::move(desc));
}

/// @brief The `--ttl` flag, whose wording differs by one word between the
/// acquisition verbs ("Lease TTL") and `heartbeat` ("New TTL").
///
/// Declared as a plain string with a DEFAULT, not as an integer: the value
/// is a duration (`600`, `10m`, `500ms`) that
/// `planar.cmd.planar_agent.args::parse_ttl_seconds` interprets. The
/// default string is load-bearing beyond help — `planar.cliapp.args::
/// harvest` materializes a declared default into the parsed result, so an
/// omitted `--ttl` still reaches the handler as "600".
/// @param app The node to declare it on.
/// @param desc The verb's own wording.
auto add_ttl(CLI::App& app, std::string desc) -> void {
  app.add_option("--ttl")->description(std::move(desc))->default_str("600");
}

/// @brief The `--vendor` / `--vendor-session` pair.
/// @param app The node to declare them on.
/// @param vendor_desc The verb's wording for `--vendor`.
/// @param session_desc The verb's wording for `--vendor-session`.
auto add_vendor(CLI::App& app, std::string vendor_desc, std::string session_desc) -> void {
  app.add_option("--vendor")->description(std::move(vendor_desc))->default_str("planar-agent");
  app.add_option("--vendor-session")->description(std::move(session_desc));
}

/// @brief The `--run` / `--stage` pair carried by `pull` and `claim`
/// (NOT by `claim-associate`, whose versions are required and worded
/// differently).
/// @param app The node to declare them on.
auto add_run_stage(CLI::App& app) -> void {
  app.add_option("--run")
      ->description("workflow_runs.id to associate with this claim (populated by an external workflow harness; omit "
                    "for interactive claims)")
      ->check(cliapp::zig_int_validator());
  app.add_option("--stage")->description("Workflow stage name (e.g. code, review) to record on the claim; requires --run");
}

} // namespace

auto root_app() -> std::unique_ptr<CLI::App> {
  // Every description below is transcribed from the Zig node. The four
  // verbs this port has not landed — `ingest`, `run`, `dispatch`,
  // `context` — are simply absent (an absent child beats a registered
  // stub), so the root page lists fourteen where the oracle lists
  // eighteen. Declaration ORDER matches the oracle's `handlers/cmd.zig`
  // registry exactly.
  auto app = std::make_unique<CLI::App>("Agent-callable coordination binary (pull / claim / complete / heartbeat / reconcile).",
                                        "planar-agent");
  app->require_subcommand(0);

  app->add_subcommand("version", "Print the planar-agent version, commit, and zig runtime.");

  // --- pull ---------------------------------------------------------------
  CLI::App* pull = app->add_subcommand("pull", "Atomically pick the next eligible task, claim it, and flip status to doing.");
  add_vendor(*pull, "Vendor tag (default: planar-agent)", "Vendor session id (e.g. claude:s1)");
  pull->add_option("--role")->description("Role name (planner|coder|reviewer|test_coder|...)");
  add_ttl(*pull, "Lease TTL (default 600s; accepts bare int seconds or suffixed duration: 10m, 1h, 500ms)");
  pull->add_option("--purpose")->description("Free-text purpose recorded on the claim");
  pull->add_option("--base-ref")->description("Git ref the work is based on");
  pull->add_option("--worktree")->description("Worktree id or path for isolation context");
  pull->add_option("--repo-root")->description("Absolute path of checkout to probe locality against");
  add_no_locality_probe(*pull, "Skip the git locality probe");
  pull->add_option("--metadata")
      ->description("Opaque text (typically JSON) persisted on the dispatch action row; validated as well-formed "
                    "JSON when supplied");
  pull->add_option("--parent-action")
      ->description("Parent action id; wires the new action as a child of this action in `planar-watch tree` "
                    "(cross-session hierarchy)")
      ->check(cliapp::zig_int_validator());
  add_run_stage(*pull);
  add_json(*pull);
  pull->add_option("plan-id")->description("Plan id to pull from")->required()->check(cliapp::zig_int_validator());

  // --- peek ---------------------------------------------------------------
  CLI::App* peek = app->add_subcommand("peek", "Read-only what's-next selector (same query as pull, no writes).");
  add_json(*peek);
  peek->add_option("plan-id")->description("Plan id to peek into")->required()->check(cliapp::zig_int_validator());

  // --- complete / fail / release / block -----------------------------------
  CLI::App* complete =
      app->add_subcommand("complete", "Atomically end the work session: task \xE2\x86\x92 done, claim \xE2\x86\x92 completed.");
  add_claim(*complete, "Claim token returned by pull/claim");
  complete->add_option("--summary")->description("Free-text completion summary recorded on the action");
  add_no_locality_probe(*complete, "Skip the git locality probe and commit collection");
  add_json(*complete);

  CLI::App* fail =
      app->add_subcommand("fail", "Atomically fail the work session: task \xE2\x86\x92 todo, claim \xE2\x86\x92 aborted.");
  add_claim(*fail, "Claim token returned by pull/claim");
  fail->add_option("--reason")->description("Failure reason recorded on the claim and action")->required();
  fail->add_option("--category")
      ->description("Closed failure category (default: unknown)")
      ->check(CLI::IsMember{failure_categories()})
      ->default_str("unknown");
  add_no_locality_probe(*fail, "Skip the git locality probe and commit collection");
  add_json(*fail);

  CLI::App* release = app->add_subcommand(
      "release", "Graceful give-up: task \xE2\x86\x92 todo, claim \xE2\x86\x92 released (vs fail's aborted).");
  add_claim(*release, "Claim token returned by pull/claim");
  release->add_option("--reason")->description("Optional reason for releasing");
  add_no_locality_probe(*release, "Skip the git locality probe and commit collection");
  add_json(*release);

  CLI::App* block = app->add_subcommand("block", "Atomically park the task on an external blocker.");
  add_claim(*block, "Claim token returned by pull/claim");
  block->add_option("--blocker")
      ->description("Task id of the blocker (entity_links target)")
      ->required()
      ->check(cliapp::zig_int_validator());
  block->add_option("--reason")->description("Free-text reason recorded on the claim");
  add_no_locality_probe(*block, "Skip the git locality probe and commit collection");
  add_json(*block);

  // --- claim --------------------------------------------------------------
  CLI::App* claim =
      app->add_subcommand("claim", "Direct entity claim; task claims atomically transition todo to doing by default.");
  claim->add_option("--entity")->description("Entity ref: task:<id> | plan:<id> | plan_step:<id>")->required();
  add_vendor(*claim, "Vendor tag (default: planar-agent)", "Vendor session id (e.g. claude:s1)");
  claim->add_option("--role")->description("Role name (planner|coder|reviewer|test_coder|...)");
  claim->add_option("--model")->description(
      "Model actually used, recorded verbatim as an opaque string. Never validated against a supported list.");
  add_ttl(*claim, "Lease TTL (default 600s; accepts bare int seconds or suffixed duration: 10m, 1h, 500ms)");
  claim->add_option("--purpose")->description("Free-text purpose recorded on the claim");
  claim->add_option("--worktree")->description("Worktree id or path for isolation context");
  claim->add_option("--repo-root")->description("Absolute path of checkout to probe locality against");
  add_no_locality_probe(*claim, "Skip the git locality probe");
  claim->add_flag("--no-transition")->description("Claim without changing task status (plan and plan_step are always unchanged)");
  claim->add_flag("--force")->description("Take over an existing live claim (operator recovery)");
  add_run_stage(*claim);
  add_json(*claim);

  // --- heartbeat ----------------------------------------------------------
  CLI::App* heartbeat = app->add_subcommand("heartbeat", "Refresh the lease on an active claim.");
  add_claim(*heartbeat, "Claim token to refresh");
  add_ttl(*heartbeat, "New TTL (default 600s; accepts bare int seconds or suffixed duration: 10m, 1h, 500ms)");
  heartbeat->add_option("--status")->description("Free-text status string recorded on the heartbeat action row's summary column");
  add_json(*heartbeat);

  // --- claim-associate ----------------------------------------------------
  CLI::App* claim_associate =
      app->add_subcommand("claim-associate", "Associate a pre-acquired active claim with a workflow run (and optional stage). "
                                             "Used by an external workflow harness at dispatch time.");
  add_claim(*claim_associate, "Claim token to associate");
  claim_associate->add_option("--run")
      ->description("workflow_runs.id to stamp on the claim")
      ->required()
      ->check(cliapp::zig_int_validator());
  claim_associate->add_option("--stage")->description("Stage name to record (e.g. code, review); omit for NULL");
  add_json(*claim_associate);

  // --- action start / end -------------------------------------------------
  CLI::App* action = app->add_subcommand("action", "Nested action lifecycle (sub-tool-calls inside a claim).");
  action->require_subcommand(0);
  CLI::App* action_start = action->add_subcommand("start", "Start a nested action under a claim (child of the claim's role "
                                                           "action).");
  add_claim(*action_start, "Claim token the action attaches to");
  action_start->add_option("--kind")->description("Action kind (planner|coder|tool_call|heartbeat|...)")->required();
  action_start->add_option("--entity")->description("Optional entity ref kind:id");
  action_start->add_option("--vendor-role")->description("Optional vendor role tag");
  action_start->add_option("--repo-root")->description("Absolute path of checkout to probe locality against");
  add_no_locality_probe(*action_start, "Skip the git locality probe");
  action_start->add_option("--metadata")
      ->description("Opaque text (typically JSON) persisted on the action row; validated as well-formed JSON when supplied");
  add_json(*action_start);

  CLI::App* action_end = action->add_subcommand("end", "Close a nested action started under a claim.");
  action_end->add_option("--action")
      ->description("Action id returned by `action start`")
      ->required()
      ->check(cliapp::zig_int_validator());
  // A plain string with a hand-rolled validator in the handler, NOT a
  // choice — the oracle's declaration. Making it a choice would add the
  // value set to the help page and to the schema catalog.
  action_end->add_option("--outcome")->description("ok | error | aborted | timeout (default ok)")->default_str("ok");
  action_end->add_option("--summary")->description("Optional free-text summary recorded on the action");
  add_json(*action_end);

  // --- reconcile / abort --------------------------------------------------
  CLI::App* reconcile =
      app->add_subcommand("reconcile", "Operator recovery: mark expired claims stale, close orphaned actions, abandon dead "
                                       "runs.");
  reconcile->add_flag("--dry-run")->description("Report candidates without writing");
  reconcile->add_option("--stale-after")
      ->description("Additional grace beyond lease expiry (default 0s; accepts bare int seconds or suffixed "
                    "duration: 10m, 1h, 500ms)")
      ->default_str("0");
  // Sentinel-zero default, where `--plan` right below is a plain optional.
  // Both mean "global sweep"; the asymmetry is the oracle's.
  reconcile->add_option("--session")
      ->description("Scope the sweep to a single session id (0 = global sweep, the default)")
      ->check(cliapp::zig_int_validator())
      ->default_str("0");
  reconcile->add_option("--plan")
      ->description("Scope the sweep to claims/actions/runs belonging to this plan id (0 or absent = global sweep)")
      ->check(cliapp::zig_int_validator());
  reconcile->add_option("--category")
      ->description("Optional closed failure category applied to claims made stale")
      ->check(CLI::IsMember{failure_categories()});
  add_json(*reconcile);

  CLI::App* abort_cmd =
      app->add_subcommand("abort", "Operator force-release of a stuck claim (any session, not just the owner).");
  add_claim(*abort_cmd, "Claim token to force-release");
  abort_cmd->add_option("--reason")->description("Optional reason recorded on the claim and audit row");
  // No default here, unlike `fail`'s "unknown" — an abort records a
  // category only when the operator names one.
  abort_cmd->add_option("--category")
      ->description("Optional closed failure category for the recovered claim")
      ->check(CLI::IsMember{failure_categories()});
  add_vendor(*abort_cmd, "Vendor tag for the aborting session", "Vendor session id for the aborting session");
  add_json(*abort_cmd);

  app->add_subcommand("schema", "Print the full command tree as a JSON catalog (flags, aliases, positionals).");

  return app;
}

auto forbidden_verbs() -> std::vector<std::string_view> {
  return {"plan", "task",      "decision", "question", "scenario", "artifact", "annotate", "init",      "workbench", "doc",
          "spec", "templates", "ext",      "sync",     "promote",  "demote",   "capture",  "dashboard", "tree",      "health"};
}

} // namespace planar::cmd::agent
