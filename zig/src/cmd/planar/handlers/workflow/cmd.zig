//! handlers/workflow/cmd.zig — `planar workflow {list, show, run}`
//!
//! Discovery and invocation layer for user-authored and shipped Lua
//! workflows.  All subverbs are READ-ONLY w.r.t. SQLite — they resolve
//! workflows via filesystem scan only.  `run` additionally execs
//! `planar-execute` as a subprocess and forwards its stdout/exit code.

const cli = @import("cli");

const list = @import("list.zig");
const show = @import("show.zig");
const run = @import("run.zig");

pub const verb: cli.Cmd = .{
    .name = "workflow",
    .desc = "Discover, inspect, and run Lua workflows for planar-execute.",
    .long_desc = "Enumerate, inspect, and invoke shipped and sandbox Lua workflows.\n\n" ++
        "  Shipped workflows live at $PLANAR_HOME/workflows/ (default\n" ++
        "  ~/.planar/workflows/).  Sandbox workflows live at\n" ++
        "  ~/.planar/local/workflows/ and are marked `local`.\n\n" ++
        "  These commands are READ-ONLY w.r.t. SQLite.  `run` delegates\n" ++
        "  execution to `planar-execute` and forwards its output + exit code.",
    .cmds = &.{
        .{
            .name = "list",
            .desc = "List shipped and sandbox workflows.",
            .flags = &.{
                .{ .long = "--local", .kind = .bool, .default = .{ .bool = false } },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .run = cli.handler(list.handle),
        },
        .{
            .name = "show",
            .desc = "Show @meta and source path for a named workflow.",
            .flags = &.{
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{.{ .name = "name", .kind = .string, .required = true }},
            .run = cli.handler(show.handle),
        },
        .{
            .name = "run",
            .desc = "Resolve a workflow by name and exec it via planar-execute.",
            .long_desc = "Resolve <name> across shipped and sandbox workflows, then exec\n" ++
                "  `planar-execute run <path> --phase <phase> [--args <json>]\n" ++
                "  [--worktree <dir>] [--sandbox-root <dir>]`.  The workflow's\n" ++
                "  flow.result JSON streams to stdout; the exit code is forwarded\n" ++
                "  exactly (non-zero on flow.fail or engine error).\n\n" ++
                "  planar-execute resolution order: $PLANAR_EXECUTE_BIN →\n" ++
                "  sibling of argv[0] → PATH.",
            .flags = &.{
                .{ .long = "--phase", .kind = .string, .required = true, .desc = "Phase function to invoke inside the workflow." },
                .{ .long = "--args", .kind = .string, .default = .{ .string = "" }, .desc = "JSON args blob forwarded to planar-execute --args." },
                .{ .long = "--worktree", .kind = .string, .default = .{ .string = "" }, .desc = "Worktree directory forwarded to planar-execute --worktree." },
                .{ .long = "--sandbox-root", .kind = .string, .default = .{ .string = "" }, .desc = "Sandbox root forwarded to planar-execute --sandbox-root." },
                .{ .long = "--local", .kind = .bool, .default = .{ .bool = false }, .desc = "Restrict resolution to sandbox (local) workflows only." },
            },
            .positionals = &.{.{ .name = "name", .kind = .string, .required = true }},
            .run = cli.handler(run.handle),
        },
    },
};
