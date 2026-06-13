//! handlers/run/cmd — `planar-agent run {start, end}` parent group.
//!
//! Lifecycle verbs for `workflow_runs` rows. `planar-execute` shells
//! these to stay DB-handle-free (decision 444). `start` inserts a row;
//! `end` moves it to a terminal status. Crash recovery (marking runs
//! `abandoned` when the harness process died) lives in `reconcile`.

const cli = @import("cli");

const start = @import("start.zig");
const end = @import("end.zig");

pub const verb: cli.Cmd = .{
    .name = "run",
    .desc = "Workflow run lifecycle (start / end). Used by planar-execute to stay DB-handle-free.",
    .cmds = &.{
        start.verb,
        end.verb,
    },
};
