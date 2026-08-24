/// @file capture.cppm
/// @brief `planar.cmd.planar.handlers.capture` — six of the seven
/// `planar capture *` leaves (plan 996, task 6040).
///
/// Port target: zig/src/cmd/planar/handlers/capture/{session,end,note,
/// command,file,snapshot}.zig plus util.zig's `resolveSessionId`.
///
/// This layer is genuinely THIN, and that thinness is evidence rather than
/// a shortcut: `planar.engine.runtime.capture` already carried the
/// body-composition halves (`compose_command_body`, `compose_file_body`),
/// the `next_action` fallback (`resolve_next_action`) and every renderer,
/// ported in an earlier cycle. What was missing was only the binary that
/// runs them. The same shape `workspace doctor` has.
///
/// ## The vendor tuple is read from `ctx.env()`, NOT from the engine
///
/// `planar.engine.runtime.capture::resolve_session_id` and
/// `resolve_existing_session_id` exist and do exactly what these leaves
/// need — but they resolve the vendor tuple through
/// `session::vendor_from_env()`, which calls `std::getenv` on the REAL
/// process environment. Using them here would put a second `std::getenv`
/// caller in this binary and silently break `planar.cmd.planar.context`'s
/// stated invariant that `process_env()` is the only one, taking the
/// handler tests' scratch-map environment out of the loop with it.
///
/// So the tuple is resolved HERE from `ctx.env()` and the underlying
/// `session::ensure_active` / `session::active_for_vendor` are called
/// directly. The resolution rule is identical to the engine's and to
/// util.zig's: `$PLANAR_VENDOR` when set and non-empty else `"cli"`, and
/// `$PLANAR_VENDOR_SESSION_ID` when set and non-empty else unset. An empty
/// value reads as ABSENT rather than as an empty vendor, which is the Zig
/// original's `if (v.len > 0)` guard and is pinned in the tests.
///
/// ## `ensure_active` versus `active_for_vendor` is the load-bearing split
///
/// `note`, `command`, `file` and `snapshot` resolve through
/// `ensure_active`, which CREATES a session when none exists — which is
/// why `planar capture note "x"` works on a fresh database with no prior
/// `planar capture session`. `end` resolves through `active_for_vendor`,
/// which does not, and an absent session is `error: no active session`
/// with exit 1. Collapsing the two would make `capture end` silently open
/// a session and then end it.
///
/// ## Exit codes, oracle-captured
///
///   session already bound to a different task   exit 1
///     `session already bound to a different task`
///   `capture end` with no active session        exit 1  `no active session`
///   `capture end <id>` for an absent session    exit 1  `session N not found`
///   `capture end <id>` twice                    exit 1  `session N is already ended`
///   non-integer `<session-id>` positional       exit 2
///     `session id must be an integer, got 'x'`
///
/// The `<session-id>` positional on `end` is declared as a STRING and
/// parsed in the handler, not as an int in the tree, precisely so the
/// oracle's own exit-2 message survives: a tree-level int validator would
/// answer CLI11's `ExtrasError` wording instead.
///
/// ## Not here: `capture commits`
///
/// The seventh leaf. Deferred WITH its dependency — 1205 lines of
/// `git`-subprocess walking in zig/src/engine/runtime/sessioncommits.zig,
/// and no process-spawn seam in this tree. Task 6099 owns it. Omitting an
/// unported child from a group node stays the rule, so `planar capture
/// --help` lists six commands where the oracle lists seven.
module;

export module planar.cmd.planar.handlers.capture;

import std;
import cli11;
import planar.cliapp.args;
import planar.cmd.planar.context;
import planar.cmd.planar.handler;

namespace planar::cmd::handlers {

/// @brief `planar capture session [--vendor v] [--vendor-session-id s]
/// [--model m] [--task N] [--json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure.
export auto capture_session(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief `planar capture end [<session-id>] [--session N] [--summary s]
/// [--json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure.
export auto capture_end(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief `planar capture note <body> [--session N] [--json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure.
export auto capture_note(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief `planar capture command <command> [--outcome o] [--session N]
/// [--json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure.
export auto capture_command(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief `planar capture file <path> [--role r] [--session N] [--json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure.
export auto capture_file(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief `planar capture snapshot [<body>] [--session N] [--task N]
/// [--note n] [--next-action a] [--json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure.
export auto capture_snapshot(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief The vendor tuple these leaves resolve from the context's
/// environment: `$PLANAR_VENDOR` (defaulting to `"cli"`) and
/// `$PLANAR_VENDOR_SESSION_ID`, each treating an EMPTY value as absent.
///
/// Exported because `planar.cmd.planar.handlers.handoff` resolves the
/// same tuple the same way and the rule must not exist twice. An empty
/// value reading as absent is the part worth naming: `PLANAR_VENDOR=`
/// yields `"cli"`, not `""`.
export struct vendor_tuple {
  std::string                vendor;            ///< The resolved vendor, never empty.
  std::optional<std::string> vendor_session_id; ///< The vendor's session id, when set and non-empty.
};

/// @brief Resolve the vendor tuple from an environment lookup.
/// @param env The environment to read.
/// @return The resolved tuple.
export auto resolve_vendor_tuple(const env_lookup& env) -> vendor_tuple;

} // namespace planar::cmd::handlers
