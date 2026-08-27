/// @file health.cppm
/// @brief `planar.cmd.planar.handlers.health` — the `planar health
/// hygiene` leaf (plan 996, task 6090).
///
/// Port of zig/src/cmd/planar/handlers/health_hygiene.zig.
///
/// ## Only the SUBCOMMAND lands here; the parent `planar health` does not
///
/// `planar health` itself stays unported and keeps refusing at exit 64.
/// Its blocker is not this layer: its handler folds
/// `engine.installedsurface.status` into every run, and that classifier
/// (548 Zig lines of manifest-driven filesystem inspection) has no
/// counterpart in this tree. See `src/lib/engine/health/health.cppm` for
/// why a `check`-without-projections port would be a leaf that compiles
/// and reports the wrong `overall`.
///
/// The parent/child split is safe to land unevenly here because `health`
/// is a DUAL node — it has both a handler and a subcommand — and
/// `dispatch.cpp` registers the two independently.
///
/// ## This verb ALWAYS exits 0 when it produces a report
///
/// Findings are not failures. That is stated in the oracle's own long
/// description and it is the opposite of its parent, which exits 1 on
/// `degraded`. Only the three refusals below are non-zero, and all three
/// are exit 1:
///
///   - a negative `--stale-doing` / `--stale-open`
///     (`stale thresholds must be non-negative`)
///   - a `--scope` that resolves to anything but an association —
///     `global` and `repo:<slug>` both refuse
///     (`--scope must name one association`)
///   - a `--scope` that resolves to no row (`scope slug not found`)
///
/// ## Absent `--scope` reports EVERY scope
///
/// Unlike `plan list` / `search`, this verb does NOT resolve a cwd-derived
/// read set: no `--scope` means no scope predicate at all. Hygiene drift is
/// a whole-database question. Oracle-confirmed, and called out because the
/// same absent flag means the opposite on the listing verbs.
module;

export module planar.cmd.planar.handlers.health;

import std;
import planar.cliapp.args;
import planar.cmd.planar.context;
import planar.cmd.planar.handler;

namespace planar::cmd::handlers {

/// @brief Handle `planar health hygiene [--scope --stale-doing
/// --stale-open --json]`.
/// @param ctx The process context.
/// @param args The parsed command line.
/// @return Success after writing the report, or the refusal.
export auto health_hygiene(context& ctx, const cliapp::parsed_args& args) -> handler_result;

} // namespace planar::cmd::handlers
