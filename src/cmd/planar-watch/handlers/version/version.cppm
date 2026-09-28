/// @file version.cppm
/// @brief `planar.cmd.planar_watch.handlers.version` — the `planar-watch
/// version` leaf (plan 996, task 6107).
///
/// Port target: zig/src/cmd/planar-watch/handlers/version.zig, whose own
/// header states the contract this leaf must keep: the binary name prefix
/// exists "so operators can tell the three binaries apart in scripts", with
/// the same field order "so a shell grep on the leading `planar-watch `
/// prefix is stable".
///
/// Same inherited `cxx` vs `zig` divergence as the other two binaries'
/// version leaves: no Zig runtime here to report. Note that the field
/// COUNT does NOT survive it, contrary to `planar.cliapp.version`'s header —
/// `compiler_version_string()` returns `Clang 22.1.8`, whose own space
/// makes the line six tokens against the oracle's five. Task 6106 found it
/// first on the operator binary; see
/// `planar.cmd.planar_agent.handlers.version`'s header for the full note.
/// The leading `planar-watch ` prefix — the thing the Zig header says
/// operators actually grep on — is unaffected.
///
/// This leaf also carries a second, quieter proof: it opens no database.
/// On the read-only binary that matters more than elsewhere, because the
/// cheapest way to accidentally break the "a viewer never creates state"
/// rule is to open eagerly at startup. `handlers.t.cpp` asserts
/// `ctx.db().opened()` is still false afterwards.
module;

export module planar.cmd.planar_watch.handlers.version;

import std;
import cli11;
import planar.cliapp.args;
import planar.cmd.planar_watch.context;
import planar.cmd.planar_watch.handler;

namespace planar::cmd::watch::handlers {

/// @brief Handle `planar-watch version`.
/// @param ctx The invocation context.
/// @param args The parsed arguments (the leaf declares none).
/// @return Success; this leaf has no failure path.
export auto version(context& ctx, const cliapp::parsed_args& args) -> handler_result;

} // namespace planar::cmd::watch::handlers
