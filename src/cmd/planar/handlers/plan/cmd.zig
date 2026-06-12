//! handlers/plan/cmd.zig — `planar plan {create, show, list, update, link, recompute-status,
//!   edit, view, step {add, list, done, skip, link}, closeout}`

const cli = @import("cli");

const create = @import("create.zig");
const show = @import("show.zig");
const list = @import("list.zig");
const update = @import("update.zig");
const edit = @import("edit.zig");
const view = @import("view.zig");
const diff = @import("diff.zig");
const review = @import("review.zig");
const link = @import("link.zig");
const next = @import("next.zig");
const recommend_strategy = @import("recommend_strategy.zig");
const recompute_status = @import("recompute_status.zig");
const closeout = @import("closeout.zig");
const step = @import("step/cmd.zig");

pub const verb: cli.Cmd = .{
    .name = "plan",
    .desc = "Manage plans and plan steps.",
    .long_desc = "Manage plans — the top-level structured intent for a body of work.\n\n  Plans may be hierarchical (--parent) and contain ordered steps\n  (plan step add).\n  Status lifecycle: draft → active → paused / done / abandoned.",
    .cmds = &.{
        .{
            .name = "create",
            .desc = "Create a new plan.",
            .flags = &.{
                .{ .long = "--summary", .kind = .string },
                .{ .long = "--slug", .kind = .string },
                .{ .long = "--scope", .kind = .string },
                .{ .long = "--status", .kind = .string, .default = .{ .string = "draft" } },
                .{ .long = "--parent", .kind = .int },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{.{ .name = "title", .kind = .string, .required = true }},
            .run = cli.handler(create.handle),
        },
        .{
            .name = "show",
            .desc = "Show a plan's details, steps, and child plans.",
            .flags = &.{.{ .long = "--json", .kind = .bool, .default = .{ .bool = false } }},
            .positionals = &.{.{ .name = "plan-id", .kind = .string, .required = true }},
            .run = cli.handler(show.handle),
        },
        .{
            .name = "list",
            .desc = "List plans.",
            .flags = &.{
                .{ .long = "--scope", .kind = .string },
                .{ .long = "--status", .kind = .string },
                .{ .long = "--parent", .kind = .int },
                .{ .long = "--touches", .kind = .string },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .run = cli.handler(list.handle),
        },
        .{
            .name = "update",
            .desc = "Update mutable fields on a plan.",
            .flags = &.{
                .{ .long = "--title", .kind = .string },
                .{ .long = "--slug", .kind = .string },
                .{ .long = "--summary", .kind = .string },
                .{ .long = "--status", .kind = .string },
                .{ .long = "--parent", .kind = .int },
                .{ .long = "--scope", .kind = .string },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{.{ .name = "plan-id", .kind = .string, .required = true }},
            .run = cli.handler(update.handle),
        },
        .{
            .name = "edit",
            .desc = "Edit a plan in $EDITOR (editor-first flow).",
            .flags = &.{
                .{ .long = "--no-pull", .kind = .bool, .default = .{ .bool = false } },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{.{ .name = "plan-id", .kind = .string, .required = true }},
            .run = cli.handler(edit.handle),
        },
        .{
            .name = "view",
            .desc = "View a plan's workbench file.",
            .positionals = &.{.{ .name = "plan-id", .kind = .string, .required = true }},
            .run = cli.handler(view.handle),
        },
        .{
            .name = "diff",
            .desc = "Diff plan against database version.",
            .positionals = &.{.{ .name = "plan-id", .kind = .string, .required = true }},
            .run = cli.handler(diff.handle),
        },
        .{
            .name = "review",
            .desc = "Reviewer entry point for plan diff.",
            .flags = &.{
                .{ .long = "--approve", .kind = .bool, .default = .{ .bool = false } },
                .{ .long = "--request-changes", .kind = .bool, .default = .{ .bool = false } },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{.{ .name = "plan-id", .kind = .string, .required = true }},
            .run = cli.handler(review.handle),
        },
        .{
            .name = "link",
            .desc = "Create an entity link from a plan to another entity.",
            .flags = &.{
                .{ .long = "--relationship", .kind = .string },
                .{ .long = "--scope", .kind = .string },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{
                .{ .name = "plan-id", .kind = .string, .required = true },
                .{ .name = "ref", .kind = .string, .required = true },
            },
            .run = cli.handler(link.handle),
        },
        .{
            .name = "next",
            .desc = "Bucketed claim-aware view of next work on a plan (available / claimed / stale / blocked).",
            .long_desc = "Bucketed claim-aware view of next work on a plan.\n\n  Buckets:\n    available  task is todo (or doing without an active claim)\n               and ready to be pulled\n    claimed    task has an active unexpired claim\n    stale      task has a stale claim (reconcile or lease-expired)\n    blocked    task status is blocked\n\n  Without --include-claimed / --include-stale the text rendering\n  shows only the available + blocked buckets — the JSON shape always\n  carries every bucket.",
            .flags = &.{
                .{ .long = "--include-claimed", .kind = .bool, .default = .{ .bool = false }, .desc = "Show the claimed bucket in text mode (JSON always includes it)." },
                .{ .long = "--include-stale", .kind = .bool, .default = .{ .bool = false }, .desc = "Show the stale bucket in text mode (JSON always includes it)." },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{.{ .name = "plan-id", .kind = .string, .required = true }},
            .run = cli.handler(next.handle),
        },
        .{
            .name = "recommend-strategy",
            .desc = "Recommend an execution strategy: compute the parallel-eligible subset of a plan's open tasks via the six parallelizability rules.",
            .long_desc = "Recommend an execution strategy for a plan's open (todo) tasks.\n\n  Applies the six parallel-eligibility rules (decision 370) and\n  reports the parallel-eligible subset plus the serialized remainder\n  with per-task exclusion reasons:\n    1. no blocked_by chain to a not-done task\n    2. disjoint task_touches (empty touches = touches-everything)\n    3. no schema migration touched\n    4. no singleton authoritative file touched\n    5. no open question linked\n    6. no proposed decision linked\n\n  READ-ONLY: computes and reports; writes nothing. fan_out_available\n  is true when >= 2 tasks are eligible.",
            .flags = &.{
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{.{ .name = "plan-id", .kind = .string, .required = true }},
            .run = cli.handler(recommend_strategy.handle),
        },
        .{
            .name = "recompute-status",
            .desc = "Recompute a plan's roll-up status (--plan <id> or --all).",
            .flags = &.{
                .{ .long = "--plan", .kind = .int, .desc = "Recompute one plan by id." },
                .{ .long = "--all", .kind = .bool, .default = .{ .bool = false }, .desc = "Recompute every plan in the DB." },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .run = cli.handler(recompute_status.handle),
        },
        .{
            .name = "closeout",
            .desc = "Delivery-evidence gate: report whether a plan is ready to close and (without --dry-run) mark it done.",
            .long_desc = "Evaluate the DB-hard gate (all tasks terminal, all descendants terminal, no live claims)\n" ++
                "  and advisory git-evidence for a plan. In apply mode (no --dry-run), marks the plan\n" ++
                "  done when the hard gate passes. Cancelled tasks are terminal — they do not block.\n\n" ++
                "  Hard gate failures produce a non-zero exit in both dry-run and apply modes.\n\n" ++
                "  --check-merge adds an advisory epic-branch merge roll-up: for each contributing\n" ++
                "  branch from agent_work_claims, reports how many are merged to the target branch.\n" ++
                "  Never blocks; absent branches are inconclusive.",
            .flags = &.{
                .{ .long = "--dry-run", .kind = .bool, .default = .{ .bool = false }, .desc = "Evaluate and report only; never writes." },
                .{ .long = "--check-merge", .kind = .bool, .default = .{ .bool = false }, .desc = "Include advisory epic-branch merge roll-up in the output." },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{.{ .name = "plan-id", .kind = .string, .required = true }},
            .run = cli.handler(closeout.handle),
        },
        step.verb,
    },
};
