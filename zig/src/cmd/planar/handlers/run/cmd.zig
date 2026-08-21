//! handlers/run/cmd.zig — `planar run {start, event, finish, show}`
//!
//! Operational trace surface. Wraps the engine/runs lifecycle module behind
//! the `planar` CLI. This surface is intentionally thin: it provides the
//! minimal start→event→finish→show cycle that workflows emit to, without
//! exposing the measurement-only verbs (touch, harvest) that belong on
//! `planar bench`.
//!
//! One engine, two thin CLI surfaces (decision D10):
//!
//!   planar bench * — measurement surface (strict/eligibility/grouped arms,
//!                    declared/actual touches, git-diff harvest)
//!   planar run *   — operational surface (arm defaults to 'op',
//!                    seq auto-incremented, no touch/harvest)
//!
//! The underlying engine/runs/runs.zig functions are shared without copy.

const cli = @import("cli");

const start = @import("start.zig");
const event = @import("event.zig");
const finish = @import("finish.zig");
const show = @import("show.zig");

pub const verb: cli.Cmd = .{
    .name = "run",
    .desc = "Record and query operational run traces.",
    .long_desc =
    \\Record operational run traces emitted by workflows.
    \\
    \\  Arm defaults to 'op' (or the workflow name when --workflow is given).
    \\  Statuses: running, completed, aborted, error.
    \\
    \\  Workflow: run start → run event (repeat) → run finish → run show --json.
    \\
    \\  See `planar bench` for the measurement-rig surface (strict/eligibility/
    \\  grouped arms, declared/actual touch tracking, git-diff harvest).
    ,
    .cmds = &.{
        .{
            .name = "start",
            .desc = "Mint a new operational run record and print its run_uid as JSON.",
            .flags = &.{
                .{ .long = "--plan", .kind = .int, .required = true },
                .{ .long = "--workflow", .kind = .string },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .run = cli.handler(start.handle),
        },
        .{
            .name = "event",
            .desc = "Append a journal event to a run (seq auto-incremented).",
            .flags = &.{
                .{ .long = "--kind", .kind = .string, .required = true },
                .{ .long = "--payload", .kind = .string },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{.{ .name = "run-uid", .kind = .string, .required = true }},
            .run = cli.handler(event.handle),
        },
        .{
            .name = "finish",
            .desc = "Set the terminal status on a run.",
            .flags = &.{
                .{ .long = "--status", .kind = .string, .required = true },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{.{ .name = "run-uid", .kind = .string, .required = true }},
            .run = cli.handler(finish.handle),
        },
        .{
            .name = "show",
            .desc = "Show a run's full state (header + events).",
            .flags = &.{
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{.{ .name = "run-uid", .kind = .string, .required = true }},
            .run = cli.handler(show.handle),
        },
    },
};
