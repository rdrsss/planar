/// @file src/cmd/planar/handlers/health/command.cppm
/// @brief `planar.cmd.planar.handlers.health` — the `planar health` DUAL
/// node and its `hygiene` subcommand (plan 996, tasks 6090 and 6357).
///
/// Port of zig/src/cmd/planar/handlers/health.zig and
/// zig/src/cmd/planar/handlers/health_hygiene.zig.
///
/// ## `health` is a DUAL node
///
/// It has both a handler (this file's `health`) and a subcommand
/// (`health_hygiene`), and `dispatch.cpp` registers the two independently
/// under `"health"` / `"health hygiene"` — same shape as `handoff` and
/// `resume`.
///
/// ## `health hygiene` ALWAYS exits 0 when it produces a report; `health`
/// exits 1 on `degraded`
///
/// Findings are not failures for the SUBCOMMAND — that is stated in the
/// oracle's own long description. Only three refusals are non-zero for
/// `health hygiene`, and all three are exit 1:
///
///   - a negative `--stale-doing` / `--stale-open`
///     (`stale thresholds must be non-negative`)
///   - a `--scope` that resolves to anything but an association —
///     `global` and `repo:<slug>` both refuse
///     (`--scope must name one association`)
///   - a `--scope` that resolves to no row (`scope slug not found`)
///
/// `health` (the PARENT) is the opposite: it always writes its report, then
/// exits 1 whenever `report.overall == "degraded"` — mirroring the oracle's
/// `std.process.exit(1)` after `output.emit` runs. Because this binary's
/// handler contract is "return a value, dispatch decides the exit code" (no
/// `noreturn` die — see `planar.cmd.planar.exit`'s header), the handler
/// writes the report to `ctx.out()` itself and then, on `degraded`, returns
/// `error_from_rendered(domain_error_kind::generic_failure, "")` — an EMPTY
/// stderr payload, so dispatch's `report()` writes nothing extra, matching
/// the oracle's exit(1) with no additional stderr text.
///
/// A missing `$HOME` refuses at exit 1 with
/// `health check failed: resolving install homes: HomeNotSet`, mirroring
/// the oracle's `resolveHomes` -> `exit.die` chain (`skills/common.zig`).
///
/// ## Absent `--scope` reports EVERY scope (hygiene only)
///
/// Unlike `plan list` / `search`, `health hygiene` does NOT resolve a
/// cwd-derived read set: no `--scope` means no scope predicate at all.
/// Hygiene drift is a whole-database question. Oracle-confirmed, and called
/// out because the same absent flag means the opposite on the listing
/// verbs.
module;

export module planar.cmd.planar.handlers.health;

import std;
import planar.cliapp.args;
import planar.cmd.planar.context;
import planar.cmd.planar.handler;
import cli11;

namespace planar::cmd::handlers {

/// @brief Handle `planar health [--json]`.
///
/// The report is always written to `ctx.out()` before this returns,
/// regardless of `overall`. A `"degraded"` report comes back as
/// `std::unexpected(error_from_rendered(generic_failure, ""))` PURELY to
/// drive dispatch's exit code to 1 — the empty rendered payload means
/// dispatch's `report()` appends nothing to stderr, so the only visible
/// effect is the exit code. A genuine refusal (schema/DB/home-resolution
/// failure) returns the ordinary non-empty error instead.
/// @param ctx The process context.
/// @param args The parsed command line.
/// @return See above.
export auto health(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar health hygiene [--scope --stale-doing
/// --stale-open --json]`.
/// @param ctx The process context.
/// @param args The parsed command line.
/// @return Success after writing the report, or the refusal.
export auto health_hygiene(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Declare the `health` group. Folded out of the generated
/// `surface.cpp` at M11.3f (task 6636, decision 1068).
/// @param root The root app to attach it to.
export auto declare_health(CLI::App& root) -> void;

} // namespace planar::cmd::handlers
