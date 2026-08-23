/// @file version.cppm
/// @brief `planar.cmd.planar.handlers.version` — the `planar version` leaf
/// (plan 996, task 6105).
///
/// The cheapest possible proof that the spine works end to end: argv in,
/// parsed, dispatched, bytes on stdout, exit 0 — with no database, no
/// scope, and no engine bucket involved. It is in the subset because a
/// failure anywhere in that spine shows up here FIRST and unambiguously,
/// not because it is interesting.
///
/// It is also the ONE leaf in this subset that is deliberately not
/// byte-identical to the oracle, and the divergence is inherited, not
/// introduced: `planar.cliapp.version`'s port renders `cxx <compiler-version>`
/// where the Zig binary renders `zig <zig-version>`, because this binary
/// has no Zig runtime to report. The field POSITION and COUNT match — a
/// script splitting the line on whitespace still finds five tokens. See
/// that module's header for the oracle capture (`planar dev dev zig
/// 0.16.0`).
module;

export module planar.cmd.planar.handlers.version;

import std;
import cli11;
import planar.cliapp.args;
import planar.cmd.planar.context;
import planar.cmd.planar.handler;

namespace planar::cmd::handlers {

/// @brief Handle `planar version`.
/// @param ctx The invocation context.
/// @param args The parsed arguments (the leaf declares none).
/// @return Success; this leaf has no failure path.
export auto version(context& ctx, const cliapp::parsed_args& args) -> handler_result;

} // namespace planar::cmd::handlers
