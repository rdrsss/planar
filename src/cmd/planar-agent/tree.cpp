/// @file tree.cpp
/// @brief Implementation of `planar.cmd.planar_agent.tree`.

module planar.cmd.planar_agent.tree;

import std;
import cli11;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::agent {

namespace {

/// @brief The `--json` flag every verb in this binary carries.
///
/// It has NO description, deliberately: the oracle's declaration carries
/// none, so its help line renders bare.
/// @param app The node to declare it on.
auto add_json(CLI::App& app) -> void {
  cliapp::add_bool_flag(app, "--json");
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
  cliapp::add_bool_flag(app, "--no-locality-probe", desc);
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

/// @brief The engine-supervision flags (plan 1033 D3/D4, task 6488).
///
/// `--as engine --attempt <id>` is how the engine supervisor identifies
/// itself on a claim it has been handed with `claim-associate --supervisor
/// engine`. Absent, the verb acts as the caller, exactly as before plan
/// 1033. `--override-supervisor` (terminal verbs only) is operator recovery:
/// a caller terminal verb on an engine claim, logged as a
/// `supervisor_override` action.
/// @param app The verb.
/// @param terminal Whether to add `--override-supervisor`.
auto add_supervision(CLI::App& app, bool terminal) -> void {
  app.add_option("--as")
      ->description("Who is acting: caller (default) or engine (the supervisor of an engine-associated claim; needs --attempt)")
      ->check(CLI::IsMember{std::vector<std::string>{"caller", "engine"}});
  app.add_option("--attempt")
      ->description("The Centurion attempt the engine acts for; must match the claim's associated attempt");
  if (terminal) {
    cliapp::add_bool_flag(
        app, "--override-supervisor",
        "Operator recovery: let the caller terminate an engine-supervised claim (logged as supervisor_override)");
  }
}

/// @brief The `heartbeat` verb's `--ttl`, declared WITHOUT a default.
///
/// Deliberately not `add_ttl`. That helper's `default_str("600")` is
/// load-bearing (see above): `harvest` materializes it, so an omitted
/// `--ttl` would reach the handler as "600" and be indistinguishable from
/// an explicit `--ttl 600`. For `heartbeat` that distinction IS the
/// contract — omitting `--ttl` renews the lease length the claim already
/// holds, rather than resetting it to a fixed default and truncating every
/// long lease (Planar task 6093). Leaving the default off is what lets the
/// handler see `std::nullopt`.
/// @param app The node to declare it on.
auto add_heartbeat_ttl(CLI::App& app) -> void {
  app.add_option("--ttl")->description(
      "Set a new TTL absolutely (accepts bare int seconds or suffixed duration: 10m, 1h, 500ms). "
      "When omitted, the claim's CURRENT lease length is renewed from now — a heartbeat never "
      "shortens the lease it was sent to preserve.");
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
  // Declaration order matches the oracle's `handlers/cmd.zig` registry.
  auto app = std::make_unique<CLI::App>("Agent-callable coordination binary (pull / claim / complete / heartbeat / reconcile).",
                                        "planar-agent");
  app->require_subcommand(0);

  app->add_subcommand("version", "Print the planar-agent version, commit, and C++ toolchain.");

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
  add_supervision(*complete, true);
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
  add_supervision(*fail, true);
  add_json(*fail);

  CLI::App* release = app->add_subcommand(
      "release", "Graceful give-up: task \xE2\x86\x92 todo, claim \xE2\x86\x92 released (vs fail's aborted).");
  add_claim(*release, "Claim token returned by pull/claim");
  release->add_option("--reason")->description("Optional reason for releasing");
  add_no_locality_probe(*release, "Skip the git locality probe and commit collection");
  add_supervision(*release, true);
  add_json(*release);

  CLI::App* block = app->add_subcommand("block", "Atomically park the task on an external blocker.");
  add_claim(*block, "Claim token returned by pull/claim");
  block->add_option("--blocker")
      ->description("Task id of the blocker (entity_links target)")
      ->required()
      ->check(cliapp::zig_int_validator());
  block->add_option("--reason")->description("Free-text reason recorded on the claim");
  add_no_locality_probe(*block, "Skip the git locality probe and commit collection");
  add_supervision(*block, true);
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
  cliapp::add_bool_flag(*claim, "--no-transition",
                        "Claim without changing task status (plan and plan_step are always unchanged)");
  cliapp::add_bool_flag(*claim, "--force", "Take over an existing live claim (operator recovery)");
  add_run_stage(*claim);
  add_json(*claim);

  // --- heartbeat ----------------------------------------------------------
  CLI::App* heartbeat = app->add_subcommand("heartbeat", "Refresh the lease on an active claim.");
  add_claim(*heartbeat, "Claim token to refresh");
  add_heartbeat_ttl(*heartbeat);
  heartbeat->add_option("--status")->description("Free-text status string recorded on the heartbeat action row's summary column");
  add_supervision(*heartbeat, false);
  add_json(*heartbeat);

  // --- claim-associate ----------------------------------------------------
  CLI::App* claim_associate =
      app->add_subcommand("claim-associate", "Associate a pre-acquired active claim with a workflow run (and optional stage), "
                                             "and/or hand it to the engine supervisor. At least one of --run or --supervisor.");
  add_claim(*claim_associate, "Claim token to associate");
  claim_associate->add_option("--run")->description("workflow_runs.id to stamp on the claim")->check(cliapp::zig_int_validator());
  claim_associate->add_option("--stage")->description("Stage name to record (e.g. code, review); omit for NULL");
  claim_associate->add_option("--supervisor")
      ->description("engine: hand the claim to the engine supervisor (one-way; needs --attempt). caller: assert it is still "
                    "caller-supervised")
      ->check(CLI::IsMember{std::vector<std::string>{"caller", "engine"}});
  claim_associate->add_option("--attempt")->description("The Centurion attempt supervising the claim (with --supervisor engine)");
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

  // --- ingest ---------------------------------------------------------------
  CLI::App* ingest = app->add_subcommand(
      "ingest", "Translate a vendor hook event into store primitives (claude + copilot adapters wired; codex reserved).");
  ingest->add_option("--vendor")->description("Vendor tag (claude|copilot wired; codex reserved)")->required();
  ingest->add_option("--event")->description("Event JSON: @<file> reads from path; @- reads from stdin")->required();
  add_json(*ingest);

  // --- reconcile / abort --------------------------------------------------
  CLI::App* reconcile =
      app->add_subcommand("reconcile", "Operator recovery: mark expired claims stale, close orphaned actions, abandon dead "
                                       "runs.");
  cliapp::add_bool_flag(*reconcile, "--dry-run", "Report candidates without writing");
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
  cliapp::add_bool_flag(*reconcile, "--override-supervisor",
                        "Also reconcile engine-supervised claims and centurion runs (skipped by default; logged as "
                        "supervisor_override)");
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
  cliapp::add_bool_flag(*abort_cmd, "--override-supervisor",
                        "Abort an engine-supervised claim (refused otherwise; logged as supervisor_override)");
  add_json(*abort_cmd);

  app->add_subcommand("schema", "Print the full command tree as a JSON catalog (flags, aliases, positionals).");

  // --- run start / end -----------------------------------------------------
  CLI::App* run = app->add_subcommand(
      "run", "Workflow run lifecycle (start / end). Used by an external workflow harness to stay DB-handle-free.");
  run->require_subcommand(0);
  CLI::App* run_start = run->add_subcommand("start", "Insert a workflow_runs row in running status.");
  run_start->add_option("--plan")->description("Plan id the run belongs to")->required();
  run_start->add_option("--workflow")->description("Workflow name (e.g. isolated-sequential)")->required();
  run_start->add_option("--run-id")->description("Unique run identifier (run-<pid>-<nanos>)")->required();
  run_start->add_option("--pid")->description("PID of the external workflow harness process")->required();
  run_start->add_option("--repo-root")->description("Absolute path of the repo root the harness is driving")->required();
  add_json(*run_start);

  CLI::App* run_end =
      run->add_subcommand("end", "Close a workflow_runs row with a terminal status (completed|failed|interrupted).");
  run_end->add_option("--run-id")->description("The run identifier (run-<pid>-<nanos>) returned by run start")->required();
  run_end->add_option("--status")->description("Terminal status: completed | failed | interrupted")->required();
  add_json(*run_end);

  // --- dispatch preview / confirm -------------------------------------------
  CLI::App* dispatch = app->add_subcommand(
      "dispatch", "Routing dispatch authorization (preview / confirm). Binds current state to a single-use token.");
  dispatch->require_subcommand(0);
  CLI::App* dispatch_preview =
      dispatch->add_subcommand("preview", "Bind current routing state to a single-use, expiry-bound confirmation token.");
  dispatch_preview->add_option("--task")->description("Task id this dispatch targets");
  dispatch_preview->add_option("--work-item")->description("Logical work item id")->required();
  dispatch_preview->add_option("--project")->description("Project id")->required();
  dispatch_preview->add_option("--validation-policy")->description("Validation policy version")->required();
  dispatch_preview->add_option("--routing-policy")->description("Routing policy version")->required();
  dispatch_preview->add_option("--profile-rule")->description("Profile rule version")->required();
  dispatch_preview->add_option("--vendor")->description("Vendor (opaque)")->required();
  dispatch_preview->add_option("--role")->description("Role (opaque)")->required();
  dispatch_preview->add_option("--tier")->description("small|medium|large")->required();
  dispatch_preview->add_option("--work-type")->description("schema|engine|architectural|cli|feature|mechanical")->required();
  dispatch_preview->add_option("--complexity")->description("bounded|standard|high-risk")->required();
  dispatch_preview->add_option("--packet-digest")->description("Digest of the authoritative packet")->required();
  dispatch_preview->add_option("--profile-digest")->description("Digest of the compiled profile")->required();
  dispatch_preview->add_option("--policy-digest")->description("Digest of the policy snapshot")->required();
  dispatch_preview->add_option("--capability-digest")->description("Digest of the host capability snapshot")->required();
  dispatch_preview->add_option("--candidate")->description("Requested candidate row id")->required();
  dispatch_preview->add_option("--host")->description("Host id whose capability snapshot was consulted")->required();
  dispatch_preview->add_option("--class")->description("fallback|default|override|declared_experiment")->required();
  dispatch_preview->add_option("--experiment")->description("Experiment id (required for declared_experiment)");
  dispatch_preview->add_option("--claim")->description("Claim token this dispatch is bound to");
  dispatch_preview->add_option("--claim-status")->description("Claim status observed at preview time");
  dispatch_preview->add_option("--evidence-state")->description("evidential|observational")->required();
  dispatch_preview->add_option("--expires-at")->description("RFC3339 instant after which the token is dead")->required();
  add_json(*dispatch_preview);

  CLI::App* dispatch_confirm = dispatch->add_subcommand(
      "confirm", "Revalidate a preview token against current state and atomically write the dispatch snapshot.");
  dispatch_confirm->add_option("--token")->description("Preview token to spend")->required();
  dispatch_confirm->add_option("--dispatch-key")->description("Unique key for the resulting dispatch")->required();
  dispatch_confirm->add_option("--now")->description("RFC3339 instant to evaluate expiry against")->required();
  dispatch_confirm->add_option("--packet-digest")->description("Currently observed packet digest")->required();
  dispatch_confirm->add_option("--profile-digest")->description("Currently observed profile digest")->required();
  dispatch_confirm->add_option("--policy-digest")->description("Currently observed policy digest")->required();
  dispatch_confirm->add_option("--capability-digest")->description("Currently observed capability digest")->required();
  dispatch_confirm->add_option("--candidate")->description("Currently resolved candidate row id")->required();
  dispatch_confirm->add_option("--vendor")->description("Currently resolved vendor")->required();
  dispatch_confirm->add_option("--role")->description("Currently resolved role")->required();
  dispatch_confirm->add_option("--tier")->description("Currently resolved tier")->required();
  dispatch_confirm->add_option("--work-type")->description("Currently resolved work type")->required();
  dispatch_confirm->add_option("--complexity")->description("Currently resolved complexity")->required();
  dispatch_confirm->add_option("--validation-policy")->description("Currently active validation policy version")->required();
  dispatch_confirm->add_option("--routing-policy")->description("Currently active routing policy version")->required();
  dispatch_confirm->add_option("--claim")->description("Currently held claim token");
  dispatch_confirm->add_option("--claim-status")->description("Currently observed claim status");
  dispatch_confirm->add_option("--reviewer")->description("Reviewer disposition to record (default required)");
  dispatch_confirm->add_option("--decision")->description("confirmed|overridden (default confirmed)");
  add_json(*dispatch_confirm);

  // --- context add / capsule / list / resolve -------------------------------
  CLI::App* context = app->add_subcommand(
      "context", "Run-scoped working-memory records (add / list / resolve / capsule). Used by an external workflow harness.");
  context->require_subcommand(0);
  CLI::App* context_add =
      context->add_subcommand("add", "Write a context_records row stamped from the claim's run_id, stage, session_id.");
  context_add->add_option("--claim")->description("Claim token that owns this context record")->required();
  context_add->add_option("--kind")->description("Record kind: finding|risk|artifact|followup|summary|capsule")->required();
  context_add->add_option("--body")->description("Record body text")->required();
  context_add->add_option("--compiled-from")
      ->description("Comma-separated context_record ids this capsule was compiled from (capsule kind only)");
  add_json(*context_add);

  CLI::App* context_capsule = context->add_subcommand(
      "capsule", "Write a compiled capsule context_records row, run-keyed (claim_id=NULL, decision 456).");
  context_capsule->add_option("--run")->description("workflow_runs.id \xE2\x80\x94 the run that owns this capsule")->required();
  context_capsule->add_option("--stage")->description("Stage name this capsule distills (e.g. 'plan', 'code')")->required();
  context_capsule->add_option("--body")->description("Compiled capsule body text")->required();
  context_capsule->add_option("--compiled-from")
      ->description("Comma-separated context_record ids this capsule distills (provenance)");
  context_capsule->add_option("--session")
      ->description("session_id (integer). Optional \xE2\x80\x94 an ephemeral session is created when omitted.");
  add_json(*context_capsule);

  CLI::App* context_list =
      context->add_subcommand("list", "List context_records for a run, with optional stage/status/kind filters.");
  context_list->add_option("--run")->description("Run id (integer) to query")->required();
  context_list->add_option("--stage")->description("Filter to records from this stage");
  context_list->add_option("--status")->description("Filter by status: active|consumed|superseded");
  context_list->add_option("--kind")->description("Filter by kind: finding|risk|artifact|followup|summary|capsule");
  add_json(*context_list);

  CLI::App* context_resolve = context->add_subcommand(
      "resolve", "Transition context_records active \xE2\x86\x92 consumed|superseded (single record or bulk stage sweep).");
  context_resolve->add_option("--id")->description("Single record id to transition");
  context_resolve->add_option("--run")->description("Run id for bulk stage sweep (use with --stage)");
  context_resolve->add_option("--stage")->description("Stage name for bulk sweep (use with --run)");
  context_resolve->add_option("--status")->description("Target status: consumed|superseded")->required();
  add_json(*context_resolve);

  // Help renders the same page it rendered before every bool flag gained
  // its `--no-X` negation — see `planar.cliapp.surface::hide_negations_in_help`.
  // Must come AFTER the whole tree exists.
  cliapp::hide_negations_in_help(*app);
  return app;
}

auto forbidden_verbs() -> std::vector<std::string_view> {
  return {"plan", "task",      "decision", "question", "scenario", "artifact", "annotate", "init",      "workbench", "doc",
          "spec", "templates", "ext",      "sync",     "promote",  "demote",   "capture",  "dashboard", "tree",      "health"};
}

} // namespace planar::cmd::agent
