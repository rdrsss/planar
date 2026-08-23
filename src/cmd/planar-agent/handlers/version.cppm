/// @file version.cppm
/// @brief `planar.cmd.planar_agent.handlers.version` — the `planar-agent
/// version` leaf (plan 996, task 6107).
///
/// Port target: zig/src/cmd/planar-agent/handlers/version.zig.
///
/// The cheapest end-to-end proof that this binary's spine works: argv in,
/// parsed against THIS binary's tree, dispatched, bytes on stdout, exit 0,
/// with no database opened.
///
/// The PROGRAM NAME in the output is the point of the leaf, not
/// decoration. zig/src/cmd/planar-watch/handlers/version.zig's own header
/// says the prefix exists "so operators can tell the three binaries apart
/// in scripts" and that the field order is stable so "a shell grep on the
/// leading `planar-watch ` prefix" works. `planar.cli.version` grew a
/// program-name overload in this task for exactly that; the previous
/// single-argument form hardcoded `"planar "` and would have made this
/// binary report itself as the operator one.
///
/// Like `planar version`, the line is deliberately NOT byte-identical to
/// the oracle, and the divergence is inherited rather than introduced:
/// this tree renders `cxx <compiler-version>` where the Zig binary renders
/// `zig <zig-version>`, because there is no Zig runtime here to report.
///
/// DO NOT REPEAT `planar.cli.version`'s claim that this "preserves the
/// field COUNT". It does not, and task 6106 already recorded the same
/// finding on the operator binary: `compiler_version_string()` returns
/// `Clang 22.1.8`, which contains a space of its own, so the line splits
/// into SIX whitespace-separated tokens where the oracle's splits into
/// five. Field POSITION 0-3 does match — a script keying on the leading
/// `planar-agent ` prefix, or on the sha and date tokens, is stable — but
/// one indexing the LAST field is not. `handlers.t.cpp` and `parity.t.cpp`
/// both pin the actual behaviour so a later layer-1 fix surfaces as a
/// failing test rather than passing silently.
module;

export module planar.cmd.planar_agent.handlers.version;

import std;
import planar.cli;
import planar.cmd.planar_agent.context;
import planar.cmd.planar_agent.handler;

namespace planar::cmd::agent::handlers {

/// @brief Handle `planar-agent version`.
/// @param ctx The invocation context.
/// @param args The parsed arguments (the leaf declares none).
/// @return Success; this leaf has no failure path.
export auto version(context& ctx, const cli::match_result& args) -> handler_result;

} // namespace planar::cmd::agent::handlers
