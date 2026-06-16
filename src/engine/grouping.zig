//! engine.grouping — task-grouping heuristics & solvers (M3).
//!
//! Forms **slices** (groups of tasks sharing a context window) that minimize
//! closure replication under the window budget. M3.1 (this file's `greedy`
//! submodule) is the greedy overlap-merge first pass — a budget-exact heuristic
//! that unblocks the grouped measurement arm before the hypergraph solver
//! (M3.3) lands. Later milestones add the `planar groups recommend` verb (M3.2)
//! and the Mt-KaHyPar binding (M3.3).

pub const greedy = @import("grouping/greedy.zig");

test {
    _ = greedy;
}
