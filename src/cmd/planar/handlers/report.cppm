/// @file report.cppm
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
/// ## `verb_path` leaks `search`'s free-text query — reproduced, not fixed
///
/// `engine::introspect::cli_preview_jsonl` selects `cli_invocations.
/// verb_path` verbatim, and `search <query>` records `verb_path = "search
/// <query>"` (task 6351's finding, pinned in `introspect.t.cpp`). This
/// handler renders that column through to `[invocations]`/`[failure
/// tail]`/the JSONL boundary the CLI adapter feeds into `preview`'s own
/// `cli_log` coverage row unchanged — this is the oracle's own behavior,
/// not a defect this port introduces or an exposure this handler's own
/// docs claim to redact.
module;

export module planar.cmd.planar.handlers.report;

import std;
import planar.cliapp.args;
import planar.cmd.planar.context;
import planar.cmd.planar.handler;

namespace planar::cmd::handlers {

/// @brief Handle `planar report [--days --tail --json]`.
///
/// @param ctx The process context (database handle, streams, cwd, env).
/// @param args The parsed command line.
/// @return Success after writing the rendered bundle, or the refusal.
export auto report(context& ctx, const cliapp::parsed_args& args) -> handler_result;

} // namespace planar::cmd::handlers
