/// @file groups.cppm
/// @brief `planar.cmd.planar.handlers.groups` — the `planar groups recommend`
/// leaf (plan 996, task 6189).
///
/// Port target: zig/src/cmd/planar/handlers/groups/recommend.zig.
///
/// The whole `groups` family is this one leaf. It is READ-ONLY: it loads a
/// plan's open tasks, their effective closures and the dependency DAG, and
/// partitions them under a window budget. It writes nothing.
///
/// ## `--solver mtkahypar` is ACCEPTED, and that is not a silent degradation
///
/// The optional external hypergraph solver is not ported (1776 lines of
/// `std.process.run` probing, shelling and partition-file readback, with no
/// process-spawn seam in this tree). The flag is still accepted, because on a
/// machine WITHOUT the solver binary installed the oracle does exactly what
/// this build does: it runs greedy and reports `solver:"greedy"` with
/// `optimal_available:false`. The reporting fields carry that fact into both
/// output forms, so a caller can tell the optimal arm did not run — which is
/// what separates this from the inert-filter defect the brief warns about. A
/// filter that silently does not filter returns plausible rows and says
/// nothing; this says `optimal_available:false` in every envelope.
///
/// An UNKNOWN `--solver` value is still a refusal at exit 2, so the flag is
/// validated rather than ignored.
///
/// This is also why `--solver mtkahypar` is deliberately absent from the
/// oracle parity list: on a machine where the solver IS installed the oracle
/// produces a genuinely different partition, so a case pinning either outcome
/// would pass or fail based on what happens to be on the host.
module;

export module planar.cmd.planar.handlers.groups;

import std;
import cli11;
import planar.cliapp.args;
import planar.cmd.planar.context;
import planar.cmd.planar.handler;

namespace planar::cmd::handlers {

/// @brief Handle `planar groups recommend <plan-id> [--budget <n>]
/// [--solver <s>] [--json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, an `invalid_input` (exit 2) for a non-integer plan id, a
/// `--budget` that is not a non-negative 32-bit integer or an unknown
/// `--solver`, or a `generic_failure` (exit 1) when the plan does not exist.
export auto groups_recommend(context& ctx, const cliapp::parsed_args& args) -> handler_result;

} // namespace planar::cmd::handlers
