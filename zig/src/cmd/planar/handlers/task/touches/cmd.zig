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
            .long_desc = "Extract path-shaped tokens from a task's title, body, and next_action\n  and resolve them against a repo checkout, proposing task_touch_paths\n  rows. PREVIEW BY DEFAULT — without --apply nothing is written.\n\n  Each candidate is classified: 'resolved' (exact file), 'directory'\n  (expanded to its files), 'basename' (every matching path), 'unresolved'\n  (path-shaped but unplaceable) or 'too_broad' (expansion too large).\n\n  Only 'resolved' is written by default. The wide classifications —\n  directory and basename — are shown with their expansion size and\n  withheld unless --wide is passed. Measured over 46 tasks in six real\n  plans, including them yielded FEWER parallel-eligible tasks (13) than\n  resolved-only (14): a wide set intersects peers, and rule 2 drops both\n  sides of an overlap, so one loose directory mention can remove tasks\n  that were otherwise eligible.\n\n  Proposal still resolves ambiguity wide (decision 906) — a directory\n  expands, a basename yields every match, nothing unplaceable is\n  invented. What --wide controls is which proposals are WRITTEN.\n\n  --repo <slug> names the checkout to resolve against; without it the repo\n  is derived from the current directory (longest matching root_path).",
            .flags = &.{
                .{ .long = "--repo", .kind = .string },
                .{ .long = "--apply", .kind = .bool, .default = .{ .bool = false } },
                .{ .long = "--wide", .kind = .bool, .default = .{ .bool = false } },
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
            .long_desc = "Withdraw a touch declaration.\n\n  Without --path: removes the repo-level entity_links 'touches' edge.\n\n  With --path <p>: removes ONE path-level task_touch_paths row and leaves\n  the repo edge in place. Deliberately not symmetric with `touches add`,\n  where a path-touch implies the repo-touch — withdrawing one file should\n  not silently drop a repo claim that may carry other paths.\n\n  Removing the repo edge is not a substitute for --path: the parallel\n  eligibility rules read task_touch_paths directly, so orphaned path rows\n  keep driving eligibility after their edge is gone.",
            .flags = &.{
                .{ .long = "--path", .kind = .string },
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
