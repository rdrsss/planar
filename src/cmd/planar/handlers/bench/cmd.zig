//! handlers/bench/cmd.zig — `planar bench {start, event, touch, harvest, finish, show}`
//!
//! Verb group for the measurement-rig recording surface. Wraps the
//! engine/runs lifecycle and harvest modules behind the `planar` CLI.
//! Enum validation (arm, status, kind) happens at the parse layer, not
//! the schema layer, per the run-record-schema design (§2).
//!
//! The verb group is named `bench` (the canonical name chosen in the
//! plan-635 locked decision; `run` was the early draft name in the
//! schema doc).

const cli = @import("cli");

const start = @import("start.zig");
const event = @import("event.zig");
const touch = @import("touch.zig");
const harvest = @import("harvest.zig");
const finish = @import("finish.zig");
const show = @import("show.zig");

pub const verb: cli.Cmd = .{
    .name = "bench",
    .desc = "Record and query benchmark run data (measurement rig).",
    .long_desc =
    \\Record measurement-rig data for the vertical-slice decomposition experiment.
    \\
    \\  Arms: strict, eligibility, grouped (or any free-text pilot value).
    \\  Statuses: running, completed, aborted, error.
    \\  Touch kinds: declared, actual.
    \\
    \\  Workflow: bench start → bench event (repeat) → bench touch (repeat)
    \\            → bench harvest → bench finish → bench show --json.
    ,
    .cmds = &.{
        .{
            .name = "start",
            .desc = "Mint a new run record and print its run_uid.",
            .flags = &.{
                .{ .long = "--plan", .kind = .int, .required = true },
                .{ .long = "--arm", .kind = .string, .required = true },
                .{ .long = "--base-sha", .kind = .string, .required = true },
                .{ .long = "--config-hash", .kind = .string, .required = true },
                .{ .long = "--config-json", .kind = .string },
                .{ .long = "--corpus-repo", .kind = .string },
            },
            .positionals = &.{.{ .name = "run-uid", .kind = .string, .required = true }},
            .run = cli.handler(start.handle),
        },
        .{
            .name = "event",
            .desc = "Append a journal event to a run.",
            .flags = &.{
                .{ .long = "--kind", .kind = .string, .required = true },
                .{ .long = "--seq", .kind = .int, .required = true },
                .{ .long = "--payload", .kind = .string },
            },
            .positionals = &.{.{ .name = "run-uid", .kind = .string, .required = true }},
            .run = cli.handler(event.handle),
        },
        .{
            .name = "touch",
            .desc = "Record a declared or actual file touch for a run.",
            .flags = &.{
                .{ .long = "--task", .kind = .int, .required = true },
                .{ .long = "--path", .kind = .string, .required = true },
                .{ .long = "--kind", .kind = .string, .required = true },
            },
            .positionals = &.{.{ .name = "run-uid", .kind = .string, .required = true }},
            .run = cli.handler(touch.handle),
        },
        .{
            .name = "harvest",
            .desc = "Harvest git diff as actual touches for a run/task.",
            .flags = &.{
                .{ .long = "--task", .kind = .int, .required = true },
                .{ .long = "--worktree", .kind = .string, .required = true },
                .{ .long = "--base", .kind = .string },
                .{ .long = "--head", .kind = .string },
            },
            .positionals = &.{.{ .name = "run-uid", .kind = .string, .required = true }},
            .run = cli.handler(harvest.handle),
        },
        .{
            .name = "finish",
            .desc = "Set the terminal status on a run.",
            .flags = &.{
                .{ .long = "--status", .kind = .string, .required = true },
            },
            .positionals = &.{.{ .name = "run-uid", .kind = .string, .required = true }},
            .run = cli.handler(finish.handle),
        },
        .{
            .name = "show",
            .desc = "Show a run's full state (header + events + touches).",
            .flags = &.{
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{.{ .name = "run-uid", .kind = .string, .required = true }},
            .run = cli.handler(show.handle),
        },
    },
};
