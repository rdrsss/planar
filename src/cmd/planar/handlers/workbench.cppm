/// @file workbench.cppm
/// @brief `planar.cmd.planar.handlers.workbench` — the ten ported
/// `planar workbench *` leaves (plan 996, task 6037).
///
/// Port target: zig/src/cmd/planar/handlers/workbench/{lint,pull,push,
/// status,resolve,sync,archive,restore,gc,list}.zig plus the argument
/// halves of common.zig.
///
/// The PRINTING halves of those files live one layer down, in
/// `planar.engine.workbench.render_cli`, so their exact bytes are testable
/// without spawning a process. What is left here is genuinely the wiring:
/// resolve the plan argument, resolve the workbench root, call the engine,
/// write the payload, map the failure onto an exit code.
///
/// ## Where the workbench root comes from — and which leaves CREATE it
///
/// Every leaf resolves it through `planar.engine.workbench.root`, handing
/// it the context's env callable (never `std::getenv`). Four of them —
/// `list`, `gc`, `archive`, `restore` — then CREATE the root directory if
/// it is missing, mirroring the Zig `resolveAndEnsureWorkbenchRoot`;
/// `status` and `lint` resolve without creating, mirroring the plain
/// `resolveRoot` call. `pull`, `push` and `sync` create the FEATURE
/// directory instead, inside the engine. That split is reproduced rather
/// than unified: it decides whether a bare `workbench status` on a fresh
/// machine leaves a directory behind.
///
/// ## Exit codes, oracle-captured
///
///   plan not found                exit 1  `plan not found: 999`
///   plan argument `0` / negative   exit 2  `invalid plan '0'`
///   malformed workbench file(s)   exit 1
///   sync conflict(s)              exit 3
///   lint found issues             exit 1  (warnings alone still fail)
///   lint target missing           exit 1
///   lint target not a `.md` file  exit 2
///   bad `--filter-mode`           exit 2
///   `--apply-cleanup` with `--filter-mode all`  exit 2
///   gc drift refusal              exit 1, message on STDERR with no
///                                 `error: ` prefix
module;

export module planar.cmd.planar.handlers.workbench;

import std;
import cli11;
import planar.cliapp.args;
import planar.cmd.planar.context;
import planar.cmd.planar.handler;

namespace planar::cmd::handlers {

/// @brief `planar workbench lint [<plan>] [--all] [--path <p>] [--json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure.
export auto workbench_lint(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief `planar workbench pull <plan> [--verbose] [--json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure.
export auto workbench_pull(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief `planar workbench push <plan> [--verbose] [--json] [--filter-mode m]
/// [--apply-cleanup]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure.
export auto workbench_push(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief `planar workbench status [<plan>] [--verbose] [--json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure.
export auto workbench_status(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief `planar workbench resolve <event-id> --prefer fs|db [--json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure.
export auto workbench_resolve(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief `planar workbench sync <plan> [--verbose] [--json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure.
export auto workbench_sync(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief `planar workbench archive <plan> [--json] [--filter-mode m]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure.
export auto workbench_archive(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief `planar workbench restore <plan> [--json] [--filter-mode m]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure.
export auto workbench_restore(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief `planar workbench gc [<plan>] [--dry-run] [--yes] [--filter-mode m]
/// [--all-scopes] [--json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure.
export auto workbench_gc(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief `planar workbench list [--json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure.
export auto workbench_list(context& ctx, const cliapp::parsed_args& args) -> handler_result;

} // namespace planar::cmd::handlers
