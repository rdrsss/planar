/// @file surface.cpp
/// @brief Implementation of `planar.cmd.planar_watch.surface` — GENERATED, do not hand-edit.
///
/// Regenerate with `scripts/gen-cli-surface.py` (see that script and this
/// module's interface header for the provenance argument).

module planar.cmd.planar_watch.surface;

import std;
import planar.cliapp.surface;

namespace planar::cmd::watch {

using cliapp::flag_spec;
using cliapp::node_spec;
using cliapp::positional_spec;

namespace {

constexpr std::string_view k_path_0[]  = {"feed"};
constexpr std::string_view k_path_1[]  = {"ps"};
constexpr std::string_view k_path_2[]  = {"claims"};
constexpr std::string_view k_path_3[]  = {"actions"};
constexpr std::string_view k_path_4[]  = {"plans"};
constexpr std::string_view k_path_5[]  = {"log"};
constexpr std::string_view k_path_6[]  = {"tree"};
constexpr std::string_view k_path_7[]  = {"run"};
constexpr std::string_view k_path_8[]  = {"sync-events"};
constexpr std::string_view k_path_9[]  = {"version"};
constexpr std::string_view k_path_10[] = {"completion"};
constexpr std::string_view k_path_11[] = {"schema"};
constexpr std::string_view k_path_12[] = {"run", "list"};
constexpr std::string_view k_path_13[] = {"run", "show"};

constexpr flag_spec k_flags_0[] = {
    {.name          = "--follow",
     .kind          = "bool",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Stream new events until SIGINT"},
    {.name = "--vendor", .kind = "string", .required = false, .list = false, .default_value = {}, .description = "Vendor filter"},
    {.name          = "--plan",
     .kind          = "int",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Plan id filter (matches plan-direct, task-on-plan, and plan_step-on-plan events)"},
    {.name = "--task", .kind = "int", .required = false, .list = false, .default_value = {}, .description = "Task id filter"},
    {.name          = "--since",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Only events with at >= this ISO8601 timestamp"},
    {.name          = "--limit",
     .kind          = "int",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Snapshot row cap (default 100)"},
    {.name          = "--tail",
     .kind          = "int",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Return only the most-recent N events (must be > 0)"},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = "Emit NDJSON"},
    {.name          = "--interval",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Poll interval for --follow (default 1s)"},
};
constexpr flag_spec k_flags_1[] = {
    {.name          = "--vendor",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Filter by vendor (claude, codex, copilot, ...)"},
    {.name          = "--plan",
     .kind          = "int",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Filter by plan id (matches plan-direct, task-on-plan, and plan_step-on-plan claims)"},
    {.name          = "--stale",
     .kind          = "bool",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Include stale + lease-expired claims"},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name          = "--follow",
     .kind          = "bool",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Stream snapshots until SIGINT"},
    {.name          = "--interval",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Poll interval for --follow (default 1s; e.g. 100ms)"},
    {.name          = "--sort-by",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Sort order for active claims: heartbeat (default) or lease"},
    {.name          = "--group-by",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Group claims by dimension: role, scope, or vendor"},
};
constexpr flag_spec k_flags_2[] = {
    {.name          = "--vendor",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Filter by vendor"},
    {.name          = "--plan",
     .kind          = "int",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Filter by plan id (matches plan-direct, task-on-plan, and plan_step-on-plan claims)"},
    {.name          = "--status",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "active (default) | stale | all"},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name          = "--follow",
     .kind          = "bool",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Stream snapshots until SIGINT"},
    {.name          = "--interval",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Poll interval for --follow (default 1s)"},
};
constexpr flag_spec k_flags_3[] = {
    {.name = "--vendor", .kind = "string", .required = false, .list = false, .default_value = {}, .description = "Vendor filter"},
    {.name          = "--kind",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "action_kind filter"},
    {.name          = "--entity",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Restrict to one entity, kind:id form"},
    {.name = "--plan", .kind = "int", .required = false, .list = false, .default_value = {}, .description = "Filter by plan id"},
    {.name = "--task", .kind = "int", .required = false, .list = false, .default_value = {}, .description = "Filter by task id"},
    {.name          = "--limit",
     .kind          = "int",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Row cap (default 100)"},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name          = "--follow",
     .kind          = "bool",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Stream snapshots until SIGINT"},
    {.name          = "--interval",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Poll interval for --follow (default 1s)"},
};
constexpr flag_spec k_flags_4[] = {
    {.name          = "--in-flight-only",
     .kind          = "bool",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Skip plans with no live work"},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name          = "--follow",
     .kind          = "bool",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Stream snapshots until SIGINT"},
    {.name          = "--interval",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Poll interval for --follow (default 1s)"},
};
constexpr flag_spec k_flags_5[] = {
    {.name          = "--task",
     .kind          = "int",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Filter to one task id"},
    {.name          = "--plan",
     .kind          = "int",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Filter to one plan id (matches entity_kind=plan rows)"},
    {.name          = "--entity",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Filter to one entity, kind:id form"},
    {.name          = "--session",
     .kind          = "int",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Filter to one session_id"},
    {.name          = "--claim",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Filter to one claim_token"},
    {.name          = "--limit",
     .kind          = "int",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Row cap (default 100)"},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_6[] = {
    {.name          = "--root-session",
     .kind          = "int",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Scope output to one session's subtree (session id)"},
    {.name          = "--follow",
     .kind          = "bool",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Stream re-renders until SIGINT"},
    {.name          = "--interval",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Poll interval for --follow (default 1s; e.g. 100ms)"},
};
constexpr flag_spec k_flags_8[] = {
    {.name = "--plan", .kind = "int", .required = false, .list = false, .default_value = {}, .description = "Filter by plan id"},
    {.name          = "--system",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Filter by external system slug"},
    {.name          = "--entity",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Filter by entity, kind:id form (e.g. task:42)"},
    {.name          = "--outcome",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Filter by outcome (ok, conflict, error, noop, …)"},
    {.name          = "--since",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Only rows at >= this ISO8601 timestamp"},
    {.name          = "--limit",
     .kind          = "int",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Row cap (default 100)"},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_12[] = {
    {.name = "--plan", .kind = "int", .required = false, .list = false, .default_value = {}, .description = "Filter by plan id"},
    {.name          = "--status",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Filter by status (default: all)"},
    {.name          = "--arm",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Source arm: wf | op | all (default: all)"},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_13[] = {
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};

constexpr positional_spec k_pos_10[] = {
    {.name = "shell", .required = true, .description = "Shell: bash, zsh, or fish"},
};
constexpr positional_spec k_pos_13[] = {
    {.name = "id", .required = true, .description = "Workflow run id (integer)"},
};

} // namespace

/// @brief Every command node `planar-watch` declares, as a flat list.
///
/// This is the declarative surface the CLI11 tree is built from and the
/// schema catalog is emitted from — one description of the verb set, not
/// two that can drift. `catalog_parity.hpp` diffs it against the Zig
/// oracle's own `schema` output, so a node added here without a handler is
/// caught by dispatch's registration gate rather than shipping as a verb
/// that silently exits 0.
///
/// @return One `node_spec` per command node, in declaration order.
auto surface_nodes() -> std::vector<node_spec> {
  return {
      {.path = k_path_0,
       .description =
           "One event per claim transition, action transition, or task status\n  change, in occurrence-time order. The default "
           "planar-watch\n  invocation routes here.\n\n  Without --follow: print the initial snapshot up to --limit\n  events "
           "(default 100), newest first.\n  With --follow: print the snapshot, then stream new events as\n  they appear. Tier-1 "
           "poll; --interval defaults to 1s.\n\n  --tail N: emit the most-recent N events on first call (the\n  journalctl -f -n "
           "idiom). --tail 0 or negative exits with\n  InvalidValue. Combined with --follow: the tail emission comes\n  first, "
           "then only NEW events stream (no re-emit of tailed events).\n\n  Filters (--vendor / --plan / --task / --since) "
           "narrow both the\n  snapshot and the streaming view.\n\n  --json emits NDJSON — one JSON object per line, no "
           "surrounding\n  array, no trailing comma. Consumers can pipe through `jq -c`.",
       .flags       = k_flags_0,
       .positionals = {},
       .group       = false},
      {.path        = k_path_1,
       .description = "Lists every currently active agent claim — one row per claim_token.\n\n  --stale also includes claims "
                      "whose lease has expired OR whose\n  status is `stale` (set by `planar-agent reconcile`).\n\n  --vendor / "
                      "--plan narrow the result.\n\n  --sort-by heartbeat (default) orders by most-recently-heartbeated\n  "
                      "first. --sort-by lease restores the pre-M3 claimed_at ordering.\n\n  --follow turns the snapshot into a "
                      "streaming view (Tier-1 poll;\n  --interval defaults to 1s). Exits 0 on SIGINT.",
       .flags       = k_flags_1,
       .positionals = {},
       .group       = false},
      {.path = k_path_2,
       .description =
           "Returns claim rows from agent_work_claims. The default is\n  --status active.\n\n  --status active : claim row is in "
           "'active' state with an\n                    unexpired lease (default).\n  --status stale  : status='stale' OR an "
           "expired-lease active\n                    claim (matches `ps --stale`).\n  --status all    : every row (active, "
           "released, completed,\n                    aborted, stale) — the full claim ledger.",
       .flags       = k_flags_2,
       .positionals = {},
       .group       = false},
      {.path = k_path_3,
       .description =
           "Returns agent_actions rows ordered by started_at descending.\n\n  --kind     : action_kind filter (coder, reviewer, "
           "tool_call, etc.).\n  --entity   : restrict to one entity, `kind:id` form (e.g. `task:42`).\n  --plan     : restrict "
           "to actions on the plan, or on tasks/plan_steps belonging to it.\n  --task     : restrict to actions whose "
           "entity_kind=task, entity_id=N.\n  --vendor   : vendor filter.\n  --limit    : cap row count (default 100).",
       .flags       = k_flags_3,
       .positionals = {},
       .group       = false},
      {.path = k_path_4,
       .description =
           "Each row pairs a plan with its in-flight summary:\n    active_claims  — claims with status='active' and\n            "
           "         lease_expires_at >= now() targeting any task\n                     under the plan.\n    active_actions — "
           "agent_actions rows with ended_at IS NULL\n                     whose entity_kind/entity_id refer to a task\n         "
           "            under the plan.\n    last_event_at  — max of claim claimed_at / heartbeat /\n                     "
           "released_at and action started_at /\n                     ended_at across the plan's tasks; null\n                   "
           "  when no events recorded.\n\n  --in-flight-only drops plans where active_claims=0 AND\n  active_actions=0.",
       .flags       = k_flags_4,
       .positionals = {},
       .group       = false},
      {.path        = k_path_5,
       .description = "Streams the agent_actions + agent_work_claims history scoped to\n  one entity or one claim_token. Exactly "
                      "one of\n  --task / --plan / --entity / --session / --claim is required.\n\n  Entries are emitted in "
                      "occurrence-time order (oldest first)\n  as a discriminated union: each entry carries a `.kind` field\n  "
                      "that is either `action` (full ActionRow payload) or\n  `claim_acquired` / `claim_heartbeat` / "
                      "`claim_released` /\n  `claim_stale` (with ClaimRow payload).",
       .flags       = k_flags_5,
       .positionals = {},
       .group       = false},
      {.path        = k_path_6,
       .description = "Walks agent_actions.parent_action_id chains and renders the\n  orchestrator → sub-agent forest. Root rows "
                      "have parent_action_id IS NULL.\n  Each child is indented with unicode tree characters (├── / └── / "
                      "│).\n\n  --root-session <id>  scope to one session's subtree (error if unknown).\n  --follow             "
                      "stream; re-renders on WAL change (Tier-2 wake).\n  --interval           maximum poll cadence for --follow "
                      "(default 1s).\n\n  Each row shows the claim's: scope vendor activity worktree branch last_hb.",
       .flags       = k_flags_6,
       .positionals = {},
       .group       = false},
      {.path        = k_path_7,
       .description = "Read-only view of run tables. `list` covers both workflow_runs (wf)\nand the runs table (op-arm); `show` "
                      "drills into wf-source runs only.\n\n  list  — list runs (--plan / --status / --arm filters).\n  show  — "
                      "drill into one wf-source run's context records.",
       .flags       = {},
       .positionals = {},
       .group       = true},
      {.path        = k_path_8,
       .description = "Returns sync_events rows ordered by `at` descending.\n\n  --plan     : restrict to events whose link "
                      "belongs to the given plan id.\n  --system   : restrict to events via a link on the given external system "
                      "slug.\n  --entity   : restrict to events via a link on one entity, `kind:id` form.\n  --outcome  : filter "
                      "by outcome value (ok, conflict, error, noop, …).\n  --since    : only return rows with `at` >= this "
                      "ISO8601 timestamp.\n  --limit    : cap row count (default 100).",
       .flags       = k_flags_8,
       .positionals = {},
       .group       = false},
      {.path        = k_path_9,
       .description = "Print the planar-watch version, commit, and zig runtime.",
       .flags       = {},
       .positionals = {},
       .group       = false},
      {.path        = k_path_10,
       .description = "Generate the autocompletion script for the specified shell.",
       .flags       = {},
       .positionals = k_pos_10,
       .group       = false},
      {.path        = k_path_11,
       .description = "Print the full command tree as a JSON catalog (flags, aliases, positionals).",
       .flags       = {},
       .positionals = {},
       .group       = false},
      {.path = k_path_12,
       .description =
           "Returns runs ordered by started_at descending.\n\n  --plan <id>    restrict to runs for the given plan.\n  --status "
           "<s>   restrict by status: running | completed | failed |\n                 interrupted | abandoned. Default: all.\n  "
           "--arm <a>      source table: wf (workflow_runs / context-plane),\n                 op (runs / op-arm), or all "
           "(default, both).\n  --json         emit a single JSON object instead of human text.",
       .flags       = k_flags_12,
       .positionals = {},
       .group       = false},
      {.path        = k_path_13,
       .description = "Returns the full workflow_runs row for <id> plus all\n  context_records for that run, grouped and ordered "
                      "by\n  stage then created_at.\n\n  Exits non-zero when the run id is unknown.",
       .flags       = k_flags_13,
       .positionals = k_pos_13,
       .group       = false},
  };
}

auto surface_summaries() -> std::span<std::pair<std::string_view, std::string_view> const> {
  static constexpr std::pair<std::string_view, std::string_view> k_summaries[] = {
      {"planar-watch", "Read-only viewer for live agent activity (feed / ps / claims / actions / plans / log / tree / run)."},
      {"planar-watch feed", "Cross-cutting activity feed across all vendors (default verb)."},
      {"planar-watch ps", "Snapshot of active (and stale) agent claims."},
      {"planar-watch claims", "List claims in the agent_work_claims ledger (filterable by status)."},
      {"planar-watch actions", "List agent_actions rows with optional filters."},
      {"planar-watch plans", "List plans with in-flight agent work."},
      {"planar-watch log", "Per-entity / per-claim history (union of agent actions and claim transitions)."},
      {"planar-watch tree", "Render the orchestrator → sub-agent action forest."},
      {"planar-watch run", "Observe workflow runs and their context records."},
      {"planar-watch run list", "List workflow runs (filterable by plan, status, and source arm)."},
      {"planar-watch run show", "Show one workflow run plus its context_records grouped by stage."},
      {"planar-watch sync-events", "List sync_events rows with optional filters (read-only)."},
      {"planar-watch version", "Print the planar-watch version, commit, and zig runtime."},
      {"planar-watch completion", "Generate the autocompletion script for the specified shell."},
      {"planar-watch schema", "Print the full command tree as a JSON catalog (flags, aliases, positionals)."},
  };
  return k_summaries;
}

auto unported_paths() -> std::span<std::string_view const> {
  static constexpr std::string_view k_unported[] = {
      "run list",
      "run show",
      "sync-events",
  };
  return k_unported;
}

} // namespace planar::cmd::watch
