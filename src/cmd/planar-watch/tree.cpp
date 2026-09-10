/// @file tree.cpp
/// @brief Implementation of `planar.cmd.planar_watch.tree`.

module planar.cmd.planar_watch.tree;

import std;
import cli11;
import planar.cliapp.args;
import planar.cliapp.surface;

namespace planar::cmd::watch {

namespace {

/// @brief The `--json` flag every read verb carries.
///
/// `feed`'s carries a description (`"Emit NDJSON"`) and every other verb's
/// is bare — an asymmetry in the oracle's own declarations, transcribed
/// rather than harmonised. `feed`'s `--json` is declared directly at its
/// own call site (not through this helper) precisely because it needs
/// that non-bare description; only the bare form used by the other
/// hand-transcribed verbs appears here.
/// @param app The node to declare it on.
auto add_json(CLI::App& app) -> void {
  cliapp::add_bool_flag(app, "--json");
}

/// @brief The `--follow` / `--interval` pair the streaming verbs declare.
///
/// DECLARED but REFUSED at the handler — see
/// `planar.cmd.planar_watch.handlers.ledger`'s header for why a loud exit
/// 64 beats a silent single-shot. They are declared anyway because
/// `src/cmd/catalog_parity.hpp` compares this tree's flag set against the
/// oracle's, and a flag missing from the declaration is exactly the
/// transcription slip that comparison exists to catch.
/// @param app The node to declare them on.
/// @param follow_desc The verb's own wording for `--follow`.
/// @param interval_desc The verb's own wording for `--interval`.
auto add_follow(CLI::App& app, std::string follow_desc, std::string interval_desc) -> void {
  cliapp::add_bool_flag(app, "--follow", follow_desc);
  app.add_option("--interval")->description(std::move(interval_desc));
}

/// @brief The `--vendor` flag, whose wording differs between verbs.
/// @param app The node to declare it on.
/// @param desc The verb's own wording.
auto add_vendor(CLI::App& app, std::string desc) -> void {
  app.add_option("--vendor")->description(std::move(desc));
}

/// @brief An integer-valued option, through the shared Zig `parseInt`
/// validator so `--plan 1_0` means plan 10 here exactly as it does on the
/// reference binary.
/// @param app The node to declare it on.
/// @param name The flag's long name.
/// @param desc The description.
auto add_int(CLI::App& app, std::string name, std::string desc) -> void {
  app.add_option(std::move(name))->description(std::move(desc))->check(cliapp::zig_int_validator());
}

} // namespace

auto root_app() -> std::unique_ptr<CLI::App> {
  // The root's description is the multi-line block the oracle's own
  // `--help` renders; `planar-watch schema` reports the one-line
  // "Read-only viewer for live agent activity (…)" as its summary. A
  // `CLI::App` carries ONE description string where `cli::cmd` carried
  // `desc` and `long_desc` separately, so the longer, operator-facing one
  // is what survives — see `planar.cliapp.schema`'s header, divergence 1.
  // The em dash and the `→` are the oracle's own bytes and must survive
  // verbatim.
  auto app = std::make_unique<CLI::App>("planar-watch is the human-facing live cockpit for agent activity.\n"
                                        "\n"
                                        "  The default invocation with no args is the activity feed.\n"
                                        "  Subcommands narrow the view; `--follow` turns each one into a\n"
                                        "  streaming view that emits new rows as the underlying tables\n"
                                        "  change. The binary opens the database in strict read-only mode\n"
                                        "  (SQLITE_OPEN_READONLY) \xe2\x80\x94 every write SQL string is rejected by\n"
                                        "  the SQLite driver itself, the second line of defense behind\n"
                                        "  this binary's `no write verbs registered` capability boundary.\n"
                                        "\n"
                                        "  `tree` renders the orchestrator \xe2\x86\x92 sub-agent forest by walking\n"
                                        "  agent_actions.parent_action_id chains.",
                                        "planar-watch");

  // A bare `planar-watch` must render root help rather than fail, so the
  // root requires no subcommand and `dispatch::run` treats "matched a node
  // that has children, but none of them" as a help request.
  app->require_subcommand(0);

  // Declaration ORDER matches the oracle's `handlers/cmd.zig` registry:
  // feed, ps, claims, actions, plans, log, tree, run, sync-events, version,
  // completion, schema — `feed`, `run` (+ its two children) and
  // `sync-events` used to come from generated surface data applied after
  // this block (`cliapp::apply_surface`), folded in here directly at task
  // 6613 (M11.1) so there is exactly one declaration site per node. Every
  // leaf below has a real handler (`dispatch.cpp`'s table); none refuses at
  // exit 64. Each description is the oracle node's LONG description
  // transcribed verbatim — `CLI::App` carries one description string, and
  // this tree's settled choice is that the longer operator-facing one
  // survives (see the root's note above, and `src/cmd/planar/tree.cpp`'s
  // `workbench`).
  //
  // TRANSCRIPTION INCLUDES THE `--follow` SENTENCES, which promise a
  // streaming arm this build refuses (exit 64). Left verbatim rather than
  // edited: the description is a D2 transcription, the divergence is named
  // in this binary's CMakeLists and in the two handler modules' headers,
  // and the runtime refusal is loud rather than silent. A reader who
  // follows the help page gets an explicit "not implemented in this build",
  // not a snapshot pretending to be a stream.

  // --- feed -----------------------------------------------------------------
  // Folded in from `surface.cpp`'s generated `k_path_0` at task 6613 (M11.1).
  // Was previously declared via `cliapp::apply_surface` below, alongside
  // `run`/`sync-events`/`run list`/`run show`; `dispatch.cpp` already
  // registers a real handler for it (`handlers::feed`), so this is a pure
  // declaration-site move, not a behavior change. Verified byte-identical
  // against the prior generated description before this edit landed.
  CLI::App* feed = app->add_subcommand(
      "feed", "One event per claim transition, action transition, or task status\n  change, in occurrence-time order. The "
              "default planar-watch\n  invocation routes here.\n\n  Without --follow: print the initial snapshot up to "
              "--limit\n  events (default 100), newest first.\n  With --follow: print the snapshot, then stream new events "
              "as\n  they appear. Tier-1 poll; --interval defaults to 1s.\n\n  --tail N: emit the most-recent N events on "
              "first call (the\n  journalctl -f -n idiom). --tail 0 or negative exits with\n  InvalidValue. Combined with "
              "--follow: the tail emission comes\n  first, then only NEW events stream (no re-emit of tailed events).\n\n  "
              "Filters (--vendor / --plan / --task / --since) narrow both the\n  snapshot and the streaming view.\n\n  "
              "--json emits NDJSON \xe2\x80\x94 one JSON object per line, no surrounding\n  array, no trailing comma. "
              "Consumers can pipe through `jq -c`.");
  cliapp::add_bool_flag(*feed, "--follow", "Stream new events until SIGINT");
  add_vendor(*feed, "Vendor filter");
  add_int(*feed, "--plan", "Plan id filter (matches plan-direct, task-on-plan, and plan_step-on-plan events)");
  add_int(*feed, "--task", "Task id filter");
  feed->add_option("--since")->description("Only events with at >= this ISO8601 timestamp");
  add_int(*feed, "--limit", "Snapshot row cap (default 100)");
  add_int(*feed, "--tail", "Return only the most-recent N events (must be > 0)");
  cliapp::add_bool_flag(*feed, "--json", "Emit NDJSON");
  feed->add_option("--interval")->description("Poll interval for --follow (default 1s)");

  // --- ps -----------------------------------------------------------------
  CLI::App* ps = app->add_subcommand("ps", "Lists every currently active agent claim \xe2\x80\x94 one row per claim_token.\n"
                                           "\n"
                                           "  --stale also includes claims whose lease has expired OR whose\n"
                                           "  status is `stale` (set by `planar-agent reconcile`).\n"
                                           "\n"
                                           "  --vendor / --plan narrow the result.\n"
                                           "\n"
                                           "  --sort-by heartbeat (default) orders by most-recently-heartbeated\n"
                                           "  first. --sort-by lease restores the pre-M3 claimed_at ordering.\n"
                                           "\n"
                                           "  --follow turns the snapshot into a streaming view (Tier-1 poll;\n"
                                           "  --interval defaults to 1s). Exits 0 on SIGINT.");
  add_vendor(*ps, "Filter by vendor (claude, codex, copilot, ...)");
  add_int(*ps, "--plan", "Filter by plan id (matches plan-direct, task-on-plan, and plan_step-on-plan claims)");
  cliapp::add_bool_flag(*ps, "--stale", "Include stale + lease-expired claims");
  add_json(*ps);
  add_follow(*ps, "Stream snapshots until SIGINT", "Poll interval for --follow (default 1s; e.g. 100ms)");
  ps->add_option("--sort-by")->description("Sort order for active claims: heartbeat (default) or lease");
  ps->add_option("--group-by")->description("Group claims by dimension: role, scope, or vendor");

  // --- claims -------------------------------------------------------------
  CLI::App* claims = app->add_subcommand("claims", "Returns claim rows from agent_work_claims. The default is\n"
                                                   "  --status active.\n"
                                                   "\n"
                                                   "  --status active : claim row is in 'active' state with an\n"
                                                   "                    unexpired lease (default).\n"
                                                   "  --status stale  : status='stale' OR an expired-lease active\n"
                                                   "                    claim (matches `ps --stale`).\n"
                                                   "  --status all    : every row (active, released, completed,\n"
                                                   "                    aborted, stale) \xe2\x80\x94 the full claim ledger.");
  add_vendor(*claims, "Filter by vendor");
  add_int(*claims, "--plan", "Filter by plan id (matches plan-direct, task-on-plan, and plan_step-on-plan claims)");
  claims->add_option("--status")->description("active (default) | stale | all");
  add_json(*claims);
  add_follow(*claims, "Stream snapshots until SIGINT", "Poll interval for --follow (default 1s)");

  // --- actions ------------------------------------------------------------
  CLI::App* actions =
      app->add_subcommand("actions", "Returns agent_actions rows ordered by started_at descending.\n"
                                     "\n"
                                     "  --kind     : action_kind filter (coder, reviewer, tool_call, etc.).\n"
                                     "  --entity   : restrict to one entity, `kind:id` form (e.g. `task:42`).\n"
                                     "  --plan     : restrict to actions on the plan, or on tasks/plan_steps belonging to it.\n"
                                     "  --task     : restrict to actions whose entity_kind=task, entity_id=N.\n"
                                     "  --vendor   : vendor filter.\n"
                                     "  --limit    : cap row count (default 100).");
  add_vendor(*actions, "Vendor filter");
  actions->add_option("--kind")->description("action_kind filter");
  actions->add_option("--entity")->description("Restrict to one entity, kind:id form");
  add_int(*actions, "--plan", "Filter by plan id");
  add_int(*actions, "--task", "Filter by task id");
  add_int(*actions, "--limit", "Row cap (default 100)");
  add_json(*actions);
  add_follow(*actions, "Stream snapshots until SIGINT", "Poll interval for --follow (default 1s)");

  // --- plans --------------------------------------------------------------
  CLI::App* plans = app->add_subcommand("plans", "Each row pairs a plan with its in-flight summary:\n"
                                                 "    active_claims  \xe2\x80\x94 claims with status='active' and\n"
                                                 "                     lease_expires_at >= now() targeting any task\n"
                                                 "                     under the plan.\n"
                                                 "    active_actions \xe2\x80\x94 agent_actions rows with ended_at IS NULL\n"
                                                 "                     whose entity_kind/entity_id refer to a task\n"
                                                 "                     under the plan.\n"
                                                 "    last_event_at  \xe2\x80\x94 max of claim claimed_at / heartbeat /\n"
                                                 "                     released_at and action started_at /\n"
                                                 "                     ended_at across the plan's tasks; null\n"
                                                 "                     when no events recorded.\n"
                                                 "\n"
                                                 "  --in-flight-only drops plans where active_claims=0 AND\n"
                                                 "  active_actions=0.");
  cliapp::add_bool_flag(*plans, "--in-flight-only", "Skip plans with no live work");
  add_json(*plans);
  add_follow(*plans, "Stream snapshots until SIGINT", "Poll interval for --follow (default 1s)");

  // --- log ----------------------------------------------------------------
  CLI::App* log = app->add_subcommand("log", "Streams the agent_actions + agent_work_claims history scoped to\n"
                                             "  one entity or one claim_token. Exactly one of\n"
                                             "  --task / --plan / --entity / --session / --claim is required.\n"
                                             "\n"
                                             "  Entries are emitted in occurrence-time order (oldest first)\n"
                                             "  as a discriminated union: each entry carries a `.kind` field\n"
                                             "  that is either `action` (full ActionRow payload) or\n"
                                             "  `claim_acquired` / `claim_heartbeat` / `claim_released` /\n"
                                             "  `claim_stale` (with ClaimRow payload).");
  add_int(*log, "--task", "Filter to one task id");
  add_int(*log, "--plan", "Filter to one plan id (matches entity_kind=plan rows)");
  log->add_option("--entity")->description("Filter to one entity, kind:id form");
  add_int(*log, "--session", "Filter to one session_id");
  log->add_option("--claim")->description("Filter to one claim_token");
  add_int(*log, "--limit", "Row cap (default 100)");
  add_json(*log);

  // --- tree ---------------------------------------------------------------
  CLI::App* forest =
      app->add_subcommand("tree", "Walks agent_actions.parent_action_id chains and renders the\n"
                                  "  orchestrator \xe2\x86\x92 sub-agent forest. Root rows have parent_action_id IS NULL.\n"
                                  "  Each child is indented with unicode tree characters (\xe2\x94\x9c\xe2\x94\x80\xe2\x94\x80 / "
                                  "\xe2\x94\x94\xe2\x94\x80\xe2\x94\x80 / \xe2\x94\x82).\n"
                                  "\n"
                                  "  --root-session <id>  scope to one session's subtree (error if unknown).\n"
                                  "  --follow             stream; re-renders on WAL change (Tier-2 wake).\n"
                                  "  --interval           maximum poll cadence for --follow (default 1s).\n"
                                  "\n"
                                  "  Each row shows the claim's: scope vendor activity worktree branch last_hb.");
  add_int(*forest, "--root-session", "Scope output to one session's subtree (session id)");
  add_follow(*forest, "Stream re-renders until SIGINT", "Poll interval for --follow (default 1s; e.g. 100ms)");

  // --- run ------------------------------------------------------------
  // Folded in from `surface.cpp`'s generated `k_path_7/12/13` at task 6613
  // (M11.1). `run list`/`run show` are real handlers —
  // `dispatch.cpp` registers `handlers::run_list` / `handlers::run_show`.
  // The group node itself carries no flags and no handler; a bare
  // `planar-watch run` renders its own help page (`require_subcommand(0)`),
  // matching the prior `apply_surface`-declared shape.
  CLI::App* run = app->add_subcommand(
      "run", "Read-only view of run tables. `list` covers both workflow_runs (wf)\nand the runs table (op-arm); `show` "
             "drills into wf-source runs only.\n\n  list  \xe2\x80\x94 list runs (--plan / --status / --arm filters).\n  "
             "show  \xe2\x80\x94 drill into one wf-source run's context records.");
  run->require_subcommand(0);

  CLI::App* run_list = run->add_subcommand(
      "list", "Returns runs ordered by started_at descending.\n\n  --plan <id>    restrict to runs for the given plan.\n  "
              "--status <s>   restrict by status: running | completed | failed |\n                 interrupted | abandoned. "
              "Default: all.\n  --arm <a>      source table: wf (workflow_runs / context-plane),\n                 op (runs "
              "/ op-arm), or all (default, both).\n  --json         emit a single JSON object instead of human text.");
  add_int(*run_list, "--plan", "Filter by plan id");
  run_list->add_option("--status")->description("Filter by status (default: all)");
  run_list->add_option("--arm")->description("Source arm: wf | op | all (default: all)");
  add_json(*run_list);

  CLI::App* run_show = run->add_subcommand(
      "show", "Returns the full workflow_runs row for <id> plus all\n  context_records for that run, grouped and ordered "
              "by\n  stage then created_at.\n\n  Exits non-zero when the run id is unknown.");
  add_json(*run_show);
  run_show->add_option("id")->description("Workflow run id (integer)")->required();

  // --- sync-events ----------------------------------------------------
  CLI::App* sync_events = app->add_subcommand(
      "sync-events", "Returns sync_events rows ordered by `at` descending.\n\n  --plan     : restrict to events whose link "
                     "belongs to the given plan id.\n  --system   : restrict to events via a link on the given external "
                     "system slug.\n  --entity   : restrict to events via a link on one entity, `kind:id` form.\n  "
                     "--outcome  : filter by outcome value (ok, conflict, error, noop, \xe2\x80\xa6).\n  --since    : only "
                     "return rows with `at` >= this ISO8601 timestamp.\n  --limit    : cap row count (default 100).");
  add_int(*sync_events, "--plan", "Filter by plan id");
  sync_events->add_option("--system")->description("Filter by external system slug");
  sync_events->add_option("--entity")->description("Filter by entity, kind:id form (e.g. task:42)");
  sync_events->add_option("--outcome")->description("Filter by outcome (ok, conflict, error, noop, \xe2\x80\xa6)");
  sync_events->add_option("--since")->description("Only rows at >= this ISO8601 timestamp");
  add_int(*sync_events, "--limit", "Row cap (default 100)");
  add_json(*sync_events);

  app->add_subcommand("version", "Print the planar-watch version, commit, and C++ toolchain.");

  CLI::App* completion = app->add_subcommand("completion", "Generate the autocompletion script for the specified shell.");
  completion->add_option("shell")->description("Shell: bash, zsh, or fish")->required();

  app->add_subcommand("schema", "Print the full command tree as a JSON catalog (flags, aliases, positionals).");

  // Every node above (including `feed`, `run`/`run list`/`run show` and
  // `sync-events`) is now hand-transcribed here — task 6613 (M11.1) folded
  // `surface.cpp`'s generated `node_spec` table in, matching the
  // `planar-ext` shape (one declaration site per node, no
  // `cliapp::apply_surface` precedence rule). `planar.cmd.planar_watch.surface`
  // still exists but now holds only the summary-vs-description divergence
  // table (`surface_summaries`, consumed by `handlers::schema`) and the
  // (currently empty) unported-leaf inventory `dispatch.cpp` still reads —
  // neither is a second declaration of a node this tree already owns.

  // Help renders the same page it rendered before every bool flag gained
  // its `--no-X` negation — see `planar.cliapp.surface::hide_negations_in_help`.
  // Must come AFTER the whole tree exists.
  cliapp::hide_negations_in_help(*app);
  return app;
}

auto forbidden_verbs() -> std::vector<std::string_view> {
  return {// Every planar-agent write verb.
          "pull", "claim", "heartbeat", "complete", "fail", "release", "block", "action", "ingest", "reconcile", "abort", "peek",
          // Every planar planning-entity verb.
          "plan", "task", "decision", "question", "scenario", "artifact", "annotate", "init", "workbench", "doc", "spec",
          "templates", "ext", "sync", "promote", "demote", "capture"};
}

} // namespace planar::cmd::watch
