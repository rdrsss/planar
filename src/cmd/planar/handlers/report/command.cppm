/// @file src/cmd/planar/handlers/report/command.cppm
/// @brief `planar.cmd.planar.handlers.report` — the `planar report` leaf
/// (plan 996, task 6352).
///
/// Port of zig/src/cmd/planar/handlers/report.zig. Two-step assembly,
/// exactly mirroring the oracle: `engine::introspect::build` for the
/// DB-aggregate half, then `engine::introspection_adapters::
/// collect_preview_from_paths` for the filesystem-discovery half, glued
/// together here because `bundle::preview` and the collector both live in
/// engine buckets that cannot import each other (D15) — see decision 981
/// and `introspect.cppm`'s header for the full account.
///
/// ## Flag validation, oracle-captured
///
///   planar report --days 0   exit 2, "error: --days must be a positive integer (got 0)"
///   planar report --tail 0   exit 2, "error: --tail must be a positive integer (got 0)"
///
/// Both flags default to a positive value (`--days` 30, `--tail` 20) via
/// the declared CLI11 defaults `cliapp::harvest` seeds, so the refusal is
/// reachable only through an EXPLICIT non-positive value, never through
/// omission.
///
/// ## `report` IS hermetically parity-testable
///
/// A prior cycle's note in `handlers/CMakeLists.txt` (superseded by this
/// one landing) claimed otherwise, having measured against the real
/// operator home. Under a pinned scratch `$HOME` every vendor reports
/// `coverage: [{"vendor":"claude","state":"unavailable","scanned":0,...}]`
/// (each of the three transcript vendors probes a directory that does not
/// exist under the scratch home) and the CLI adapter reports
/// `available:false` when `[introspection].cli_log` is off — fully
/// deterministic, same class of fix as commit 3ec6c37. This handler reads
/// `$HOME` ONLY through `ctx.env()` (never `std::getenv` directly), so a
/// test context constructed over `map_env({...})` cannot reach the real
/// operator home by accident — same structural protection
/// `context::env_lookup` already gives every other handler.
///
/// ## `verb_path` is masked by the live CLI catalog
///
/// Older writers recorded `search <query>` verbatim in `cli_invocations.
/// verb_path`. This handler passes `verb_path_recognized` (built from the
/// live CLI tree in `cli_log`) to the introspection engine, which renders a
/// stored value the catalog rejects as `<unrecognized>` in `[invocations]`,
/// `[failure tail]` and the JSONL boundary that feeds `preview`'s `cli_log`
/// evidence. Rows are never purged; the 90-day retention ages them out.
module;

export module planar.cmd.planar.handlers.report;

import std;
import planar.cliapp.args;
import planar.cmd.planar.context;
import planar.cmd.planar.handler;
import cli11;

namespace planar::cmd::handlers {

/// @brief Handle `planar report [--days --tail --json]`.
///
/// @param ctx The process context (database handle, streams, cwd, env).
/// @param args The parsed command line.
/// @return Success after writing the rendered bundle, or the refusal.
export auto report(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Declare the `report` leaf. Folded out of the generated
/// `surface.cpp` at M11.3f (task 6636, decision 1068).
/// @param root The root app to attach it to.
export auto declare_report(CLI::App& root) -> void;

} // namespace planar::cmd::handlers
