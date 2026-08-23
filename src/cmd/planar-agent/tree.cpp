/// @file tree.cpp
/// @brief Implementation of `planar.cmd.planar_agent.tree`.

module planar.cmd.planar_agent.tree;

import std;
import planar.cli;

namespace planar::cmd::agent {

namespace {

/// @brief The `--json` flag every verb in this binary carries.
///
/// It has NO `desc`, deliberately: the oracle's declaration carries none,
/// so the help page renders the line bare (`--json (bool) default=false`)
/// with no trailing em-dash clause. Adding a description here would change
/// fourteen help pages at once.
/// @return The flag spec.
auto json_flag() -> cli::flag {
  return cli::flag{.long_name = "--json", .value_kind = cli::kind::boolean, .default_value = false};
}

/// @brief The `--claim <token>` flag the eight token-addressed verbs carry.
/// @param desc The verb's own wording for it — these genuinely differ
/// ("returned by pull/claim", "to refresh", "to force-release"...).
/// @return The flag spec.
auto claim_flag(std::string desc) -> cli::flag {
  return cli::flag{.long_name = "--claim", .desc = std::move(desc), .value_kind = cli::kind::string, .required = true};
}

/// @brief The closed failure taxonomy shared by `fail`, `abort` and
/// `reconcile` — the same six values in the same order in all three.
/// @return The choice set.
auto failure_categories() -> std::vector<std::string> {
  return {"usage_limit", "context_limit", "output_limit", "tool_failure", "validation", "unknown"};
}

/// @brief The `--no-locality-probe` flag.
/// @param desc The verb's own wording (the terminal verbs mention commit
/// collection; the acquisition verbs do not).
/// @return The flag spec.
auto no_locality_probe_flag(std::string desc) -> cli::flag {
  return cli::flag{
      .long_name = "--no-locality-probe", .desc = std::move(desc), .value_kind = cli::kind::boolean, .default_value = false};
}

/// @brief The `--ttl` flag, whose wording differs by one word between the
/// acquisition verbs ("Lease TTL") and `heartbeat` ("New TTL").
/// @param desc The verb's own wording.
/// @return The flag spec.
auto ttl_flag(std::string desc) -> cli::flag {
  return cli::flag{
      .long_name = "--ttl", .desc = std::move(desc), .value_kind = cli::kind::string, .default_value = std::string{"600"}};
}

/// @brief The `--vendor` / `--vendor-session` pair.
/// @param vendor_desc The verb's wording for `--vendor`.
/// @param session_desc The verb's wording for `--vendor-session`.
/// @return The two flags, in declaration order.
auto vendor_flags(std::string vendor_desc, std::string session_desc) -> std::vector<cli::flag> {
  return {cli::flag{.long_name     = "--vendor",
                    .desc          = std::move(vendor_desc),
                    .value_kind    = cli::kind::string,
                    .default_value = std::string{"planar-agent"}},
          cli::flag{.long_name = "--vendor-session", .desc = std::move(session_desc), .value_kind = cli::kind::string}};
}

/// @brief The `--run` / `--stage` pair carried by `pull` and `claim`
/// (NOT by `claim-associate`, whose versions are required and worded
/// differently).
/// @return The two flags, in declaration order.
auto run_stage_flags() -> std::vector<cli::flag> {
  return {cli::flag{.long_name = "--run",
                    .desc      = "workflow_runs.id to associate with this claim (populated by an external workflow harness; omit "
                                 "for interactive claims)",
                    .value_kind = cli::kind::integer},
          cli::flag{.long_name  = "--stage",
                    .desc       = "Workflow stage name (e.g. code, review) to record on the claim; requires --run",
                    .value_kind = cli::kind::string}};
}

/// @brief Append `extra` to `flags` in order.
/// @param flags The accumulating flag list.
/// @param extra The flags to append.
auto append(std::vector<cli::flag>& flags, std::vector<cli::flag> extra) -> void {
  flags.insert(flags.end(), std::make_move_iterator(extra.begin()), std::make_move_iterator(extra.end()));
}

} // namespace

auto root_command() -> cli::cmd {
  // Every `desc` below is transcribed from the Zig node and then CHECKED
  // against the oracle's rendered `--help` bytes (tree.t.cpp), which is the
  // direction that catches a mistake: a wrong word changes the page.
  cli::cmd version{.name = "version", .desc = "Print the planar-agent version, commit, and zig runtime."};
  cli::cmd schema{.name = "schema", .desc = "Print the full command tree as a JSON catalog (flags, aliases, positionals)."};

  // --- pull ---------------------------------------------------------------
  std::vector<cli::flag> pull_flags;
  append(pull_flags, vendor_flags("Vendor tag (default: planar-agent)", "Vendor session id (e.g. claude:s1)"));
  pull_flags.push_back(
      {.long_name = "--role", .desc = "Role name (planner|coder|reviewer|test_coder|...)", .value_kind = cli::kind::string});
  pull_flags.push_back(ttl_flag("Lease TTL (default 600s; accepts bare int seconds or suffixed duration: 10m, 1h, 500ms)"));
  pull_flags.push_back(
      {.long_name = "--purpose", .desc = "Free-text purpose recorded on the claim", .value_kind = cli::kind::string});
  pull_flags.push_back({.long_name = "--base-ref", .desc = "Git ref the work is based on", .value_kind = cli::kind::string});
  pull_flags.push_back(
      {.long_name = "--worktree", .desc = "Worktree id or path for isolation context", .value_kind = cli::kind::string});
  pull_flags.push_back({.long_name  = "--repo-root",
                        .desc       = "Absolute path of checkout to probe locality against",
                        .value_kind = cli::kind::string});
  pull_flags.push_back(no_locality_probe_flag("Skip the git locality probe"));
  pull_flags.push_back({.long_name = "--metadata",
                        .desc = "Opaque text (typically JSON) persisted on the dispatch action row; validated as well-formed "
                                "JSON when supplied",
                        .value_kind = cli::kind::string});
  pull_flags.push_back({.long_name  = "--parent-action",
                        .desc       = "Parent action id; wires the new action as a child of this action in `planar-watch tree` "
                                      "(cross-session hierarchy)",
                        .value_kind = cli::kind::integer});
  append(pull_flags, run_stage_flags());
  pull_flags.push_back(json_flag());
  cli::cmd pull{
      .name        = "pull",
      .desc        = "Atomically pick the next eligible task, claim it, and flip status to doing.",
      .flags       = std::move(pull_flags),
      .positionals = {{.name = "plan-id", .desc = "Plan id to pull from", .value_kind = cli::kind::integer, .required = true}}};

  // --- peek ---------------------------------------------------------------
  cli::cmd peek{
      .name        = "peek",
      .desc        = "Read-only what's-next selector (same query as pull, no writes).",
      .flags       = {json_flag()},
      .positionals = {{.name = "plan-id", .desc = "Plan id to peek into", .value_kind = cli::kind::integer, .required = true}}};

  // --- complete / fail / release / block -----------------------------------
  cli::cmd complete{.name  = "complete",
                    .desc  = "Atomically end the work session: task \xE2\x86\x92 done, claim \xE2\x86\x92 completed.",
                    .flags = {claim_flag("Claim token returned by pull/claim"),
                              {.long_name  = "--summary",
                               .desc       = "Free-text completion summary recorded on the action",
                               .value_kind = cli::kind::string},
                              no_locality_probe_flag("Skip the git locality probe and commit collection"),
                              json_flag()}};

  cli::cmd fail{.name  = "fail",
                .desc  = "Atomically fail the work session: task \xE2\x86\x92 todo, claim \xE2\x86\x92 aborted.",
                .flags = {claim_flag("Claim token returned by pull/claim"),
                          {.long_name  = "--reason",
                           .desc       = "Failure reason recorded on the claim and action",
                           .value_kind = cli::kind::string,
                           .required   = true},
                          {.long_name     = "--category",
                           .desc          = "Closed failure category (default: unknown)",
                           .value_kind    = cli::kind::choice,
                           .choices       = failure_categories(),
                           .default_value = std::string{"unknown"}},
                          no_locality_probe_flag("Skip the git locality probe and commit collection"),
                          json_flag()}};

  cli::cmd release{.name  = "release",
                   .desc  = "Graceful give-up: task \xE2\x86\x92 todo, claim \xE2\x86\x92 released (vs fail's aborted).",
                   .flags = {claim_flag("Claim token returned by pull/claim"),
                             {.long_name = "--reason", .desc = "Optional reason for releasing", .value_kind = cli::kind::string},
                             no_locality_probe_flag("Skip the git locality probe and commit collection"),
                             json_flag()}};

  cli::cmd block{
      .name  = "block",
      .desc  = "Atomically park the task on an external blocker.",
      .flags = {claim_flag("Claim token returned by pull/claim"),
                {.long_name  = "--blocker",
                 .desc       = "Task id of the blocker (entity_links target)",
                 .value_kind = cli::kind::integer,
                 .required   = true},
                {.long_name = "--reason", .desc = "Free-text reason recorded on the claim", .value_kind = cli::kind::string},
                no_locality_probe_flag("Skip the git locality probe and commit collection"),
                json_flag()}};

  // --- claim --------------------------------------------------------------
  std::vector<cli::flag> claim_flags;
  claim_flags.push_back({.long_name  = "--entity",
                         .desc       = "Entity ref: task:<id> | plan:<id> | plan_step:<id>",
                         .value_kind = cli::kind::string,
                         .required   = true});
  append(claim_flags, vendor_flags("Vendor tag (default: planar-agent)", "Vendor session id (e.g. claude:s1)"));
  claim_flags.push_back(
      {.long_name = "--role", .desc = "Role name (planner|coder|reviewer|test_coder|...)", .value_kind = cli::kind::string});
  claim_flags.push_back(
      {.long_name  = "--model",
       .desc       = "Model actually used, recorded verbatim as an opaque string. Never validated against a supported list.",
       .value_kind = cli::kind::string});
  claim_flags.push_back(ttl_flag("Lease TTL (default 600s; accepts bare int seconds or suffixed duration: 10m, 1h, 500ms)"));
  claim_flags.push_back(
      {.long_name = "--purpose", .desc = "Free-text purpose recorded on the claim", .value_kind = cli::kind::string});
  claim_flags.push_back(
      {.long_name = "--worktree", .desc = "Worktree id or path for isolation context", .value_kind = cli::kind::string});
  claim_flags.push_back({.long_name  = "--repo-root",
                         .desc       = "Absolute path of checkout to probe locality against",
                         .value_kind = cli::kind::string});
  claim_flags.push_back(no_locality_probe_flag("Skip the git locality probe"));
  claim_flags.push_back({.long_name     = "--no-transition",
                         .desc          = "Claim without changing task status (plan and plan_step are always unchanged)",
                         .value_kind    = cli::kind::boolean,
                         .default_value = false});
  claim_flags.push_back({.long_name     = "--force",
                         .desc          = "Take over an existing live claim (operator recovery)",
                         .value_kind    = cli::kind::boolean,
                         .default_value = false});
  append(claim_flags, run_stage_flags());
  claim_flags.push_back(json_flag());
  cli::cmd claim{.name  = "claim",
                 .desc  = "Direct entity claim; task claims atomically transition todo to doing by default.",
                 .flags = std::move(claim_flags)};

  // --- heartbeat ----------------------------------------------------------
  cli::cmd heartbeat{.name  = "heartbeat",
                     .desc  = "Refresh the lease on an active claim.",
                     .flags = {claim_flag("Claim token to refresh"),
                               ttl_flag("New TTL (default 600s; accepts bare int seconds or suffixed duration: 10m, 1h, 500ms)"),
                               {.long_name  = "--status",
                                .desc       = "Free-text status string recorded on the heartbeat action row's summary column",
                                .value_kind = cli::kind::string},
                               json_flag()}};

  // --- claim-associate ----------------------------------------------------
  cli::cmd claim_associate{
      .name  = "claim-associate",
      .desc  = "Associate a pre-acquired active claim with a workflow run (and optional stage). Used by an external workflow "
               "harness at dispatch time.",
      .flags = {claim_flag("Claim token to associate"),
                {.long_name  = "--run",
                 .desc       = "workflow_runs.id to stamp on the claim",
                 .value_kind = cli::kind::integer,
                 .required   = true},
                {.long_name  = "--stage",
                 .desc       = "Stage name to record (e.g. code, review); omit for NULL",
                 .value_kind = cli::kind::string},
                json_flag()}};

  // --- action start / end -------------------------------------------------
  cli::cmd action_start{
      .name  = "start",
      .desc  = "Start a nested action under a claim (child of the claim's role action).",
      .flags = {claim_flag("Claim token the action attaches to"),
                {.long_name  = "--kind",
                 .desc       = "Action kind (planner|coder|tool_call|heartbeat|...)",
                 .value_kind = cli::kind::string,
                 .required   = true},
                {.long_name = "--entity", .desc = "Optional entity ref kind:id", .value_kind = cli::kind::string},
                {.long_name = "--vendor-role", .desc = "Optional vendor role tag", .value_kind = cli::kind::string},
                {.long_name  = "--repo-root",
                 .desc       = "Absolute path of checkout to probe locality against",
                 .value_kind = cli::kind::string},
                no_locality_probe_flag("Skip the git locality probe"),
                {.long_name  = "--metadata",
                 .desc       = "Opaque text (typically JSON) persisted on the action row; validated as well-formed JSON when "
                               "supplied",
                 .value_kind = cli::kind::string},
                json_flag()}};
  cli::cmd action_end{.name  = "end",
                      .desc  = "Close a nested action started under a claim.",
                      .flags = {{.long_name  = "--action",
                                 .desc       = "Action id returned by `action start`",
                                 .value_kind = cli::kind::integer,
                                 .required   = true},
                                // A plain string with a hand-rolled validator, NOT a choice
                                // — the oracle's declaration. Making it a choice would add
                                // the value set to the help page and change its bytes.
                                {.long_name     = "--outcome",
                                 .desc          = "ok | error | aborted | timeout (default ok)",
                                 .value_kind    = cli::kind::string,
                                 .default_value = std::string{"ok"}},
                                {.long_name  = "--summary",
                                 .desc       = "Optional free-text summary recorded on the action",
                                 .value_kind = cli::kind::string},
                                json_flag()}};
  cli::cmd action{
      .name = "action", .desc = "Nested action lifecycle (sub-tool-calls inside a claim).", .cmds = {action_start, action_end}};

  // --- reconcile / abort --------------------------------------------------
  cli::cmd reconcile{
      .name  = "reconcile",
      .desc  = "Operator recovery: mark expired claims stale, close orphaned actions, abandon dead runs.",
      .flags = {{.long_name     = "--dry-run",
                 .desc          = "Report candidates without writing",
                 .value_kind    = cli::kind::boolean,
                 .default_value = false},
                {.long_name     = "--stale-after",
                 .desc          = "Additional grace beyond lease expiry (default 0s; accepts bare int seconds or suffixed "
                                  "duration: 10m, 1h, 500ms)",
                 .value_kind    = cli::kind::string,
                 .default_value = std::string{"0"}},
                // Sentinel-zero default, where `--plan` right below is a
                // plain optional. Both mean "global sweep"; the asymmetry is
                // the oracle's and is visible in help (`default=0`).
                {.long_name     = "--session",
                 .desc          = "Scope the sweep to a single session id (0 = global sweep, the default)",
                 .value_kind    = cli::kind::integer,
                 .default_value = std::int64_t{0}},
                {.long_name  = "--plan",
                 .desc       = "Scope the sweep to claims/actions/runs belonging to this plan id (0 or absent = global sweep)",
                 .value_kind = cli::kind::integer},
                {.long_name  = "--category",
                 .desc       = "Optional closed failure category applied to claims made stale",
                 .value_kind = cli::kind::choice,
                 .choices    = failure_categories()},
                json_flag()}};

  std::vector<cli::flag> abort_flags;
  abort_flags.push_back(claim_flag("Claim token to force-release"));
  abort_flags.push_back(
      {.long_name = "--reason", .desc = "Optional reason recorded on the claim and audit row", .value_kind = cli::kind::string});
  // No default here, unlike `fail`'s "unknown" — an abort records a
  // category only when the operator names one.
  abort_flags.push_back({.long_name  = "--category",
                         .desc       = "Optional closed failure category for the recovered claim",
                         .value_kind = cli::kind::choice,
                         .choices    = failure_categories()});
  append(abort_flags, vendor_flags("Vendor tag for the aborting session", "Vendor session id for the aborting session"));
  abort_flags.push_back(json_flag());
  cli::cmd abort{.name  = "abort",
                 .desc  = "Operator force-release of a stuck claim (any session, not just the owner).",
                 .flags = std::move(abort_flags)};

  // No `long_desc`, and that is READ OFF THE RENDERED PAGE rather than off
  // the schema catalog. `planar-agent schema` reports "summary" and
  // "description" as the same string, which looks like a long_desc equal to
  // the desc — but the Zig emitter FALLS BACK to desc when long_desc is
  // empty, so the catalog cannot tell the two apart. The oracle's
  // `--help` can: it renders the sentence INDENTED by two spaces, which is
  // `planar.cli.help`'s desc branch; the long_desc branch emits flush left
  // (as planar-watch's genuinely-distinct long_desc does). Setting
  // long_desc here would silently shift the line two columns.
  // Declaration ORDER matches the oracle's `handlers/cmd.zig` registry
  // exactly, because the root help page lists commands in tree order and
  // that page is compared line-for-line where the two trees agree. The
  // four verbs this port has not landed — `ingest`, `run`, `dispatch`,
  // `context` — are simply absent (see this file's header for why an
  // absent child beats a registered stub), so the root page lists
  // fourteen where the oracle lists eighteen.
  return cli::cmd{
      .name = "planar-agent",
      .desc = "Agent-callable coordination binary (pull / claim / complete / heartbeat / reconcile).",
      .cmds = {version, pull, peek, complete, fail, release, block, claim, heartbeat, claim_associate, action, reconcile, abort,
               schema},
  };
}

auto forbidden_verbs() -> std::vector<std::string_view> {
  return {"plan", "task",      "decision", "question", "scenario", "artifact", "annotate", "init",      "workbench", "doc",
          "spec", "templates", "ext",      "sync",     "promote",  "demote",   "capture",  "dashboard", "tree",      "health"};
}

} // namespace planar::cmd::agent
