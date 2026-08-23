/// @file tree.cpp
/// @brief Implementation of `planar.cmd.planar.tree`.

module planar.cmd.planar.tree;

import std;
import planar.cli;

namespace planar::cmd {

namespace {

/// @brief The `--json` flag every ported leaf declares, with the default
/// the Zig nodes carry (`.default = .{ .bool = false }`). The default is
/// load-bearing in rendered help — it prints as `default=false`, and a
/// leaf that omitted it would render a shorter line.
/// @return The flag spec.
auto json_flag() -> cli::flag {
  return cli::flag{.long_name = "--json", .value_kind = cli::kind::boolean, .default_value = cli::value{false}};
}

/// @brief A plain string flag with no default.
/// @param name The canonical long name.
/// @return The flag spec.
auto string_flag(std::string name) -> cli::flag {
  return cli::flag{.long_name = std::move(name), .value_kind = cli::kind::string};
}

/// @brief A plain integer flag with no default.
/// @param name The canonical long name.
/// @return The flag spec.
auto int_flag(std::string name) -> cli::flag {
  return cli::flag{.long_name = std::move(name), .value_kind = cli::kind::integer};
}

/// @brief The `version` leaf — transcribed from
/// zig/src/cmd/planar/handlers/version.zig.
/// @return The node.
auto version_verb() -> cli::cmd {
  return cli::cmd{.name = "version", .desc = "Print the planar version, commit, and zig runtime."};
}

/// @brief The `workflow` group — transcribed from
/// zig/src/cmd/planar/handlers/workflow/cmd.zig. `run` is absent: it is
/// deferred with its process-spawn dependency (see
/// src/lib/engine/workflows/CMakeLists.txt), so this group's own help page
/// lists two commands where the oracle lists three.
/// @return The node.
auto workflow_verb() -> cli::cmd {
  cli::cmd list{
      .name  = "list",
      .desc  = "List shipped and sandbox workflows.",
      .flags = {cli::flag{.long_name = "--local", .value_kind = cli::kind::boolean, .default_value = cli::value{false}},
                json_flag()},
  };
  cli::cmd show{
      .name        = "show",
      .desc        = "Show @meta and source path for a named workflow.",
      .flags       = {json_flag()},
      .positionals = {cli::positional{.name = "name", .required = true}},
  };
  return cli::cmd{
      .name      = "workflow",
      .desc      = "Discover, inspect, and run Lua workflows for planar-execute.",
      .long_desc = "Enumerate, inspect, and invoke shipped and sandbox Lua workflows.\n\n"
                   "  Shipped workflows live at $PLANAR_HOME/workflows/ (default\n"
                   "  ~/.planar/workflows/).  Sandbox workflows live at\n"
                   "  ~/.planar/local/workflows/ and are marked `local`.\n\n"
                   "  These commands are READ-ONLY w.r.t. SQLite.  `run` delegates\n"
                   "  execution to `planar-execute` and forwards its output + exit code.",
      .cmds      = {std::move(list), std::move(show)},
  };
}

/// @brief The `annotate` group — transcribed from
/// zig/src/cmd/planar/handlers/annotate/cmd.zig. Two of its fourteen
/// subcommands are ported here.
/// @return The node.
auto annotate_verb() -> cli::cmd {
  cli::cmd add{
      .name  = "add",
      .desc  = "Create a new annotation.",
      .flags = {string_flag("--anchor-path"), int_flag("--line-start"), int_flag("--line-end"), string_flag("--commit-sha"),
                string_flag("--text-hash"), string_flag("--text"), string_flag("--title"), string_flag("--slug"),
                string_flag("--body"), string_flag("--vendor"), int_flag("--plan"), int_flag("--task"), string_flag("--tags"),
                string_flag("--scope"), json_flag()},
  };
  cli::cmd list{
      .name  = "list",
      .desc  = "List annotations.",
      .flags = {string_flag("--anchor-path"), string_flag("--status"), int_flag("--plan"), int_flag("--task"),
                string_flag("--vendor"), string_flag("--tag"), string_flag("--scope"), json_flag()},
  };
  return cli::cmd{
      .name      = "annotate",
      .desc      = "Manage source annotations.",
      .long_desc = "Manage line-anchored annotations on source code.\n\n"
                   "  Status lifecycle: active \xe2\x86\x92 resolved / dismissed / archived.",
      .cmds      = {std::move(add), std::move(list)},
  };
}

/// @brief The `unlink` leaf — transcribed from
/// zig/src/cmd/planar/handlers/unlink.zig. A top-level LEAF, not a group:
/// it takes the link id as a positional and has no subcommands.
/// @return The node.
auto unlink_verb() -> cli::cmd {
  return cli::cmd{
      .name        = "unlink",
      .desc        = "Remove an external_links row by link id.",
      .long_desc   = "Remove an external_links row by its link id.\n\n"
                     "  Associated sync_events rows are detached by setting link_id to null\n"
                     "  rather than cascade-deleted; they are no longer reachable through\n"
                     "  the deleted link's audit trail.",
      .flags       = {cli::flag{.long_name  = "--scope",
                                .desc       = "Scope for the cross-scope guard (currently informational)",
                                .value_kind = cli::kind::string},
                      json_flag()},
      .positionals = {cli::positional{.name = "link-id", .desc = "External-link id (integer)", .required = true}},
  };
}

/// @brief The `skills` node — transcribed from
/// zig/src/cmd/planar/handlers/skills/cmd.zig.
///
/// Zero subcommands and zero flags, which is the whole point: plan 918 M5
/// retired `render`/`status`/`repair` and left the node registered so the
/// verb reports "no subcommands" rather than an unknown-verb error. Under
/// `planar.cli.cmd`'s model an empty `cmds` vector makes this a LEAF, so
/// unlike the Zig original it needs a handler — see
/// `planar.cmd.planar.handlers.skills`.
/// @return The node.
auto skills_verb() -> cli::cmd {
  return cli::cmd{
      .name      = "skills",
      .desc      = "Retired: rendering and drift detection now live in scriptorium.",
      .long_desc = "The unified skill source tree under skills/src/ is rendered by the\n"
                   "  external scriptorium binary (plan 918). Planar no longer renders vendor\n"
                   "  projections nor tracks their install-drift in-band; use `scriptorium\n"
                   "  check`/`scriptorium status` instead. This command has no subcommands.",
  };
}

/// @brief The `workspace` group — transcribed from
/// zig/src/cmd/planar/handlers/workspace/cmd.zig. One of its four
/// subcommands is ported (`doctor`), so this group's own help page lists
/// one command where the oracle lists four; see
/// `planar.cmd.planar.handlers.workspace` for what blocks the other three
/// and `init`.
/// @return The node.
auto workspace_verb() -> cli::cmd {
  cli::cmd doctor{
      .name  = "doctor",
      .desc  = "Scan and fix workspace registration and state consistency.",
      .flags = {json_flag()},
  };
  return cli::cmd{
      .name      = "workspace",
      .desc      = "Manage workspace state directories and their AGENTS.md surfaces.",
      .long_desc = "Workspace administration.\n\n"
                   "  A workspace is identified by an associations row of kind=org. Each\n"
                   "  workspace owns a state directory under\n"
                   "  ${PLANAR_HOME:-~/.planar}/workspaces/<org_id>/ holding the canonical\n"
                   "  AGENTS.md surface for the org.",
      .cmds      = {std::move(doctor)},
  };
}

} // namespace

auto root_command() -> cli::cmd {
  return cli::cmd{
      .name = "planar",
      .desc = "Planning + agent operations CLI.",
      .cmds = {version_verb(), workflow_verb(), annotate_verb(), unlink_verb(), skills_verb(), workspace_verb()},
  };
}

} // namespace planar::cmd
