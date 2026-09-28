/// @file src/cmd/planar/handlers/capture/command.cppm
/// @brief `planar.cmd.planar.handlers.capture` — all seven
/// `planar capture *` leaves as of plan 996 task 6358, which added
/// `commits` (the group was six of seven since task 6040).
///
/// Port target: zig/src/cmd/planar/handlers/capture/{session,end,note,
/// command,file,snapshot,commits}.zig plus util.zig's `resolveSessionId`.
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
/// ## `capture commits`, landed at task 6358
///
/// The seventh leaf. Unlike the other six, its `--session` resolution does
/// NOT create a session when none is active — it shares `capture end`'s
/// `active_for_vendor` lookup, not the create-on-demand
/// `resolve_or_create_session` the append-style four use. `planar capture
/// commits --since HEAD` on a fresh database is `error: no active session
/// (run 'planar capture session' first)`, exit 2.
///
/// Its trailing SHA list is NOT a real CLI11 positional in the oracle's own
/// terms: etcli-zig's `rest_field` mechanism captures leftover argv into a
/// named slice without ever appearing in the `positionals` array the
/// oracle's `schema` catalog renders (confirmed against a live oracle
/// run). `declare_capture` below mirrors that by declaring a
/// `->group("")`-hidden positional; see its header.
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

/// @brief `planar capture commits [--session N] [--repo dir]
/// (--since ref | sha...) [--json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure.
export auto capture_commits(context& ctx, const cliapp::parsed_args& args) -> handler_result;

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

/// @brief Declare the `capture` command tree on `root`.
///
/// The CLI declaration for every `capture` node, colocated with the
/// handlers above (plan 1051, M11.3d — decision 1068). All eight were
/// SHADOWED before this fold: hand-declared in `tree.cpp` AND separately
/// described by a `node_spec` that `apply_surface`'s find-or-create arm
/// skipped. The two were compared field by field first and agreed on
/// every one, so the merge below decided nothing by picking a winner.
///
/// `commits`'s trailing SHA list is the one thing only the hand
/// declaration ever carried, because the generated table cannot express
/// it: it is declared `->group("")`-HIDDEN rather than as an ordinary
/// positional. That is not a style choice. The oracle's own `rest_field`
/// mechanism was not a real positional and never appeared in its `schema`
/// catalog (`capture commits` reports `"positionals":[]`), so declaring
/// this visibly would add a catalog entry with no counterpart. `group("")`
/// hides it from `schema`/`--help`/completion the same way CLI11 hides
/// `--help` itself, while `harvest()` still collects it — visibility is a
/// rendering concern, not a parsing one. See `parsed_args::positional_lists`.
///
/// `end`'s `<session-id>` is declared a STRING even though it names an
/// integer, and that is load-bearing: the handler parses it and answers
/// `session id must be an integer, got 'x'` with exit 2, where an
/// `add_int`-style validator would answer CLI11's `ValidationError`
/// wording instead.
/// @param root The root app to attach the `capture` group to.
export auto declare_capture(CLI::App& root) -> void;

} // namespace planar::cmd::handlers
