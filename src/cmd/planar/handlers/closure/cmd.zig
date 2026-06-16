//! handlers/closure/cmd.zig — `planar closure {compute, show}`
//!
//! Verb group for the derived-closure extractor (M2). `compute` runs the
//! symbol-level extractor over a task's declared seed paths and persists the
//! result to the `closures` table; `show` reads it back. The extractor
//! computes the symbols a task must hold resident from static analysis,
//! replacing the declared-touch path proxy (the experiment's contribution).
//!
//! `compute` WRITES (it is the only write verb in the group); `show` is
//! read-only. Both use standard write-scope resolution against the task's
//! stored scope — no `--no-scope-check`.

const cli = @import("cli");

const compute = @import("compute.zig");
const show = @import("show.zig");

pub const verb: cli.Cmd = .{
    .name = "closure",
    .desc = "Compute and inspect a task's derived symbol-level closure.",
    .long_desc =
    \\Compute the *derived* closure of a task — the symbols it must hold
    \\resident, computed by static analysis from the task's declared seed
    \\paths (task_touch_paths), partitioned by role:
    \\
    \\  modify     — the seed's own edited symbols.
    \\  reference  — the interfaces the seed depends on.
    \\  transitive — deeper hops (stored, but excluded from the effective
    \\               closure by default).
    \\
    \\  Workflow: closure compute <task-id> → closure show <task-id> --json.
    ,
    .cmds = &.{
        .{
            .name = "compute",
            .desc = "Run the extractor over a task's seeds and persist the closure.",
            .flags = &.{
                .{ .long = "--scope", .kind = .string },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{.{ .name = "task-id", .kind = .string, .required = true }},
            .run = cli.handler(compute.handle),
        },
        .{
            .name = "show",
            .desc = "Read back a task's persisted closure rows.",
            .flags = &.{
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{.{ .name = "task-id", .kind = .string, .required = true }},
            .run = cli.handler(show.handle),
        },
    },
};
