/// @file tree.cpp
/// @brief Implementation of `planar.cmd.planar.tree`.

module planar.cmd.planar.tree;

import std;
import cli11;
import planar.cliapp.args;

namespace planar::cmd {

namespace {

/// @brief The `--json` flag every ported leaf declares.
/// @param app The node to declare it on.
auto add_json(CLI::App& app) -> void {
  app.add_flag("--json");
}

/// @brief A plain string flag with no default.
/// @param app The node to declare it on.
/// @param name The canonical long name.
auto add_string(CLI::App& app, std::string name) -> void {
  app.add_option(std::move(name));
}

/// @brief An integer flag.
///
/// `zig_int_validator` is what keeps Zig's `std.fmt.parseInt` semantics
/// (underscore digit separators, a leading `+`) enforced at PARSE time
/// rather than degrading into a silent "absent" at the handler — see
/// `planar.cliapp.args`.
/// @param app The node to declare it on.
/// @param name The canonical long name.
auto add_int(CLI::App& app, std::string name) -> void {
  app.add_option(std::move(name))->check(cliapp::zig_int_validator());
}

/// @brief A boolean flag — the shape every `--verbose` / `--dry-run` /
/// `--yes` node in the `workbench` group declares.
/// @param app The node to declare it on.
/// @param name The canonical long name.
/// @param desc The help line, or empty.
auto add_bool(CLI::App& app, std::string name, std::string desc = {}) -> void {
  app.add_flag(std::move(name))->description(std::move(desc));
}

/// @brief The `--filter-mode` flag, declared identically on `push`,
/// `archive`, `restore` and `gc`.
/// @param app The node to declare it on.
auto add_filter_mode(CLI::App& app) -> void {
  app.add_option("--filter-mode")->description("Terminal-status filter: 'failures' (default) or 'all'");
}

/// @brief The `workflow` group — transcribed from
/// zig/src/cmd/planar/handlers/workflow/cmd.zig. `run` is absent: it is
/// deferred with its process-spawn dependency (see
/// src/lib/engine/workflows/CMakeLists.txt), so this group's own help page
/// lists two commands where the oracle lists three.
/// @param root The root app to attach the group to.
auto add_workflow(CLI::App& root) -> void {
  CLI::App* workflow = root.add_subcommand("workflow", "Enumerate, inspect, and invoke shipped and sandbox Lua workflows.\n\n"
                                                       "  Shipped workflows live at $PLANAR_HOME/workflows/ (default\n"
                                                       "  ~/.planar/workflows/).  Sandbox workflows live at\n"
                                                       "  ~/.planar/local/workflows/ and are marked `local`.\n\n"
                                                       "  These commands are READ-ONLY w.r.t. SQLite.  `run` delegates\n"
                                                       "  execution to `planar-execute` and forwards its output + exit code.");
  workflow->require_subcommand(0);

  CLI::App* list = workflow->add_subcommand("list", "List shipped and sandbox workflows.");
  list->add_flag("--local");
  add_json(*list);

  CLI::App* show = workflow->add_subcommand("show", "Show @meta and source path for a named workflow.");
  add_json(*show);
  show->add_option("name")->required();
}

/// @brief The `annotate` group — transcribed from
/// zig/src/cmd/planar/handlers/annotate/cmd.zig. Two of its fourteen
/// subcommands are ported here.
/// @param root The root app to attach the group to.
auto add_annotate(CLI::App& root) -> void {
  CLI::App* annotate =
      root.add_subcommand("annotate", "Manage line-anchored annotations on source code.\n\n"
                                      "  Status lifecycle: active \xe2\x86\x92 resolved / dismissed / archived.");
  annotate->require_subcommand(0);

  CLI::App* add = annotate->add_subcommand("add", "Create a new annotation.");
  add_string(*add, "--anchor-path");
  add_int(*add, "--line-start");
  add_int(*add, "--line-end");
  add_string(*add, "--commit-sha");
  add_string(*add, "--text-hash");
  add_string(*add, "--text");
  add_string(*add, "--title");
  add_string(*add, "--slug");
  add_string(*add, "--body");
  add_string(*add, "--vendor");
  add_int(*add, "--plan");
  add_int(*add, "--task");
  add_string(*add, "--tags");
  add_string(*add, "--scope");
  add_json(*add);

  CLI::App* list = annotate->add_subcommand("list", "List annotations.");
  add_string(*list, "--anchor-path");
  add_string(*list, "--status");
  add_int(*list, "--plan");
  add_int(*list, "--task");
  add_string(*list, "--vendor");
  add_string(*list, "--tag");
  add_string(*list, "--scope");
  add_json(*list);
}

/// @brief The `unlink` leaf — transcribed from
/// zig/src/cmd/planar/handlers/unlink.zig. A top-level LEAF, not a group.
///
/// `<link-id>` is declared as a plain STRING positional with no validator,
/// deliberately: the oracle's node declares `.kind = .string` and the
/// handler converts with Zig's `parseInt`, so `unlink abc` reaches the
/// HANDLER's refusal (exit 1), not a parse error (exit 2). Attaching
/// `zig_int_validator` here would move that failure a layer earlier and
/// change both the message and the exit code. The parity suite pins the
/// whole `1_0` / `007` / `+12` / `_10` / `10_` / `0x10` / overflow table
/// against the live oracle through this leaf.
/// @param root The root app to attach the leaf to.
auto add_unlink(CLI::App& root) -> void {
  CLI::App* unlink = root.add_subcommand("unlink", "Remove an external_links row by its link id.\n\n"
                                                   "  Associated sync_events rows are detached by setting link_id to null\n"
                                                   "  rather than cascade-deleted; they are no longer reachable through\n"
                                                   "  the deleted link's audit trail.");
  unlink->add_option("--scope")->description("Scope for the cross-scope guard (currently informational)");
  add_json(*unlink);
  unlink->add_option("link-id")->description("External-link id (integer)")->required();
}

/// @brief The `skills` node — transcribed from
/// zig/src/cmd/planar/handlers/skills/cmd.zig.
///
/// Zero subcommands and zero flags, which is the whole point: plan 918 M5
/// retired `render`/`status`/`repair` and left the node registered so the
/// verb reports "no subcommands" rather than an unknown-verb error. With
/// no children it is a LEAF and needs a handler — see
/// `planar.cmd.planar.handlers.skills`.
/// @param root The root app to attach the leaf to.
auto add_skills(CLI::App& root) -> void {
  root.add_subcommand("skills", "The unified skill source tree under skills/src/ is rendered by the\n"
                                "  external scriptorium binary (plan 918). Planar no longer renders vendor\n"
                                "  projections nor tracks their install-drift in-band; use `scriptorium\n"
                                "  check`/`scriptorium status` instead. This command has no subcommands.");
}

/// @brief The `workspace` group — transcribed from
/// zig/src/cmd/planar/handlers/workspace/cmd.zig. One of its four
/// subcommands is ported (`doctor`), so this group's own help page lists
/// one command where the oracle lists four.
/// @param root The root app to attach the group to.
auto add_workspace(CLI::App& root) -> void {
  CLI::App* workspace =
      root.add_subcommand("workspace", "Workspace administration.\n\n"
                                       "  A workspace is identified by an associations row of kind=org. Each\n"
                                       "  workspace owns a state directory under\n"
                                       "  ${PLANAR_HOME:-~/.planar}/workspaces/<org_id>/ holding the canonical\n"
                                       "  AGENTS.md surface for the org.");
  workspace->require_subcommand(0);
  CLI::App* doctor = workspace->add_subcommand("doctor", "Scan and fix workspace registration and state consistency.");
  add_json(*doctor);
}

/// @brief The `workbench` group — transcribed from
/// zig/src/cmd/planar/handlers/workbench/cmd.zig.
///
/// TEN of its thirteen subcommands are ported, so this group's own help
/// page lists ten where the oracle lists thirteen — the same accounted-for
/// divergence `workflow` and `workspace` already carry, and the same rule
/// (omit an unported child rather than register a stub). `publish` waits on
/// the external adapters, `edit` on a process-spawn seam, and
/// `extract-questions` is deferred on size.
///
/// `archive` and `restore` still declare `--filter-mode` even though the
/// engine's `archive` ignores it (it has no write set to filter) —
/// dropping it would be a visible divergence for a flag the oracle accepts.
/// @param root The root app to attach the group to.
auto add_workbench(CLI::App& root) -> void {
  CLI::App* workbench = root.add_subcommand("workbench", "Manage the bidirectional sync surface between the workbench\n"
                                                         "  filesystem and the Planar database.\n\n"
                                                         "  The workbench root resolution order (highest to lowest priority):\n"
                                                         "    1. $PLANAR_WORKBENCH_ROOT env var\n"
                                                         "    2. workbench.root in $PLANAR_CONFIG_PATH or ~/.planar/config.toml\n"
                                                         "    3. Default: ~/.planar/workbench/");
  workbench->require_subcommand(0);

  CLI::App* lint = workbench->add_subcommand("lint", "Validate workbench Markdown frontmatter without syncing.");
  add_bool(*lint, "--all", "Validate every workbench tree");
  lint->add_option("--path")->description("Validate one Markdown file or directory");
  add_json(*lint);
  lint->add_option("plan");

  CLI::App* pull = workbench->add_subcommand("pull", "Apply FS\xe2\x86\x92"
                                                     "DB changes; report DB\xe2\x86\x92"
                                                     "FS drift.");
  add_bool(*pull, "--verbose");
  add_json(*pull);
  pull->add_option("plan")->required();

  CLI::App* push = workbench->add_subcommand("push", "Apply DB\xe2\x86\x92"
                                                     "FS changes atomically; report FS\xe2\x86\x92"
                                                     "DB drift.");
  add_bool(*push, "--verbose");
  add_json(*push);
  add_filter_mode(*push);
  add_bool(*push, "--apply-cleanup", "Remove pre-existing FS files for entities this push would have filtered");
  push->add_option("plan")->required();

  CLI::App* status = workbench->add_subcommand("status", "Show drift and conflicts without writing.");
  add_bool(*status, "--verbose");
  add_json(*status);
  status->add_option("plan");

  CLI::App* resolve = workbench->add_subcommand("resolve", "Settle a sync conflict by choosing FS or DB.");
  resolve->add_option("--prefer")->description("Which side to prefer (fs|db)")->required();
  add_json(*resolve);
  resolve->add_option("event-id")->required();

  CLI::App* sync = workbench->add_subcommand("sync", "Atomically apply FS and DB changes via a unified sync.");
  add_bool(*sync, "--verbose");
  add_json(*sync);
  sync->add_option("plan")->required();

  CLI::App* archive = workbench->add_subcommand("archive", "Archive a feature's workbench filesystem tree.");
  add_json(*archive);
  add_filter_mode(*archive);
  archive->add_option("plan")->required();

  CLI::App* restore = workbench->add_subcommand("restore", "Restore an archived feature's workbench tree.");
  add_json(*restore);
  add_filter_mode(*restore);
  restore->add_option("plan")->required();

  CLI::App* gc = workbench->add_subcommand("gc", "Remove FS files whose backing entity is terminal in the DB.");
  add_bool(*gc, "--dry-run", "Preview only; do not touch disk");
  add_bool(*gc, "--yes", "Discard FS-content drift; remove drifted files anyway");
  add_filter_mode(*gc);
  add_bool(*gc, "--all-scopes", "Walk every plan's workbench tree");
  add_json(*gc);
  gc->add_option("plan");

  CLI::App* list = workbench->add_subcommand("list", "List features with workbench trees.");
  add_json(*list);
}

} // namespace

auto root_app() -> std::unique_ptr<CLI::App> {
  auto app = std::make_unique<CLI::App>("Planning + agent operations CLI.", "planar");
  // A bare `planar` must render root help rather than fail.
  app->require_subcommand(0);

  app->add_subcommand("version", "Print the planar version, commit, and zig runtime.");
  add_workflow(*app);
  add_annotate(*app);
  add_unlink(*app);
  add_skills(*app);
  add_workspace(*app);
  add_workbench(*app);
  return app;
}

} // namespace planar::cmd
