//! handlers/task/touches/cmd.zig — `planar task touches {add, remove}`

const cli = @import("cli");

const add = @import("add.zig");
const infer = @import("infer.zig");
const remove = @import("remove.zig");
const list = @import("list.zig");

pub const verb: cli.Cmd = .{
    .name = "touches",
    .desc = "Manage repo-touches links on a task.",
    .cmds = &.{
        .{
            .name = "add",
            .desc = "Link a task to a repo via a 'touches' relationship.",
            .long_desc = "Declare that a task touches a repo (and, with --path, a specific file).\n\n  Without --path: writes the repo-level entity_links 'touches' edge\n  (task -> repo). This is the coarse signal used by `task list --touches`.\n\n  With --path <p>: writes a path-level task_touch_paths row (task, repo,\n  path) AND the repo-level edge — a path-touch implies the repo-touch, so\n  the repo-level signal stays consistent. <p> is a repo-relative file path.\n  The parallelizability rules (`plan recommend-strategy`) read these\n  path-level declarations for rules 2/3/4 (disjoint touches, migration\n  touched, singleton file touched). Declare path touches per file (repeat\n  the verb), not as a list.",
            .flags = &.{
                .{ .long = "--path", .kind = .string },
                .{ .long = "--scope", .kind = .string },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{
                .{ .name = "task-id", .kind = .string, .required = true },
                .{ .name = "repo-slug", .kind = .string, .required = true },
            },
            .run = cli.handler(add.handle),
        },
        .{
            .name = "infer",
            .desc = "Propose path-level touches from the task's own text (preview by default).",
            .long_desc = "Extract path-shaped tokens from a task's title, body, and next_action\n  and resolve them against a repo checkout, proposing task_touch_paths\n  rows. PREVIEW BY DEFAULT — without --apply nothing is written.\n\n  Each candidate is classified: 'resolved' (exact file), 'directory'\n  (expanded to its files), 'basename' (every matching path), 'unresolved'\n  (path-shaped but unplaceable) or 'too_broad' (expansion too large).\n  The last two are printed for review and never written.\n\n  Ambiguity always resolves WIDE (decision 906): over-declaring costs\n  throughput and is recoverable, while under-declaring costs correctness —\n  two tasks marked parallel-eligible, fanned into separate worktrees, both\n  editing the same file, colliding at fan-in.\n\n  --repo <slug> names the checkout to resolve against; without it the repo\n  is derived from the current directory (longest matching root_path).",
            .flags = &.{
                .{ .long = "--repo", .kind = .string },
                .{ .long = "--apply", .kind = .bool, .default = .{ .bool = false } },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{
                .{ .name = "task-id", .kind = .string, .required = true },
            },
            .run = cli.handler(infer.handle),
        },
        .{
            .name = "list",
            .desc = "List the repo- and path-level touches declared on a task.",
            .flags = &.{
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{
                .{ .name = "task-id", .kind = .string, .required = true },
            },
            .run = cli.handler(list.handle),
        },
        .{
            .name = "remove",
            .desc = "Remove a 'touches' link between a task and a repo.",
            .flags = &.{
                .{ .long = "--scope", .kind = .string },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{
                .{ .name = "task-id", .kind = .string, .required = true },
                .{ .name = "repo-slug", .kind = .string, .required = true },
            },
            .run = cli.handler(remove.handle),
        },
    },
};
