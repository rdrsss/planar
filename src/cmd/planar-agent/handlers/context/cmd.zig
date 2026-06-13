//! handlers/context/cmd — `planar-agent context {add, list, resolve, capsule}` parent group.
//!
//! Run-scoped working-memory verbs for `context_records`. Workers holding
//! a claim write records via `add`; anyone can read them via `list`;
//! `resolve` transitions records through the active → consumed|superseded
//! lifecycle (the primitive that stage-close compaction, task 3905, calls);
//! `capsule` writes a compiled capsule row keyed to the run (not a claim —
//! decision 456, plan 585 task 3905).

const cli = @import("cli");

const add = @import("add.zig");
const capsule = @import("capsule.zig");
const list = @import("list.zig");
const resolve = @import("resolve.zig");

pub const verb: cli.Cmd = .{
    .name = "context",
    .desc = "Run-scoped working-memory records (add / list / resolve / capsule). Used by planar-execute workers.",
    .cmds = &.{
        add.verb,
        capsule.verb,
        list.verb,
        resolve.verb,
    },
};
