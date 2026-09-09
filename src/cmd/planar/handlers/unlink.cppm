/// @file unlink.cppm
/// @brief `planar.cmd.planar.handlers.unlink` — the `planar unlink
/// <link-id>` leaf (plan 996, task 6106).
///
/// Port target: zig/src/cmd/planar/handlers/unlink.zig.
///
/// ## The composition, and why it could not live one layer down
///
/// `unlink` reaches TWO layer-2 buckets that cannot reach each other:
///
///   engine_external — `link::show` to fail precisely on a bad id, then
///                     `link::remove` to delete the row
///   engine_runtime  — `session::ensure_active` + `session::append_entry`
///                     to leave an audit row behind
///
/// D18 forbids an `engine_external` -> `engine_runtime` edge and
/// `cmake/architecture.cmake` FATALs on it at configure time, so the join
/// has no legal home below this file. That is the same shape `annotate
/// add` proves for engine_identity + engine_planning; this leaf is the
/// second instance, and it is the one task 6105 named first when it
/// recorded what layer 3 was missing for.
///
/// ## `engine_external` did not exist before this task
///
/// `unlink` was blocked TWICE: on layer 3, and on a layer-2 bucket the C++
/// tree had never stood up. `src/lib/engine/external/` was created here,
/// scoped to the create/show/delete third of the Zig original — see that
/// directory's CMakeLists.txt for what was deferred and with which verb.
///
/// ## The audit append is best-effort, and silence is the contract
///
/// The Zig original writes the `session_entries` row through a helper
/// whose every failure path is `catch return` — a missing session, a
/// failed insert, anything. The unlink has already committed by then and
/// must not be reported as failed because its audit trail could not be
/// written. This port keeps that: `append_audit` returns `void` and
/// discards every error. Confirmed against the oracle, which wrote
/// `action | unlink: removed external link 1` into `session_entries` on a
/// database with no prior session at all — `ensure_active` created one.
///
/// ## `--scope` is declared and discarded
///
/// Not an oversight in either implementation. `external_links` carries no
/// scope column, and link verbs are UNGUARDED BY DESIGN (docs/concepts.md
/// § cross-scope-guard). The Zig handler's discard is the literal line
/// `_ = args.scope;`; this port names the flag in the tree so `--help`
/// matches, reads nothing from it, and never calls
/// `planar.cmd.planar.scope`.
module;

export module planar.cmd.planar.handlers.unlink;

import std;
import cli11;
import planar.cliapp.args;
import planar.cmd.planar.context;
import planar.cmd.planar.handler;

namespace planar::cmd::handlers {

/// @brief Handle `planar unlink <link-id> [--scope <s>] [--json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, `invalid_input` (exit 2) when `<link-id>` is not an
/// integer, or `generic_failure` (exit 1) when no such link exists.
export auto unlink(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Declare the `unlink` leaf. Folded out of the generated
/// `surface.cpp` at M11.3f (task 6636, decision 1068).
/// @param root The root app to attach it to.
export auto declare_unlink(CLI::App& root) -> void;

} // namespace planar::cmd::handlers
