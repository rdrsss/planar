/// @file surface.cpp
/// @brief Implementation of `planar.cmd.planar.surface` — the SHRINKING
/// remainder of the generated `node_spec` table.
///
/// ## No longer regenerable, and being folded away
///
/// `scripts/gen-cli-surface.py` produced this file from the Zig oracle's
/// `planar schema` catalog. The oracle was deleted at the M10 cutover
/// (task 6045), so the script cannot run and this file has been
/// hand-maintained ground truth since task 6267 — which is what makes
/// decision 1068's fold a move of authored code rather than a fight with a
/// generator.
///
/// M11.3 folds these entries, a wave of top-level domains per commit, into
/// hand-written declarations next to their handlers under `handlers/`. The
/// `plan` and `task` domains went first (task 6631), then `models`,
/// `annotate` and `workbench` (task 6632), then the drafting quartet
/// `question`, `scenario`, `decision` and `artifact` (task 6633), then
/// `assoc`, `workspace`, `handoff`, `capture`, `templates` and `bench`
/// (task 6634), then `scope`, `audit`, `config`, `local`, `links`, `run`
/// and `feedback` (task 6635); their entries are gone except for
/// twenty-two ORDERING ANCHORS in `surface_nodes()`, which that function's
/// own comment explains. The
/// LAST wave deletes this file once
/// `surface_nodes()` is empty; `surface_summaries()`,
/// `surface_empty_string_defaults()` and `unported_paths()` move to
/// `surface.cppm` and STAY — 57 `planar` nodes carry a summary that differs
/// from their description, so unlike `planar-agent` (task 6614) this
/// binary's summary table is genuine and deleting it would change the
/// pinned catalog.

module planar.cmd.planar.surface;

import std;
import planar.cliapp.surface;

namespace planar::cmd {

using cliapp::flag_spec;
using cliapp::node_spec;
using cliapp::positional_spec;

namespace {

constexpr std::string_view k_path_0[]   = {"init"};
constexpr std::string_view k_path_1[]   = {"scope"};
constexpr std::string_view k_path_2[]   = {"assoc"};
constexpr std::string_view k_path_3[]   = {"plan"};
constexpr std::string_view k_path_4[]   = {"task"};
constexpr std::string_view k_path_5[]   = {"question"};
constexpr std::string_view k_path_6[]   = {"scenario"};
constexpr std::string_view k_path_7[]   = {"decision"};
constexpr std::string_view k_path_8[]   = {"artifact"};
constexpr std::string_view k_path_9[]   = {"annotate"};
constexpr std::string_view k_path_10[]  = {"promote"};
constexpr std::string_view k_path_11[]  = {"demote"};
constexpr std::string_view k_path_12[]  = {"workbench"};
constexpr std::string_view k_path_13[]  = {"workspace"};
constexpr std::string_view k_path_15[]  = {"link"};
constexpr std::string_view k_path_16[]  = {"unlink"};
constexpr std::string_view k_path_17[]  = {"links"};
constexpr std::string_view k_path_19[]  = {"resume"};
constexpr std::string_view k_path_20[]  = {"handoff"};
constexpr std::string_view k_path_21[]  = {"capture"};
constexpr std::string_view k_path_22[]  = {"audit"};
constexpr std::string_view k_path_23[]  = {"health"};
constexpr std::string_view k_path_24[]  = {"models"};
constexpr std::string_view k_path_25[]  = {"dashboard"};
constexpr std::string_view k_path_26[]  = {"spec"};
constexpr std::string_view k_path_27[]  = {"test-spec"};
constexpr std::string_view k_path_28[]  = {"config"};
constexpr std::string_view k_path_29[]  = {"templates"};
constexpr std::string_view k_path_30[]  = {"tree"};
constexpr std::string_view k_path_31[]  = {"search"};
constexpr std::string_view k_path_32[]  = {"local"};
constexpr std::string_view k_path_33[]  = {"skills"};
constexpr std::string_view k_path_34[]  = {"import"};
constexpr std::string_view k_path_35[]  = {"synthesize"};
constexpr std::string_view k_path_36[]  = {"version"};
constexpr std::string_view k_path_37[]  = {"completion"};
constexpr std::string_view k_path_38[]  = {"schema"};
constexpr std::string_view k_path_39[]  = {"report"};
constexpr std::string_view k_path_40[]  = {"bench"};
constexpr std::string_view k_path_41[]  = {"closure"};
constexpr std::string_view k_path_42[]  = {"run"};
constexpr std::string_view k_path_43[]  = {"groups"};
constexpr std::string_view k_path_44[]  = {"explore"};
constexpr std::string_view k_path_45[]  = {"workflow"};
constexpr std::string_view k_path_46[]  = {"feedback"};
constexpr std::string_view k_path_174[] = {"resume", "validate"};
constexpr std::string_view k_path_193[] = {"health", "hygiene"};
constexpr std::string_view k_path_199[] = {"spec", "ingest"};
constexpr std::string_view k_path_200[] = {"test-spec", "status"};
constexpr std::string_view k_path_223[] = {"closure", "compute"};
constexpr std::string_view k_path_224[] = {"closure", "show"};
constexpr std::string_view k_path_229[] = {"groups", "recommend"};
constexpr std::string_view k_path_230[] = {"workflow", "list"};
constexpr std::string_view k_path_231[] = {"workflow", "show"};
constexpr std::string_view k_path_232[] = {"workflow", "run"};

constexpr flag_spec k_flags_0[] = {
    {.name          = "--name",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Project name (defaults to repo dir)"},
    {.name          = "--slug",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Explicit project slug (defaults to a slug derived from the directory name); with --force, targets that "
                      "registration for repoint"},
    {.name          = "--skip-project",
     .kind          = "bool",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Only init the DB; skip project registration"},
    {.name          = "--allow-no-repo",
     .kind          = "bool",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Allow initialization outside a git repo"},
    {.name          = "--force",
     .kind          = "bool",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Overwrite an existing project registration"},
    {.name          = "--json",
     .kind          = "bool",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Emit machine-readable output"},
};
constexpr flag_spec k_flags_10[] = {
    {.name          = "--to",
     .kind          = "string",
     .required      = true,
     .list          = false,
     .default_value = {},
     .description   = "Target association slug"},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_11[] = {
    {.name          = "--from",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Source association slug"},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_15[] = {
    {.name          = "--to",
     .kind          = "string",
     .required      = true,
     .list          = false,
     .default_value = {},
     .description   = "<system-slug>:<external-id>"},
    {.name          = "--role",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Link role: mirror, parent, child, reference (default: reference)"},
    {.name          = "--sync",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Sync direction: read-only, write-back, two-way (default: read-only)"},
    {.name          = "--propagate",
     .kind          = "bool",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Propagate feature after linking (M10)"},
    {.name = "--scope", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_16[] = {
    {.name          = "--scope",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Scope for the cross-scope guard (currently informational)"},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_19[] = {
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_23[] = {
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_25[] = {
    {.name          = "--scope",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Limit to a single scope slug"},
    {.name          = "--agents",
     .kind          = "bool",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Fold in live claim state + next-available-work per plan"},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_30[] = {
    {.name          = "--scope",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Limit to a single scope slug"},
    {.name          = "--all-scopes",
     .kind          = "bool",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Include every scope"},
    {.name          = "--depth",
     .kind          = "int",
     .required      = false,
     .list          = false,
     .default_value = "-1",
     .description   = "Max tree depth (-1 = unbounded)"},
    {.name          = "--kind",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Restrict to a single kind"},
    {.name          = "--status",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Restrict to a single status"},
    {.name = "--sort", .kind = "string", .required = false, .list = false, .default_value = {}, .description = "Sort key"},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_31[] = {
    {.name          = "--kind",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Restrict to a single kind"},
    {.name          = "--status",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Restrict to a single status"},
    {.name          = "--scope",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Restrict to a scope slug"},
    {.name          = "--plan",
     .kind          = "int",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Restrict to a plan id"},
    {.name = "--limit", .kind = "int", .required = false, .list = false, .default_value = "50", .description = "Max results"},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_34[] = {
    {.name          = "--from-github",
     .kind          = "bool",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Pull source from GitHub issues"},
    {.name = "--dry-run", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--strict", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name          = "--threshold",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Similarity threshold, e.g. 0.7"},
    {.name          = "--roadmap",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Path to a roadmap source"},
    {.name = "--apply", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--apply-removals", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--no-status-inference", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--interpret", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name          = "--accept-spec",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Non-interactive forward-spec selection — slug, comma-separated slugs, or 'all'"},
    {.name          = "--no-forward-specs",
     .kind          = "bool",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Skip forward-spec processing entirely"},
    {.name = "--scope", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_35[] = {
    {.name = "--apply", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--apply-removals", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--scope", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--code-layout", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--treat-as-greenfield", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name          = "--treat-as-nongreenfield",
     .kind          = "bool",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = ""},
    {.name          = "--threshold",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Similarity threshold, e.g. 0.7"},
    {.name = "--literal", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name          = "--accept-spec",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Non-interactive forward-spec selection — slug, comma-separated slugs, or 'all'"},
    {.name          = "--no-forward-specs",
     .kind          = "bool",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Skip forward-spec processing entirely"},
    {.name = "--dry-run", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_39[] = {
    {.name          = "--days",
     .kind          = "int",
     .required      = false,
     .list          = false,
     .default_value = "30",
     .description   = "Window in days (must be > 0, default 30)."},
    {.name          = "--tail",
     .kind          = "int",
     .required      = false,
     .list          = false,
     .default_value = "20",
     .description   = "Number of failure-tail rows (must be > 0, default 20)."},
    {.name          = "--json",
     .kind          = "bool",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Emit stable machine-readable JSON."},
};
constexpr flag_spec k_flags_44[] = {
    {.name          = "--plan",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Seed initial focus on this plan ID"},
    {.name          = "--task",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Seed initial focus on this task ID"},
    {.name          = "--scope",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Seed scope filter"},
    {.name          = "--plain",
     .kind          = "bool",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Fall back to help/usage instead of launching the cockpit"},
};
// k_flags_161-165 (ext list/test/create/propagate-one/propagate) removed
// at plan 996, task 6419 — moved to `planar-ext`.
// k_flags_170-173 (sync pull/push/status/resolve) removed at plan 996,
// task 6419 — moved to `planar-ext`.
constexpr flag_spec k_flags_193[] = {
    {.name          = "--scope",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Limit findings to one association slug"},
    {.name          = "--stale-doing",
     .kind          = "int",
     .required      = false,
     .list          = false,
     .default_value = "7",
     .description   = "Doing-task age threshold in days"},
    {.name          = "--stale-open",
     .kind          = "int",
     .required      = false,
     .list          = false,
     .default_value = "30",
     .description   = "Open-question age threshold in days"},
};
constexpr flag_spec k_flags_199[] = {
    {.name = "--apply", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--apply-removals", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--format", .kind = "string", .required = false, .list = false, .default_value = "text", .description = ""},
    {.name = "--scope", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--strict", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_200[] = {
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_223[] = {
    {.name = "--scope", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_224[] = {
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_229[] = {
    {.name = "--budget", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--solver", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_230[] = {
    {.name = "--local", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_231[] = {
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_232[] = {
    {.name          = "--phase",
     .kind          = "string",
     .required      = true,
     .list          = false,
     .default_value = {},
     .description   = "Phase function to invoke inside the workflow."},
    {.name          = "--args",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "JSON args blob forwarded to planar-execute --args."},
    {.name          = "--worktree",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Worktree directory forwarded to planar-execute --worktree."},
    {.name          = "--sandbox-root",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Sandbox root forwarded to planar-execute --sandbox-root."},
    {.name          = "--local",
     .kind          = "bool",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Restrict resolution to sandbox (local) workflows only."},
};
// k_flags_245/246 (ext register jira/github) removed at plan 996, task
// 6419 — moved to `planar-ext`.

constexpr positional_spec k_pos_10[] = {
    {.name = "ref", .required = true, .description = "Entity ref (kind:id)"},
};
constexpr positional_spec k_pos_11[] = {
    {.name = "ref", .required = true, .description = "Entity ref (kind:id)"},
};
constexpr positional_spec k_pos_15[] = {
    {.name = "ref", .required = true, .description = "Entity ref (kind:id)"},
};
constexpr positional_spec k_pos_16[] = {
    {.name = "link-id", .required = true, .description = "External-link id (integer)"},
};
constexpr positional_spec k_pos_19[] = {
    {.name = "task-id", .required = false, .description = ""},
};
constexpr positional_spec k_pos_31[] = {
    {.name = "query", .required = true, .description = "FTS5 query string"},
};
constexpr positional_spec k_pos_34[] = {
    {.name = "repo-root", .required = true, .description = ""},
};
constexpr positional_spec k_pos_35[] = {
    {.name = "repo-root", .required = true, .description = ""},
};
constexpr positional_spec k_pos_37[] = {
    {.name = "shell", .required = true, .description = "Shell: bash, zsh, or fish"},
};
// k_pos_162-165 (ext test/create/propagate-one/propagate) removed at plan
// 996, task 6419 — moved to `planar-ext`.
// k_pos_170/171/173 (sync pull/push/resolve) removed at plan 996, task
// 6419 — moved to `planar-ext`.
constexpr positional_spec k_pos_174[] = {
    {.name = "task-id", .required = true, .description = ""},
};
constexpr positional_spec k_pos_199[] = {
    {.name = "plan", .required = true, .description = ""},
};
constexpr positional_spec k_pos_200[] = {
    {.name = "plan", .required = true, .description = "Plan slug or numeric id"},
};
constexpr positional_spec k_pos_223[] = {
    {.name = "task-id", .required = true, .description = ""},
};
constexpr positional_spec k_pos_224[] = {
    {.name = "task-id", .required = true, .description = ""},
};
constexpr positional_spec k_pos_229[] = {
    {.name = "plan-id", .required = true, .description = ""},
};
constexpr positional_spec k_pos_231[] = {
    {.name = "name", .required = true, .description = ""},
};
constexpr positional_spec k_pos_232[] = {
    {.name = "name", .required = true, .description = ""},
};
// k_pos_245/246 (ext register jira/github) removed at plan 996, task 6419
// — moved to `planar-ext`.

} // namespace

/// @brief Every command node `planar` declares, as a flat list.
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
      {.path        = k_path_0,
       .description = "Initialize the Planar database and register the current directory as a project.",
       .flags       = k_flags_0,
       .positionals = {},
       .group       = false},
      // ORDERING ANCHOR for `scope` (M11.3e, task 6635) -- see the block
      // below, above `plan`'s anchor, for what this entry is and is not.
      {.path = k_path_1, .description = {}, .flags = {}, .positionals = {}, .group = true},
      // ORDERING ANCHOR for `assoc` (M11.3d, task 6634) -- see the block
      // below, above `plan`'s anchor, for what this entry is and is not.
      {.path = k_path_2, .description = {}, .flags = {}, .positionals = {}, .group = true},
      // ORDERING ANCHORS (M11.3, tasks 6631-6635). `plan` and `task`
      // (here), and `scope` and `assoc` (just above), and `question`,
      // `scenario`, `decision`, `artifact`, `annotate`, `workbench`,
      // `workspace`, `links`, `handoff`, `capture`, `audit`, `models`,
      // `config`, `templates`, `local`, `bench`, `run` and `feedback`
      // (further down, at their own catalog positions) are declared by hand next to
      // handlers, so `apply_surface` finds them already present and its
      // find-or-create arm skips these entries entirely. They carry NO
      // payload for that reason -- were the hand declaration ever lost, a
      // node created from one of these would be visibly empty and the
      // surface gate would say so, rather than a stale duplicate quietly
      // standing in.
      //
      // They are retained ONLY for the SECOND pass. `reorder_children`
      // rebuilds each parent's child order from THIS list, and a name
      // missing from it is never re-appended, so it drifts to the front of
      // the root's children -- which would move each folded domain off its
      // catalog position (scope 2nd, assoc 3rd, plan 4th, task 5th,
      // question 6th, scenario 7th, decision 8th, artifact 9th, annotate
      // 10th, workbench 13th, workspace 14th, links 17th, handoff 19th,
      // capture 20th, audit 21st, models 23rd, config 27th, templates
      // 28th, local 31st, bench 39th, run 41st, feedback 45th) and change
      // the pinned surface. Each remaining
      // M11.3 wave must retain its top-level entry the same way; the last
      // wave deletes this file, and with it the `apply_surface` call, at
      // which point `tree.cpp`'s declaration order is authoritative.
      //
      // An anchor is needed at every depth a fold empties, not just the
      // root -- but only where a SURVIVING sibling list would otherwise
      // lose a member. `models registry`, `workspace routing` and
      // `feedback triage` keep NO anchor because `models`, `workspace` and
      // `feedback` are folded whole, so nothing in this list names a child
      // of any of them and `reorder_children` never visits those parents.
      // A domain folded only PARTIALLY would need child-level anchors too;
      // no wave has done that.
      {.path = k_path_3, .description = {}, .flags = {}, .positionals = {}, .group = true},
      {.path = k_path_4, .description = {}, .flags = {}, .positionals = {}, .group = true},
      // ORDERING ANCHORS for the drafting quartet (M11.3c, task 6633) --
      // see the block above `plan`'s anchor for what these entries are and
      // are not. All 40 children of these four groups are now declared in
      // `handlers/{question,scenario,decision,artifact}.cpp`; only the
      // four top-level names survive here, stripped, to hold catalog
      // positions 6-9.
      {.path = k_path_5, .description = {}, .flags = {}, .positionals = {}, .group = true},
      {.path = k_path_6, .description = {}, .flags = {}, .positionals = {}, .group = true},
      {.path = k_path_7, .description = {}, .flags = {}, .positionals = {}, .group = true},
      {.path = k_path_8, .description = {}, .flags = {}, .positionals = {}, .group = true},
      // ORDERING ANCHOR for `annotate` (M11.3b, task 6632) -- see the block
      // above `plan`'s anchor for what this entry is and is not.
      {.path = k_path_9, .description = {}, .flags = {}, .positionals = {}, .group = true},
      {.path        = k_path_10,
       .description = "Promote an entity from its current scope to a named association.\n\n  Valid entity kinds: plan, task, "
                      "question, test_scenario (alias:\n  scenario), artifact, decision.\n\n  Examples:\n    planar promote "
                      "task:42 --to org:acme\n    planar promote plan:7 --to project:planar",
       .flags       = k_flags_10,
       .positionals = k_pos_10,
       .group       = false},
      {.path = k_path_11,
       .description =
           "Reverse a promotion — move an entity back to global personal scope.\n\n  The destination is always global; the "
           "optional --from flag names the\n  source association slug for clarity. Association-to-association\n  transitions go "
           "through promote.\n\n  Example:\n    planar demote task:42 --from project:planar",
       .flags       = k_flags_11,
       .positionals = k_pos_11,
       .group       = false},
      // ORDERING ANCHOR for `workbench` (M11.3b, task 6632) -- see the block
      // above `plan`'s anchor for what this entry is and is not.
      {.path = k_path_12, .description = {}, .flags = {}, .positionals = {}, .group = true},
      // ORDERING ANCHOR for `workspace` (M11.3d, task 6634) -- see the block
      // above `plan`'s anchor for what this entry is and is not. `workspace`
      // folds WHOLE, its nested `routing` group included, so it needs no
      // DEPTH-2 anchor: nothing in this list names a `workspace` child any
      // more, and `reorder_children` therefore never visits `workspace` or
      // `workspace routing` as a parent.
      {.path = k_path_13, .description = {}, .flags = {}, .positionals = {}, .group = true},
      // k_path_14 ("ext" group) removed at plan 996, task 6419 — the whole
      // family moved to `planar-ext`. See tree.cpp's note at `add_unlink`.
      {.path        = k_path_15,
       .description = "Manually record an external_links row linking a local entity to\n  an already-existing external ticket. "
                      "Use this when the external\n  ticket was created outside of 'ext create'. Does not push any data\n  to "
                      "the external system.\n\n  <kind:id> is a local entity reference, e.g. task:42, plan:7.",
       .flags       = k_flags_15,
       .positionals = k_pos_15,
       .group       = false},
      {.path = k_path_16,
       .description =
           "Remove an external_links row by its link id.\n\n  Associated sync_events rows are detached by setting link_id to "
           "null\n  rather than cascade-deleted; they are no longer reachable through\n  the deleted link's audit trail.",
       .flags       = k_flags_16,
       .positionals = k_pos_16,
       .group       = false},
      // ORDERING ANCHOR for `links` (M11.3e, task 6635) -- see the block
      // above `plan`'s anchor for what this entry is and is not.
      {.path = k_path_17, .description = {}, .flags = {}, .positionals = {}, .group = true},
      // k_path_18 ("sync" group) removed at plan 996, task 6419 — the whole
      // family moved to `planar-ext`.
      {.path = k_path_19,
       .description =
           "Produce a structured 8-section resume packet for the specified\n  task.\n\n  The packet contains:\n    1. Identity   "
           "    — task id, plan id, title, scope\n    2. State          — status, next_action, last action\n    3. Plan position "
           " — parent plan, completed/current/remaining steps\n    4. Operational    — external_links for the task; refreshed if "
           "stale\n    5. Recent activity — session entries from recent sessions\n    6. Decisions and questions\n    7. Linked "
           "artifacts\n    8. Audit footer   — previous session vendor and timestamp, plus\n                        the active "
           "claim's worktree path (when held)\n                        so the resumer can prepend `cd <path>`",
       .flags       = k_flags_19,
       .positionals = k_pos_19,
       .group       = true},
      // ORDERING ANCHORS for `handoff` and `capture` (M11.3d, task 6634) --
      // see the block above `plan`'s anchor. Both were the most heavily
      // SHADOWED nodes in the tree before that wave: every one of their
      // thirteen children was hand-declared in `tree.cpp` as well as
      // described here, and `apply_surface` skipped this half entirely.
      {.path = k_path_20, .description = {}, .flags = {}, .positionals = {}, .group = true},
      {.path = k_path_21, .description = {}, .flags = {}, .positionals = {}, .group = true},
      // ORDERING ANCHOR for `audit` (M11.3e, task 6635).
      {.path = k_path_22, .description = {}, .flags = {}, .positionals = {}, .group = true},
      {.path = k_path_23,
       .description =
           "Check database reachability, schema version currency, SQLite\n  integrity, in-flight task resumability, pending "
           "handoff staleness,\n  and manifest-owned installed projection freshness. This command is\n  read-only; recovery "
           "commands are reported but never run.\n\n  Exit codes:\n    0  all checks pass\n    1  degraded (in-flight tasks not "
           "resumable, stale handoffs, stale\n       or missing managed projections, integrity errors, etc.)",
       .flags       = k_flags_23,
       .positionals = {},
       .group       = true},
      // ORDERING ANCHOR for `models` (M11.3b, task 6632) -- see the block
      // above `plan`'s anchor for what this entry is and is not.
      {.path = k_path_24, .description = {}, .flags = {}, .positionals = {}, .group = true},
      {.path = k_path_25,
       .description =
           "Roll-up of in-flight plans in the current scope.\n\n  --agents folds in the live claim state from agent_work_claims "
           "—\n  active claims, stale claims, and the per-plan 'next available'\n  task list. Without --agents the dashboard is "
           "a plain plan summary.\n\n  This is the operator's read surface for agent activity; the\n  `planar agent` subcommand "
           "namespace does not exist by design.\n  See `planar-agent` for the ritual (claim/heartbeat/complete) and\n  "
           "`planar-watch` for the live streaming view.",
       .flags       = k_flags_25,
       .positionals = {},
       .group       = false},
      {.path = k_path_26,
       .description =
           "Commands for the planning pipeline spec surface.\n\n  'spec ingest' decomposes workbench planning documents into a\n "
           " structured task graph in the database.\n  'spec draft' generates initial spec artifacts from a goal statement.",
       .flags       = {},
       .positionals = {},
       .group       = true},
      {.path        = k_path_27,
       .description = "Commands for inspecting test-spec coverage of a plan's tasks.\n\n  'test-spec status' prints a "
                      "per-milestone breakdown of which tasks\n  have verifying scenarios. This is a read-only complement to "
                      "the\n  ingest-time coverage gate (see `planar spec ingest --strict`).",
       .flags       = {},
       .positionals = {},
       .group       = true},
      // ORDERING ANCHOR for `config` (M11.3e, task 6635).
      {.path = k_path_28, .description = {}, .flags = {}, .positionals = {}, .group = true},
      // ORDERING ANCHOR for `templates` (M11.3d, task 6634) -- see the block
      // above `plan`'s anchor for what this entry is and is not.
      {.path = k_path_29, .description = {}, .flags = {}, .positionals = {}, .group = true},
      {.path        = k_path_30,
       .description = "Render a hierarchical view of Planar entities for one or all\n  scopes.\n\n  Walks plans (via "
                      "parent_plan_id), tasks (via plan_id and\n  parent_task_id), and entity_links(derives-from) to gather\n  "
                      "artifacts, decisions, scenarios, and questions linked to each plan.",
       .flags       = k_flags_30,
       .positionals = {},
       .group       = false},
      {.path        = k_path_31,
       .description = "Run a full-text search across every searchable entity kind.\n\n  Queries are passed to SQLite's FTS5 "
                      "MATCH operator directly.\n  Multi-word queries are AND'd unless the operator is given\n  explicitly (OR, "
                      "NOT, NEAR, \"phrase\"). Tokens are unicode61-folded\n  (case-insensitive, diacritic-stripped).",
       .flags       = k_flags_31,
       .positionals = k_pos_31,
       .group       = false},
      // ORDERING ANCHOR for `local` (M11.3e, task 6635).
      {.path = k_path_32, .description = {}, .flags = {}, .positionals = {}, .group = true},
      {.path        = k_path_33,
       .description = "The unified skill source tree under skills/src/ is rendered by the\n  external scriptorium binary (plan "
                      "918). Planar no longer renders vendor\n  projections nor tracks their install-drift in-band; use "
                      "`scriptorium\n  check`/`scriptorium status` instead. This command has no subcommands.",
       .flags       = {},
       .positionals = {},
       .group       = false},
      {.path        = k_path_34,
       .description = "import translates the planning artefacts of an existing\n  repository into Planar's data model. It "
                      "discovers\n  tech specs, roadmap milestones, ADRs, and backlog files,\n  infers completion status from "
                      "checkbox state and git history,\n  and produces an ImportPlan for review before committing.",
       .flags       = k_flags_34,
       .positionals = k_pos_34,
       .group       = false},
      {.path        = k_path_35,
       .description = "synthesize reads a repository's existing planning docs, source\n  code, and git history AS INPUT for an "
                      "LLM synthesis pass. It\n  produces fresh product-spec / tech-spec / roadmap artifacts (NOT a\n  verbatim "
                      "transcription) and proposes them via the same workbench\n  pipeline as the planner agent.",
       .flags       = k_flags_35,
       .positionals = k_pos_35,
       .group       = false},
      {.path        = k_path_36,
       .description = "Print the planar version, commit, and C++ toolchain.",
       .flags       = {},
       .positionals = {},
       .group       = false},
      {.path        = k_path_37,
       .description = "Generate the autocompletion script for the specified shell.",
       .flags       = {},
       .positionals = k_pos_37,
       .group       = false},
      {.path        = k_path_38,
       .description = "Print the full command tree as a JSON catalog (flags, aliases, positionals).",
       .flags       = {},
       .positionals = {},
       .group       = false},
      {.path        = k_path_39,
       .description = "Reads the cli_invocations capture log and the always-on observability\ntables (agent_actions, "
                      "sync_events, agent_work_claims, handoffs) and\nrenders a diagnostic bundle.\n\nInvocation and failure "
                      "sections render \"logging disabled\" when\n[introspection].cli_log is off; the always-on sections "
                      "(actions, sync,\nclaims, claim failure categories, handoffs, health) render normally in\neither "
                      "case.\n\nThe bundle is structurally redacted: queries select only counts,\ncategories, verb paths, "
                      "statuses, and timestamps — never entity text.\n\nExit codes:\n  0   bundle rendered successfully.\n  2   "
                      "invalid flag value (--days or --tail must be a positive integer).\n  1   database error.",
       .flags       = k_flags_39,
       .positionals = {},
       .group       = false},
      // ORDERING ANCHOR for `bench` (M11.3d, task 6634) -- see the block
      // above `plan`'s anchor for what this entry is and is not. `run`, the
      // sibling group that shares `bench`'s handlers and engine, is NOT
      // folded yet and still carries its full entries below.
      {.path = k_path_40, .description = {}, .flags = {}, .positionals = {}, .group = true},
      {.path        = k_path_41,
       .description = "Compute the *derived* closure of a task — the symbols it must hold\nresident, computed by static analysis "
                      "from the task's declared seed\npaths (task_touch_paths), partitioned by role:\n\n  modify     — the "
                      "seed's own edited symbols.\n  reference  — the interfaces the seed depends on.\n  transitive — deeper "
                      "hops (stored, but excluded from the effective\n               closure by default).\n\n  Workflow: closure "
                      "compute <task-id> → closure show <task-id> --json.",
       .flags       = {},
       .positionals = {},
       .group       = true},
      // ORDERING ANCHOR for `run` (M11.3e, task 6635). `run` and `bench`
      // share `handlers/runs.cpp` but are separate top-level verbs; only
      // `run` was folded by this wave.
      {.path = k_path_42, .description = {}, .flags = {}, .positionals = {}, .group = true},
      {.path = k_path_43,
       .description =
           "Form **slices** — groups of a plan's open (todo) tasks that share a\ncontext window — by minimizing the duplicated "
           "closure across slices,\nsubject to a per-slice token budget. Read-only sibling to\n`plan recommend-strategy`: it "
           "reports a recommendation, writing nothing.\n\nEach slice reports its member task ids, its unioned effective "
           "closure\n(the distinct symbols the slice must hold resident, role modify ∪\nreference), and that union's token cost. "
           "No slice's cost exceeds the\nbudget, and the slice-DAG induced by the task `blocks` dependencies is\nalways "
           "schedulable (no slice is grouped across a dependency violation).\n\n  --solver greedy|mtkahypar  (default greedy) "
           "selects the partitioner.\n  `mtkahypar` is the optional external hypergraph solver: when its binary\n  is absent or "
           "fails, the verb degrades to greedy and reports\n  `optimal_available:false` (it never errors on a missing optional "
           "dep).\n\n  Workflow: closure compute <task> (per task) → groups recommend <plan>.",
       .flags       = {},
       .positionals = {},
       .group       = true},
      {.path = k_path_44,
       .description =
           "Launch the interactive Planar cockpit.\n\n  Equivalent to invoking `planar` with no verb on a terminal. Use\n  "
           "`planar explore` when you want to force-launch the cockpit by name,\n  or from a context where bare-invocation "
           "detection may not fire.\n\n  --plan, --task, and --scope seed the initial focus.\n\n  Falls back to this help text "
           "when stdout is not a TTY, when TERM=dumb,\n  when PLANAR_NO_TUI is set, or when --plain is passed.",
       .flags       = k_flags_44,
       .positionals = {},
       .group       = false},
      {.path        = k_path_45,
       .description = "Enumerate, inspect, and invoke shipped and sandbox Lua workflows.\n\n  Shipped workflows live at "
                      "$PLANAR_HOME/workflows/ (default\n  ~/.planar/workflows/).  Sandbox workflows live at\n  "
                      "~/.planar/local/workflows/ and are marked `local`.\n\n  These commands are READ-ONLY w.r.t. SQLite.  "
                      "`run` delegates\n  execution to `planar-execute` and forwards its output + exit code.",
       .flags       = {},
       .positionals = {},
       .group       = true},
      // ORDERING ANCHOR for `feedback` (M11.3e, task 6635). Its nested
      // `triage` group needs no anchor of its own: `feedback` folds whole,
      // so nothing in this list names a `feedback` child and
      // `reorder_children` never visits that parent.
      {.path = k_path_46, .description = {}, .flags = {}, .positionals = {}, .group = true},
      // k_path_160-165 (ext register group, list, test, create,
      // propagate-one, propagate) removed at plan 996, task 6419. The first
      // five moved to `planar-ext`; `propagate` was never wired here (see
      // `unported_paths`'s header) and is not wired on either binary yet.
      // k_path_170-173 (sync pull/push/status/resolve) removed at plan 996,
      // task 6419 — moved to `planar-ext`.
      {.path = k_path_174, .description = "Check if a task is resumable.", .flags = {}, .positionals = k_pos_174, .group = false},
      {.path        = k_path_193,
       .description = "Find draft plans with zero tasks or only terminal tasks, tasks\n  left doing beyond a threshold, and "
                      "questions left open beyond a\n  threshold. Suggested repair commands are reported but never run.\n\n  "
                      "This reporter always exits 0 when the report is produced, even when\n  findings are present.",
       .flags       = k_flags_193,
       .positionals = {},
       .group       = false},
      {.path        = k_path_199,
       .description = "Decompose workbench spec documents into the task graph.",
       .flags       = k_flags_199,
       .positionals = k_pos_199,
       .group       = false},
      {.path        = k_path_200,
       .description = "Print per-milestone test-spec coverage for an anchor plan.",
       .flags       = k_flags_200,
       .positionals = k_pos_200,
       .group       = false},
      {.path        = k_path_223,
       .description = "Run the extractor over a task's seeds and persist the closure.",
       .flags       = k_flags_223,
       .positionals = k_pos_223,
       .group       = false},
      {.path        = k_path_224,
       .description = "Read back a task's persisted closure rows.",
       .flags       = k_flags_224,
       .positionals = k_pos_224,
       .group       = false},
      {.path        = k_path_229,
       .description = "Recommend closure-minimizing task slices for a plan.",
       .flags       = k_flags_229,
       .positionals = k_pos_229,
       .group       = false},
      {.path        = k_path_230,
       .description = "List shipped and sandbox workflows.",
       .flags       = k_flags_230,
       .positionals = {},
       .group       = false},
      {.path        = k_path_231,
       .description = "Show @meta and source path for a named workflow.",
       .flags       = k_flags_231,
       .positionals = k_pos_231,
       .group       = false},
      {.path        = k_path_232,
       .description = "Resolve <name> across shipped and sandbox workflows, then exec\n  `planar-execute run <path> --phase "
                      "<phase> [--args <json>]\n  [--worktree <dir>] [--sandbox-root <dir>]`.  The workflow's\n  flow.result "
                      "JSON streams to stdout; the exit code is forwarded\n  exactly (non-zero on flow.fail or engine "
                      "error).\n\n  planar-execute resolution order: $PLANAR_EXECUTE_BIN →\n  sibling of argv[0] → PATH.",
       .flags       = k_flags_232,
       .positionals = k_pos_232,
       .group       = false},
      // k_path_245/246 (ext register jira/github) removed at plan 996, task
      // 6419 — moved to `planar-ext`.
  };
}

auto surface_empty_string_defaults() -> std::span<std::pair<std::string_view, std::string_view> const> {
  // Measured, not guessed: these are the ONLY flags across all three
  // oracle catalogs whose `"default"` is `""` rather than `null` or a
  // typed literal. Verified by diffing this binary's `schema` output
  // against `zig/zig-out/bin/planar schema` byte for byte — before task
  // 6130 those four values were the entire 8-byte difference.
  static constexpr std::pair<std::string_view, std::string_view> k_empty_defaults[] = {
      {"planar workbench edit", "--editor"},
      {"planar workflow run", "--args"},
      {"planar workflow run", "--worktree"},
      {"planar workflow run", "--sandbox-root"},
  };
  return k_empty_defaults;
}

auto surface_summaries() -> std::span<std::pair<std::string_view, std::string_view> const> {
  static constexpr std::pair<std::string_view, std::string_view> k_summaries[] = {
      {"planar", "Planning + agent operations CLI."},
      {"planar init", "Initialize the Planar database and register the current directory as a project."},
      {"planar scope", "Inspect the cwd-derived scope and suggest memberships."},
      {"planar scope show", "Show the cwd-derived scope (and any --scope override)."},
      {"planar scope suggest", "Suggest scope associations based on cwd."},
      {"planar scope use", "Removed in plan 153 M5 — see `planar scope show`."},
      {"planar scope pop", "Removed in plan 153 M5 — see `planar scope show`."},
      {"planar scope clear", "Removed in plan 153 M5 — see `planar scope show`."},
      {"planar assoc", "Manage associations (many-to-many scope tags for repos)."},
      {"planar assoc list", "List all known associations."},
      {"planar assoc create", "Create a new association."},
      {"planar assoc add", "Add a repo to an association."},
      {"planar assoc remove", "Remove a repo from an association."},
      {"planar assoc members", "List all project members of an association."},
      {"planar assoc detect", "Propose (or apply) auto-detected associations for the current directory."},
      {"planar plan", "Manage plans and plan steps."},
      {"planar plan create", "Create a new plan."},
      {"planar plan show", "Show a plan's details, steps, and child plans."},
      {"planar plan list", "List plans."},
      {"planar plan update", "Update mutable fields on a plan."},
      {"planar plan edit", "Edit a plan in $EDITOR (editor-first flow)."},
      {"planar plan view", "View a plan's workbench file."},
      {"planar plan diff", "Diff plan against database version."},
      {"planar plan review", "Reviewer entry point for plan diff."},
      {"planar plan link", "Create an entity link from a plan to another entity."},
      {"planar plan next", "Bucketed claim-aware view of next work on a plan (available / claimed / stale / blocked)."},
      {"planar plan recommend-strategy", "Recommend an execution strategy: compute the parallel-eligible subset of a plan's open "
                                         "tasks via the six parallelizability rules."},
      {"planar plan divergence", "Report the declared-vs-derived closure divergence for a plan's open tasks (decision D4)."},
      {"planar plan recompute-status", "Recompute a plan's roll-up status (--plan <id> or --all)."},
      {"planar plan closeout",
       "Delivery-evidence gate: report whether a plan is ready to close and (without --dry-run) mark it done."},
      {"planar plan step", "Manage plan steps."},
      {"planar plan step add", "Append a new step to a plan."},
      {"planar plan step list", "List steps of a plan."},
      {"planar plan step done", "Mark a plan step as done."},
      {"planar plan step skip", "Mark a plan step as skipped."},
      {"planar plan step link", "Associate a plan step with the task that materializes it."},
      {"planar plan descendants", "Emit the anchor plan's full subtree in dependency-topological order. READ-ONLY."},
      {"planar task", "Manage tasks."},
      {"planar task add", "Create a new task."},
      {"planar task show", "Show full task details."},
      {"planar task packet", "Compile the authoritative current routing packet for a task."},
      {"planar task list", "List tasks."},
      {"planar task update", "Update mutable fields on a task."},
      {"planar task edit", "Edit a task in $EDITOR (editor-first flow)."},
      {"planar task view", "View task's workbench file."},
      {"planar task diff", "Diff task against its database-stored version."},
      {"planar task review", "Reviewer entry point for task diff."},
      {"planar task done", "Mark a task as done (single-arg form; Go supports variadic)."},
      {"planar task cancel", "Cancel a task (single-arg form; Go supports variadic)."},
      {"planar task block", "Mark a task as blocked and record the blocking relationship."},
      {"planar task link", "Create an entity link from a task to another entity."},
      {"planar task reopen", "Reopen a done or cancelled task with an audit-trail entry."},
      {"planar task touches", "Manage repo-touches links on a task."},
      {"planar task touches add", "Link a task to a repo via a 'touches' relationship."},
      {"planar task touches infer", "Propose path-level touches from the task's own text (preview by default)."},
      {"planar task touches list", "List the repo- and path-level touches declared on a task."},
      {"planar task touches remove", "Remove a 'touches' link between a task and a repo."},
      {"planar question", "Manage questions."},
      {"planar question add", "Create a new question."},
      {"planar question edit", "Edit a question in $EDITOR (editor-first flow)."},
      {"planar question view", "View question's workbench file."},
      {"planar question diff", "Diff question against database version."},
      {"planar question review", "Reviewer entry point for question diff."},
      {"planar question answer", "Record an answer to a question."},
      {"planar question wontfix", "Mark a question as wontfix."},
      {"planar question list", "List questions."},
      {"planar question show", "Show a question's details."},
      {"planar question link", "Create an entity link from a question to another entity."},
      {"planar scenario", "Manage test scenarios."},
      {"planar scenario add", "Create a new test scenario."},
      {"planar scenario edit", "Edit a scenario in $EDITOR (editor-first flow)."},
      {"planar scenario view", "View scenario's workbench file."},
      {"planar scenario diff", "Diff scenario against database version."},
      {"planar scenario review", "Reviewer entry point for scenario diff."},
      {"planar scenario verify", "Record a test run for a scenario (--outcome pass|fail|error|skipped; defaults to pass)."},
      {"planar scenario retire", "Mark a scenario as retired."},
      {"planar scenario list", "List scenarios."},
      {"planar scenario show", "Show a scenario's details."},
      {"planar scenario link", "Create an entity link from a scenario to another entity."},
      {"planar decision", "Manage decision records."},
      {"planar decision add", "Create a new decision record."},
      {"planar decision show", "Show a decision's details."},
      {"planar decision list", "List decisions."},
      {"planar decision accept", "Accept a proposed decision."},
      {"planar decision supersede", "Mark a decision as superseded by a newer decision."},
      {"planar decision withdraw", "Withdraw a decision."},
      {"planar decision edit", "Edit a decision in $EDITOR (editor-first flow)."},
      {"planar decision view", "View decision's workbench file."},
      {"planar decision diff", "Diff decision against database version."},
      {"planar decision review", "Reviewer entry point for decision diff."},
      {"planar decision link", "Create an entity link from a decision to another entity."},
      {"planar artifact", "Manage artifacts (tech specs, ADRs, design notes, etc.)."},
      {"planar artifact add", "Register a new artifact."},
      {"planar artifact show", "Show an artifact's metadata and body."},
      {"planar artifact list", "List artifacts."},
      {"planar artifact update", "Update mutable fields on an artifact."},
      {"planar artifact edit", "Edit an artifact in $EDITOR (editor-first flow)."},
      {"planar artifact view", "View the artifact's workbench file in $PAGER."},
      {"planar artifact diff", "Show a unified diff between the DB's artifact content and the workbench file."},
      {"planar artifact review", "Reviewer entry point for artifact diff."},
      {"planar artifact link", "Create an entity link from an artifact to another entity."},
      {"planar annotate", "Manage source annotations."},
      {"planar annotate add", "Create a new annotation."},
      {"planar annotate show", "Show an annotation."},
      {"planar annotate list", "List annotations."},
      {"planar annotate update", "Update an annotation."},
      {"planar annotate remove", "Remove an annotation."},
      {"planar annotate tag", "Add or remove a tag on an annotation."},
      {"planar annotate resolve", "Mark an annotation as resolved."},
      {"planar annotate dismiss", "Dismiss an annotation."},
      {"planar annotate archive", "Archive an annotation."},
      {"planar annotate bulk-resolve", "Resolve every active annotation matching the filter."},
      {"planar annotate bulk-dismiss", "Dismiss every active annotation matching the filter."},
      {"planar annotate bulk-archive", "Archive every annotation matching the filter (including non-active rows)."},
      {"planar annotate verify", "Verify annotation anchors against workspace state."},
      {"planar annotate sweep", "Sweep stale annotations (resolved/dismissed older than --since-days)."},
      {"planar promote", "Promote an entity to an association scope."},
      {"planar demote", "Demote an entity back to global scope."},
      {"planar workbench", "Manage workbench sync for plan feature directories."},
      {"planar workbench lint", "Validate workbench Markdown frontmatter without syncing."},
      {"planar workbench pull", "Apply FS→DB changes; report DB→FS drift."},
      {"planar workbench push", "Apply DB→FS changes atomically; report FS→DB drift."},
      {"planar workbench status", "Show drift and conflicts without writing."},
      {"planar workbench resolve", "Settle a sync conflict by choosing FS or DB."},
      {"planar workbench sync", "Atomically apply FS and DB changes via a unified sync."},
      {"planar workbench archive", "Archive a feature's workbench filesystem tree."},
      {"planar workbench restore", "Restore an archived feature's workbench tree."},
      {"planar workbench gc", "Remove FS files whose backing entity is terminal in the DB."},
      {"planar workbench list", "List features with workbench trees."},
      {"planar workbench publish", "Render and push workbench files to external system."},
      {"planar workbench extract-questions", "Parse Open questions from top-level workbench specs (read-only)."},
      {"planar workbench edit", "Edit a feature's workbench files in $EDITOR."},
      {"planar workspace", "Manage workspace state directories and their AGENTS.md surfaces."},
      {"planar workspace init", "Initialize a workspace (org-level association)."},
      {"planar workspace doctor", "Scan and fix workspace registration and state consistency."},
      {"planar workspace routing", "Manage workspace routing table."},
      {"planar workspace routing build", "Build routing table from workspace membership."},
      {"planar workspace routing show", "Display current routing table."},
      {"planar workspace regenerate", "Regenerate AGENTS.md from current state."},
      {"planar ext", "Manage external operational-plane systems (Jira, GitHub Issues, etc.)."},
      {"planar ext register", "Register an external system."},
      {"planar ext register jira", "Register a Jira instance as an external system."},
      {"planar ext register github", "Register a GitHub Issues repository as an external system."},
      {"planar ext list", "List registered external systems."},
      {"planar ext test", "Test connection to an external system."},
      {"planar ext create", "Create an external counterpart for a local entity."},
      {"planar ext propagate-one", "Render + POST + record one entity counterpart; idempotent skip on existing link."},
      {"planar ext propagate", "Propagate a feature (plan + descendants) to an external system."},
      {"planar link", "Link a local entity to an external-system ticket."},
      {"planar unlink", "Remove an external_links row by link id."},
      {"planar links", "List or remove internal entity_links relationships."},
      {"planar links add", "Create an entity_links row between two entities."},
      {"planar links list", "List entity_links where the given entity is source or target."},
      {"planar links remove", "Delete an entity_links row by its id."},
      {"planar links trail", "Show the audit trail for an entity_links row."},
      {"planar sync", "Pull and push data between the local plane and external systems."},
      {"planar sync pull", "Pull remote state for one or more external links."},
      {"planar sync push", "Push local changes for one or more external links."},
      {"planar sync status", "Report sync status for links."},
      {"planar sync resolve", "Settle a sync conflict on a link."},
      {"planar resume", "Produce a structured resume packet for the specified task."},
      {"planar resume validate", "Check if a task is resumable."},
      {"planar handoff", "Capture a context snapshot and create a validated handoff record."},
      {"planar handoff create", "Create a handoff from an existing snapshot."},
      {"planar handoff validate", "Validate a pending handoff."},
      {"planar handoff consume", "Mark a handoff as consumed."},
      {"planar handoff abandon", "Abandon a non-terminal handoff."},
      {"planar handoff list", "List handoffs."},
      {"planar handoff show", "Show a handoff's details."},
      {"planar capture", "Manage explicit session capture."},
      {"planar capture session", "Open or reuse a session for the current (vendor, vendor-session-id) tuple."},
      {"planar capture commits", "Record explicit git commits into a session."},
      {"planar capture end", "End the active or specified session."},
      {"planar capture note", "Append a narrative note to the active session."},
      {"planar capture command", "Append a command to the active session."},
      {"planar capture file", "Attach a file to the active session."},
      {"planar capture snapshot", "Create a context snapshot."},
      {"planar audit", "Cross-plane audit trail commands."},
      {"planar audit trail",
       "Show audit history for an entity (audit_log + entity_links) or an external link (external_links + sync_events)."},
      {"planar audit commits", "List commits attributed to sessions and claims."},
      {"planar audit session", "Show the timeline for a session."},
      {"planar audit publish-decision", "Post the decision body to linked operational-plane targets."},
      {"planar audit handoff-readiness", "Check resume-readiness for all in-flight tasks."},
      {"planar health", "Report database, handoff, and installed-projection health."},
      {"planar health hygiene", "Report stale plan, task, and question lifecycle state without mutating it."},
      {"planar models", "Discover installed provider CLIs and their model catalogs."},
      {"planar models evals",
       "Aggregate completed dispatch outcomes into a per-(work-type, candidate) scorecard and preview-only recommendation."},
      {"planar models resolve", "Resolve a role's routing tier from its authoritative packet, or report the fallback and why."},
      {"planar models experiments", "List declared routing experiments and how much evidence each has produced."},
      {"planar models outcomes", "List recorded terminal outcomes, including excluded ones and why they were excluded."},
      {"planar models registry", "Manage opaque operator candidates and host observations."},
      {"planar models registry list", "List registrations, bindings, and latest observations."},
      {"planar models registry add", "Register one exact opaque candidate identifier."},
      {"planar models registry update", "Update enabled state and deterministic fallback order."},
      {"planar models registry remove", "Remove a candidate when no immutable evidence references it."},
      {"planar models registry bind", "Allow one role and tier for a candidate."},
      {"planar models registry unbind", "Remove one explicit role and tier binding."},
      {"planar models registry observe", "Append an exact, versioned host capability observation."},
      {"planar models registry eligibility", "Report every independent eligibility gate and named exclusion reason."},
      {"planar models registry verify-identity", "Compare requested and actual spawn identity without aliasing."},
      {"planar models registry export", "Export the versioned registry compatibility document."},
      {"planar dashboard", "Operator situational-awareness view of in-flight plans (and, with --agents, live claims)."},
      {"planar spec", "Spec pipeline commands (draft, ingest)."},
      {"planar spec ingest", "Decompose workbench spec documents into the task graph."},
      {"planar test-spec", "Test-spec coverage inspectors."},
      {"planar test-spec status", "Print per-milestone test-spec coverage for an anchor plan."},
      {"planar config", "Manage Planar configuration."},
      {"planar config show", "Print the resolved configuration."},
      {"planar config edit", "Edit the configuration file in $EDITOR."},
      {"planar config validate", "Validate configuration file syntax."},
      {"planar config init", "Initialize the configuration file."},
      {"planar config path", "Show the configuration file path."},
      {"planar templates", "Inspect, validate, and render Planar JSON templates."},
      {"planar templates list", "List available templates."},
      {"planar templates show", "Show a template's raw JSON."},
      {"planar templates render", "Render a template against a database entity (dry run; no writes)."},
      {"planar templates validate", "Validate template syntax."},
      {"planar templates init", "Extract default templates to disk."},
      {"planar templates path", "Show template resolution paths."},
      {"planar tree", "Render plans, tasks, artifacts, decisions, scenarios, and questions as a hierarchical tree."},
      {"planar search", "Full-text search across plans, tasks, questions, scenarios, decisions, and artifacts."},
      {"planar local", "Manage user-local sandbox skills and agents under ~/.planar/local/."},
      {"planar local list", "List locally-installed skills and agents."},
      {"planar local link", "Create or reuse symlinks from vendor paths to local source."},
      {"planar local unlink", "Remove symlinks from vendor paths."},
      {"planar local import", "Import a skill or agent from an external directory."},
      {"planar local migrate", "Migrate skills/agents to new Planar version."},
      {"planar skills", "Retired: rendering and drift detection now live in scriptorium."},
      {"planar import", "Import an existing repo's state into Planar."},
      {"planar synthesize", "Synthesize fresh planning artifacts from a repo's docs + code + git history."},
      {"planar version", "Print the planar version, commit, and C++ toolchain."},
      {"planar completion", "Generate the autocompletion script for the specified shell."},
      {"planar schema", "Print the full command tree as a JSON catalog (flags, aliases, positionals)."},
      {"planar report", "Emit the diagnostic bundle: invocation and closed claim-failure aggregates plus health metrics."},
      {"planar bench", "Record and query benchmark run data (measurement rig)."},
      {"planar bench start", "Mint a new run record and print its run_uid."},
      {"planar bench event", "Append a journal event to a run."},
      {"planar bench touch", "Record a declared or actual file touch for a run."},
      {"planar bench harvest", "Harvest git diff as actual touches for a run/task."},
      {"planar bench finish", "Set the terminal status on a run."},
      {"planar bench show", "Show a run's full state (header + events + touches)."},
      {"planar closure", "Compute and inspect a task's derived symbol-level closure."},
      {"planar closure compute", "Run the extractor over a task's seeds and persist the closure."},
      {"planar closure show", "Read back a task's persisted closure rows."},
      {"planar run", "Record and query operational run traces."},
      {"planar run start", "Mint a new operational run record and print its run_uid as JSON."},
      {"planar run event", "Append a journal event to a run (seq auto-incremented)."},
      {"planar run finish", "Set the terminal status on a run."},
      {"planar run show", "Show a run's full state (header + events)."},
      {"planar groups", "Recommend task slices that minimize closure replication."},
      {"planar groups recommend", "Recommend closure-minimizing task slices for a plan."},
      {"planar explore", "Launch the interactive cockpit (same as bare `planar` on a TTY)."},
      {"planar workflow", "Discover, inspect, and run Lua workflows for planar-execute."},
      {"planar workflow list", "List shipped and sandbox workflows."},
      {"planar workflow show", "Show @meta and source path for a named workflow."},
      {"planar workflow run", "Resolve a workflow by name and exec it via planar-execute."},
      {"planar feedback", "Manage structured feedback."},
      {"planar feedback triage", "Review structured feedback triage."},
      {"planar feedback triage list", "List triaged findings."},
      {"planar feedback triage show", "Show a triaged finding."},
      {"planar feedback triage set", "Set operator-confirmed triage fields."},
  };
  return k_summaries;
}

auto unported_paths() -> std::span<std::string_view const> {
  static constexpr std::string_view k_unported[] = {
      // The `question`, `decision`, `scenario` and `artifact` DRAFTING
      // quartets — sixteen leaves — left this inventory at task 6205 with
      // the `editflow` port they were all blocked on.
      //
      // `plan edit | view | diff | review` and `task edit | view | diff |
      // review` — the remaining EIGHT — left it at task 6208, which is the
      // oracle run task 6205 held them for. The three paths that hold-note
      // named were each RUN against the oracle in a pinned arena before
      // wiring, and all three confirmed the already-ported arms:
      //   - `walk_to_anchor` from a CHILD plan and from a GRANDCHILD both
      //     land on the root (`anchor_plan_id: 1` for plans 2 and 3).
      //   - `README.md` is the anchor plan's OWN file; a non-anchor plan is
      //     `plans/<slug>.md`.
      //   - `task_workbench_dir` tries repo-scope, then a `touches` edge,
      //     then `cross` — confirmed per arm INCLUDING the precedence case
      //     the chain's shape alone does not settle: a task that is both
      //     repo-scoped to `proj` and `touches proj2` lands in
      //     `tasks/proj/`, so repo-scope wins.
      //
      // What the run DID find is a per-family divergence, and it is the
      // reason this was worth a task rather than a one-line wiring change:
      // for these two families a NONEXISTENT id reports `not_found`, not
      // `no_plan_link`. `plan diff 999` says `no plan with id 999` where
      // `question diff 999` says `question 999 is not linked to a plan`.
      // The `not_found` arm that `editflow.cpp`'s `prose_error` documents
      // as unreachable for the other four is the ONLY arm these two reach.
      // `assoc list` and `assoc remove` left this inventory at task 6279,
      // and `assoc detect` -- the family's last and largest remainder, the
      // ~680-line proposal engine (`detectProposals` /
      // `proposalsFromSignals` / `enrichProposals` / `applyProposals`) --
      // left it at task 6325. The `assoc` family is now fully ported and
      // contributes nothing to this array.
      //
      // `audit commits` left this inventory at task 6277. It had been
      // listed as blocked on the git-walk seam alongside `capture commits`
      // and `bench harvest`; that grouping was wrong (its handler spawns
      // nothing and reads rows the walk WRITES) and was corrected at task
      // 6272. The other two stay — they are genuinely blocked.
      // `audit publish-decision` left this inventory at task 6339, closing
      // the `audit` family. It had been carried as needing an adapter
      // INSTANCE — the auth-resolving factory landed at task 6258 — and
      // what actually remained was `postComment` on both adapters (~81
      // lines, not part of the four-operation `external_adapter`
      // interface) plus this 176-line handler. See handlers/audit.cppm.
      // `audit trail` left this inventory at task 6262.
      // `capture commits` left this inventory at task 6358. It had been
      // carried as blocked on 1205 lines of git-subprocess walking in
      // zig/src/engine/runtime/sessioncommits.zig, with "no process-spawn
      // seam in this tree" as the reason. Tasks 6128/6137 had already
      // closed that seam (`planar.git`) for `capture session` and the
      // worktree gate; this task reached it a second hop out through
      // `sessioncommits.cppm`'s new `resolve_repo_root_strict` /
      // `walk_strict` / `resolve_shas` / `record_count` (added to the SAME
      // `engine_runtime` CMake target `capture` already lived in — no new
      // engine-to-engine edge).
      // `bench harvest` left this inventory at task 6362, the last leaf
      // that note's "the other two stay" referred to. It had been carried
      // as blocked on the same git-subprocess seam, and by the time this
      // task landed the seam (`planar.git`, tasks 6128/6137, already used
      // by four other consumers) was the ONLY thing missing — the FAIL-
      // SOFT walk and claim-window fold the note above worried about
      // belong to `capture commits`' different oracle module
      // (`sessioncommits.zig`), not to `harvest.zig`, which shells exactly
      // two git subcommands (`diff --name-only`, `ls-files --others`) and
      // writes through the already-ported `touch_idempotent`. See
      // `src/lib/engine/runs/harvest.cppm` for the port and
      // `src/lib/engine/runs/CMakeLists.txt` for the closing account.
      // The five `config` leaves -- `show`, `edit`, `validate`, `init`,
      // `path` -- left this inventory at task 6259. Their engine half
      // (`planar.engine.config`) had been complete since commit 82820b7;
      // what landed was the wiring plus the config-file PATH, the two
      // starter blobs, and `config validate`'s four-step rule set.
      // `dashboard` left this inventory at task 6329. It had been parked
      // behind "the absent layer-3 cmd surface" since task 6102 — a
      // blocker layer 3's arrival at task 6105 removed and nobody
      // revisited. Every engine symbol it needs was already present.
      // `demote` and `promote` left this inventory at task 6299, together
      // with `test-spec status` below. All three were handler-only: their
      // engines — `planar.engine.promotion` (task 6094) and
      // `planar.engine.planning.test_spec_status` — had been ported in FULL
      // including their output renderers, so the cycle wired three leaves
      // and wrote no engine code.
      "explore",
      // `ext create` left this inventory at task 6295. It had been carried
      // as blocked on the create/propagate half of `engine_extsync` and
      // needed ZERO of those lines: its only uses of that surface were
      // `common.{LocalEntity, CreateOptions, Header}`, all three already
      // present as `adapter::local_entity` / `adapter::create_options` /
      // `http::header`. The real precondition was two `adapter_handle`
      // accessors. Its two siblings below stayed until task 6419, when the
      // whole `ext`/`sync` family (including `ext propagate-one`, already
      // ported by then) moved to `planar-ext`. `ext propagate` moved with
      // them conceptually but is not yet wired on EITHER binary — it
      // leaves THIS inventory not because it landed, but because `ext` no
      // longer has a home on `planar` at all. See tree.cpp's note where
      // `add_ext` used to be.
      //
      // `ext propagate-one` left this inventory at task 6335, and it is the
      // SIXTH over-stated blocker of this milestone. It was carried under the
      // whole create/propagate half of `engine_extsync` — 3665 lines across
      // five files — and measured by SYMBOL it reaches exactly two functions,
      // ~40 lines: `strategyForSystem` (pure; landed as
      // `planar.engine.extsync.propagate`) and `loadExistingMirror` (SQL;
      // landed in `planar.engine.external.link`, because `engine_extsync`
      // carries no `db` edge). It reaches NOTHING in `parent_issue.zig` or
      // `projects_v2.zig` — the 2394 lines the task brief flagged as
      // "possibly not needed"; they are not needed, confirmed by call graph
      // rather than assumed. Same correction as `audit commits` (6272), the
      // `sync` trio (6294) and `plan descendants` (6298): a LEAF's
      // dependencies inferred from its MODULE's.
      //
      // `ext propagate` STAYS. It is the one leaf of the four that genuinely
      // wants the bulk — `selectStrategy`, `walkTree`, all of `strategy.zig`,
      // and both GitHub-specific files.
      //
      // Task 6353 landed ONE of those two GitHub-specific files in full:
      // `parent_issue.zig`'s entire orchestration (~1037 oracle
      // implementation lines, brace-balanced rather than whole-file
      // `wc -l` — the SAME 27% over-count task 6111 had already measured
      // for this pair) is now `planar.engine.external.parent_issue`, NOT a
      // new corner of `engine_extsync` — see that module's header for why:
      // the file is majority SQL and `engine_extsync` carries no `db`
      // edge, the same reason `record_mirror_link`/`load_existing_mirror`
      // live in `engine_external` rather than here. `projects_v2.zig`
      // (~1050 oracle implementation lines by the same brace-balanced
      // count) is UNTOUCHED — it is not a re-export of `parent_issue.zig`;
      // it duplicates that file's `entityForCreate` under its own name
      // (`parent_issue_entityForCreate`) and adds the ProjectsV2 GraphQL
      // surface (`getAuthenticatedOwner`, `createProjectV2`,
      // `getProjectV2Fields`, `addProjectV2Item`,
      // `setProjectV2ItemFieldValue`) on top.
      //
      // `ext propagate` STILL stays, for three independent reasons, only
      // one of which task 6353 touched:
      //   1. `projects_v2.zig` — untouched, per above.
      //   2. `selectStrategy` / `strategy.zig` (578 lines) — the ADR-0006
      //      repo-count bucketing that picks which of the two GitHub
      //      strategies (or the Jira one) applies. Untouched.
      //   3. NO PRODUCTION `gh_client` exists to drive
      //      `parent_issue::propagate_parent_issue_with_repo` with:
      //      `github_adapter` (engine_extsync/github.cppm) has not grown
      //      `createIssue`/`linkSubIssue`/`linkSubIssueProbe`/
      //      `postComment` — every one of them is still listed as
      //      deferred-with-`ext-propagate` in that bucket's own
      //      CMakeLists.txt, unchanged by this task. The CLI handler
      //      wiring itself (`ext.cpp`'s dispatch, matching
      //      `zig/src/cmd/planar/handlers/ext/propagate.zig`'s ~380-line
      //      `runParentIssueStrategy` bridge) is also not started.
      //
      // Worth reading before touching either creation path: `propagate-one`
      // is the IDEMPOTENT one, structurally — `load_existing_mirror` runs
      // before the template is loaded and before any adapter exists, and a
      // repeat returns `op:"skipped"` sending nothing. That makes it the
      // correct shape already present in this tree for defects 6312 and 6313
      // (`ext create` POSTs before validating `--role`, and POSTs a SECOND
      // ticket on repeat). Both defects remain reproduced deliberately in
      // `ext_create`; neither is visible to the state differential, because
      // both trees POST identically — only a fixture server's request log
      // shows them. `workbench publish` is a THIRD shape again: it REFUSES on
      // an existing link rather than skipping.
      // The three `feedback triage` leaves left this inventory at task 6303,
      // together with the `engine.planning.feedback_triage` engine that was
      // the whole of what blocked them.
      // `health` left this inventory at task 6357, closing the family (task
      // 6090 had already landed `health hygiene`). The blocker was real,
      // not over-stated this time: the handler folds
      // `engine.installedsurface.status` (548 Zig lines of manifest-driven
      // filesystem classification) into every run, and porting `check`
      // without it would report a permanently-stubbed `projection_freshness`
      // and get `overall` — the field the leaf's exit-1-on-degraded
      // contract reads — wrong on any machine with a managed install. What
      // unblocked it was the SAME move task 6352 used for `report`
      // (decision 981): rather than a same-layer `engine_health ->
      // engine_<classifier>` edge (D15/D18 FATAL on that), the classifier
      // was ported straight to layer 1 as `planar.installed_surface`
      // (src/lib/installed_surface) — it holds no `db` edge, so there was
      // nothing pulling it toward `engine_health` in the first place. See
      // handlers/health.cppm and src/lib/engine/health/health.cppm.
      // `import` is wired at task 6106: its filesystem-only request/cache
      // engine stays below the handler, while this layer composes the
      // deterministic plan write without a D15 peer dependency.
      // `report` left this inventory at task 6352, once `engine_introspect`
      // (task 6121, DB aggregates) and `engine_introspection_adapters`
      // (tasks 6102 and 6352, filesystem discovery) were both complete and
      // decision 981's layer-1 `introspection_preview` extraction let
      // `bundle::preview` reach across the D15-forbidden `engine_* ->
      // engine_*` gap between them. See src/cmd/planar/handlers/report.cppm.
      // `link` left this inventory at task 6301. It was never engine-blocked:
      // `external::link::create`, `external::system::show_by_slug` and the
      // three `*_from_text` enums were all present, and the belief that it
      // was blocked rested on the ORACLE'S OWN HEADER COMMENT claiming
      // `--propagate` "refuses with NotImplemented" — which is false; the
      // code propagates. That one FLAG is the only blocked part, and it is
      // refused at exit 64 as a recorded divergence. See handlers/link.cppm.
      // `models resolve` left this inventory at task 6343, the family's
      // fourteenth and last leaf. Task 6111 had already landed its two PURE
      // halves (`profile`, `roles`) in `engine_models`; what remained was
      // `assemble_planning`/`compile_planning` in `engine_ingest`, plus this
      // handler's `packet::evidence` -> `profile::fact` adapter.
      //
      // The inherited ~300-line estimate UNDER-STATED it, and by a wider
      // margin than the usual colocated-Zig-test inflation runs the other
      // way: this port measured ~450 implementation lines in packet.cpp
      // alone. The gap is the counting-query duplication the estimate's own
      // note anticipated but did not size — the oracle's
      // `planningCoverageEvidence` calls `test_spec_status.compute` in three
      // lines because Zig has no same-layer-edge prohibition; this tree's
      // D15/D18 FATAL on that edge, so the three queries
      // (`milestone_task_count`, `milestone_scenario_count`,
      // `milestone_tasks_covered_count`) are reproduced by hand, in full,
      // rather than called (~90 of the ~450 lines by itself).
      //
      // Landed alongside a deliberate non-decision: `profile::profile`'s
      // `cohort` field stays unported (documented in profile.cppm) because
      // the oracle's `Cohort` has seven fields and this tree's
      // `ranking::cohort` has eight — constructing one here would mean
      // inventing the eighth. Nothing on this leaf's path needs it.
      // `plan descendants` left this inventory at task 6298, and its four
      // family siblings below did NOT. It had been grouped with them and
      // with `ext propagate` as blocked on the create/propagate half of
      // `engine_extsync`; for THIS leaf that was false. Its whole engine
      // need is `walkTree` -- 78 of `propagate.zig`'s 409 lines, three SQL
      // queries reaching no adapter, transport, credential or template.
      // `strategyForSystem`, `selectStrategy`, `countDistinctReposInFeature`,
      // `hasExistingMirrorLink` and `loadExistingMirror` are all unreached
      // from it, so `ext propagate` stays below with the rest of that file.
      //
      // The walk landed as `planar.engine.planning.descendants`, NOT under
      // `engine_extsync`, because that bucket's stated invariant is that it
      // carries NO `db` edge (see its CMakeLists) and this is nothing but
      // SQL. Third instance of the same correction as `audit commits`
      // (task 6272) and the `sync` write trio (task 6294): a LEAF's
      // dependencies inferred from its MODULE's.
      // `plan closeout` LEFT this inventory at task 6317, and with it the
      // `plan` family has no unported leaf at all. It was the last of the
      // four task 6298 measured, and the only one of them that WRITES —
      // `CLAUDE.md` calls it the authoritative gate for closing a plan, so
      // every arm was captured in a pinned scratch arena against a
      // deliberately disposable fixture, never against a real database.
      //
      // Three things about it are worth reading before anything else
      // touches `agent_work_claims`:
      //   - THE THREE CLAIM-READING QUERIES DISAGREE ABOUT `status`. The
      //     COUNT query filters `status='active'`; the two LOCALITY queries
      //     behind the advisory git layer do not, so a `released` claim
      //     contributes git evidence while contributing no count. Measured,
      //     not inferred.
      //   - `--dry-run` EXITS 0 EVEN WHEN THE GATE FAILS, and the verb's own
      //     help string above says the opposite. That sentence is stale in
      //     the oracle and is reproduced verbatim; the handler comment
      //     beside the check has the real rule and the measurement agrees
      //     with the comment. Fifth time this milestone that prose lost to
      //     code.
      //   - THE ALREADY-TERMINAL SHORT-CIRCUIT RETURNS A DIFFERENT SHAPE:
      //     `git_evidence: []` where a live evaluation with no locality data
      //     returns a ONE-entry synthetic `(none)` row. Same plan, before
      //     and after closing.
      //
      // It is also the ONE leaf in this tree with a deliberate behavioural
      // divergence from the oracle, confined to advisory fields. See
      // `src/lib/engine/planning/closeout.cppm`'s DIVERGENCE section and the
      // note in that bucket's CMakeLists before assuming a git-evidence
      // difference is a bug.
      //
      // `plan next` LEFT this inventory at task 6309 — it was the fourth
      // and cheapest of the group, and it went exactly as sized:
      // `agentactivity::next_work` plus handler rendering, no surprises in
      // the engine half.
      //
      // `plan divergence` and `plan recommend-strategy` LEFT this inventory
      // TOGETHER at task 6310, which is how 6298 sized them: both sit on
      // `engine/planning/strategy.zig` and share ~300 lines of loader
      // substrate (`loadOpenTasks`/`loadTouches`/`loadClosureTouches`), so
      // splitting them across two cycles would have meant writing that
      // loader twice or leaving one leaf reaching into the other's
      // internals. One cycle, one private substrate in
      // `planar.engine.planning.strategy`, two entry points.
      //
      // The pairing was right for the loader and wrong for everything else:
      // the two verbs agree only on the candidate set and the two refusal
      // arms. `divergence` runs NO unilateral rule (measured: a 7-task
      // fixture where `recommend-strategy` serialized five reported
      // `declared_overlaps:0 derived_overlaps:0 flips:0`), an empty touch
      // set means opposite things to the two, `--closure-source` exists on
      // only one of them, and `jaccard` renders shortest-round-trip in JSON
      // but fixed-4-decimal in text. See strategy.cppm.
      //
      // `spec ingest` left this inventory at task 6365. Its brief carried the
      // now-familiar hypothesis that this is handler wiring over an already-
      // ported engine (`engine_ingest`'s parse/diff/coverage/render/
      // materialize read side, landed task 6035) -- true for PREVIEW mode,
      // and wrong for `--apply`: `engine_ingest`'s own CMakeLists.txt already
      // documented `apply.zig` (1616 Zig lines) as a genuine, architectural
      // non-port, because it composes SIX layer-2 `engine_*` peers
      // (`engine_planning`'s plan/task/decision/question/scenario CRUD,
      // `engine_entitylink`, `engine_runtime.session`) that D15/D18 forbid
      // another layer-2 bucket from depending on. What THAT note got wrong
      // was a stale premise, not the architecture: it said three of the six
      // callees "do not exist in the C++ tree yet" (decision, question,
      // scenario); all three had landed by this task. The fix that note
      // already named -- land the composition at LAYER 3, the D20 shape
      // `annotate add` and `unlink` pioneered -- is what this task did:
      // `handlers/spec_ingest.cpp` composes `engine_planning`,
      // `engine_entitylink` and `engine_runtime` directly, with no
      // `engine_ingest` module touched or extended. The handler's outer
      // `planar.db` transaction and the DB layer's nested-savepoint support
      // reproduce the oracle's single all-or-nothing `--apply` write set;
      // idempotency and rollback both have dedicated leaf tests.
      // `sync pull`, `sync push` and `sync resolve` left this inventory at
      // task 6294. All three were briefed as blocked on the unported
      // create/propagate half of `engine_extsync`; none of them touches it.
      // What they call is `engine.external.sync.{pullLink, pushLink,
      // resolveConflict}` — a DIFFERENT module that was already ported in
      // full — so the whole cycle was handler wiring plus one missing
      // cmd-layer helper (`guard_with_membership`, which moved to
      // `planar.cmd.planar.scope` at task 6303 when `feedback triage set`
      // became its second caller family). See handlers/sync.cppm.
      //
      // `sync status` left this inventory at task 6298. Task 6294's note
      // called it "NOT a fourth free leaf" and the shape bore that out --
      // it renders `engine.external.sync.status` rows, a listing none of
      // pull/push/resolve produces, takes `--entity` rather than a
      // positional, and runs NO cross-scope guard. But its ENGINE half was
      // already complete (`sync::status` + `link::list_filter` shipped with
      // the module), so what it needed was rendering, not engine work.
      // `task packet` left this inventory at task 6324. Task 6298 had
      // verified it BLOCKED on `engine/routing/packet.zig`'s 1674 lines and
      // was right about the size and wrong about the block: the LEAF needs
      // only that file's TASK half, and the PLANNING half it shares a file
      // with belonged to `models resolve`, deferred at the time with
      // `roles.zig` and `profile.zig` (the PLANNING half landed later, at
      // task 6343, in the SAME `packet.cppm`/`.cpp` — see the `models
      // resolve` note above). It landed in `engine_ingest` rather than a new
      // `engine_routing` bucket because its freshness computation is defined
      // in terms of `materialize`'s digests and D15/D18 FATAL on a
      // layer-2-to-layer-2 edge — see src/lib/engine/ingest/CMakeLists.txt.
      // `task touches infer` left this inventory at task 6330, completing
      // the `task touches` family. Its deferral note called it "773 lines
      // of git-diff and language-aware path inference"; running the oracle
      // showed that to be wrong on both counts — it shells nothing, imports
      // no git and knows no languages. See dispatch.cpp's registration
      // comment and touchinfer.cppm.
      // `test-spec status` left this inventory at task 6299 — see the note
      // beside `explore` above.
      // `tree` left this inventory at task 6278. Its whole product is a
      // RENDERED hierarchy, so every expected byte — connectors, indent
      // extensions, dirs-first grouping, the summary footer — was captured
      // from the oracle against a fixture three levels deep with siblings
      // at more than one level, never reconstructed from what looked
      // reasonable. The capture also settled what no sibling verb could
      // have told it: on this one verb `--kind ''` REFUSES (exit 2),
      // `--status ''` matches nothing (exit 0), and `--scope ''` means
      // GLOBAL (exit 0) — three different meanings for the same empty
      // value. `--sort` is accepted and INERT in the oracle, and is
      // reproduced that way deliberately; see
      // `src/lib/engine/tree/CMakeLists.txt`.
      // `workbench edit` and `workbench extract-questions` LEFT this
      // inventory at task 6302. Neither was ever architecturally blocked:
      // `edit` is push -> spawn `$EDITOR` on the feature directory -> pull
      // over the already-ported `editor::spawn_inherit`, and
      // `extract-questions` needed only a pure text walk, now
      // `planar.engine.workbench.questions`. `publish` stays, and is the
      // only workbench leaf with a real blocker: it needs
      // `extsync.parent_issue.recordLink` and `create_remote`, and lands
      // with the adapters.
      //
      // It LEFT this inventory at task 6335, and that deferral note was the
      // most misleading of the four: read as "lands with the adapters" it
      // implied the 1205-line `parent_issue.zig`. The leaf's whole reach into
      // that file is `recordLink` — 36 lines of SQL with no adapter,
      // transport, credential or template edge — now
      // `engine::external::link::record_mirror_link`, which is NOT `create`
      // with different arguments: it writes the `sync_events` audit row too.
      // `create_remote` was already in this tree, TU-private to `ext.cpp`; it
      // is now shared out of `ext_adapter_factory` for its three callers.
      // `workspace routing show` left this inventory at task 6110 and
      // `workspace routing build` at task 6275, closing the family's
      // read/write loop: the decoder and both render arms, then the builder
      // that WRITES the file they read. `build` was deferred on SIZE alone
      // (1410 lines, no architectural blocker) and that sizing held —
      // SQLite plus filesystem, no new dependency.
      //
      // `workspace regenerate` left this inventory at task 6364. It was
      // carried as blocked on an unvendored xxh64 for its `.manifest-docs`
      // merkle — verified TRANSITIVELY true rather than stale: `regenerate`'s
      // own source has no xxh64 reference, but it calls `manifest.build`,
      // which does. xxHash 0.8.3 is now vendored (`cmake/dependencies.cmake`)
      // behind the new layer-1 `planar.docs_manifest` module — NOT
      // `engine_docs`, since D15/D18 forbid an `engine_* -> engine_*` edge
      // and this bucket is its only consumer. See
      // `planar.engine.workspace.regenerate`'s header for the ported
      // hand-rolled template engine and its one deliberate reproduced quirk.
      //
      // `workspace init` is the family's LAST remaining leaf, and its
      // blocker is now strictly smaller than it was: it is layer-3 blocked
      // because it COMPOSES scan + registration + routing build +
      // regenerate + symlink install, and two of those four now exist in
      // this tree (`routing build` at task 6275, `regenerate` here).
  };
  return k_unported;
}

} // namespace planar::cmd
