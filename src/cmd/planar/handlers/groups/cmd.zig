//! handlers/groups/cmd.zig — `planar groups recommend`
//!
//! Verb group for the M3 grouping arm. `recommend` forms **slices** (groups
//! of tasks sharing a context window) over a plan's open tasks by minimizing
//! closure replication under a window budget — the read-only sibling to
//! `plan recommend-strategy`. It computes and reports; it writes nothing.
//!
//! M3.2 backs `recommend` with the GREEDY heuristic (M3.1); the Mt-KaHyPar
//! solver (M3.3) is swapped in later behind the same verb.

const cli = @import("cli");

const recommend = @import("recommend.zig");

pub const verb: cli.Cmd = .{
    .name = "groups",
    .desc = "Recommend task slices that minimize closure replication.",
    .long_desc =
    \\Form **slices** — groups of a plan's open (todo) tasks that share a
    \\context window — by minimizing the duplicated closure across slices,
    \\subject to a per-slice token budget. Read-only sibling to
    \\`plan recommend-strategy`: it reports a recommendation, writing nothing.
    \\
    \\Each slice reports its member task ids, its unioned effective closure
    \\(the distinct symbols the slice must hold resident, role modify ∪
    \\reference), and that union's token cost. No slice's cost exceeds the
    \\budget, and the slice-DAG induced by the task `blocks` dependencies is
    \\always schedulable (no slice is grouped across a dependency violation).
    \\
    \\  --solver greedy|mtkahypar  (default greedy) selects the partitioner.
    \\  `mtkahypar` is the optional external hypergraph solver: when its binary
    \\  is absent or fails, the verb degrades to greedy and reports
    \\  `optimal_available:false` (it never errors on a missing optional dep).
    \\
    \\  Workflow: closure compute <task> (per task) → groups recommend <plan>.
    ,
    .cmds = &.{
        .{
            .name = "recommend",
            .desc = "Recommend closure-minimizing task slices for a plan.",
            .flags = &.{
                .{ .long = "--budget", .kind = .string },
                .{ .long = "--solver", .kind = .string },
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .positionals = &.{.{ .name = "plan-id", .kind = .string, .required = true }},
            .run = cli.handler(recommend.handle),
        },
    },
};
