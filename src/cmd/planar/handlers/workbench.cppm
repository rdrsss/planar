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

/// @brief `planar workbench extract-questions <plan> [--json]`.
///
/// Read-only: it scans the feature tree that `workbench push` already
/// wrote and never touches the database or the filesystem. An ABSENT tree
/// is not an error — it prints a `run 'workbench push <plan>' first` hint
/// and exits 0.
///
/// The scan is top-level-only and skips `README.md`; see
/// `planar.engine.workbench.questions` for why that is the leaf's most
/// misleading property.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure.
export auto workbench_extract_questions(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief `planar workbench edit <plan> [--editor <cmd>] [--json]`.
///
/// `push` -> spawn `$EDITOR` on the FEATURE DIRECTORY -> `pull`, printing
/// a sync summary either side. The editor is given the directory, not a
/// temp file, which is what separates this leaf from the drafting quartet's
/// `edit` arms.
///
/// It always uses the default failure-terminal filter and never
/// auto-cleans: the editor-first flow exposes neither `--filter-mode` nor
/// `--apply-cleanup`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure.
export auto workbench_edit(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief `planar workbench publish <plan> --system <slug> [--json]`.
///
/// Renders the feature's workbench, concatenates every manifest file into ONE
/// body, and POSTs it as a single external mirror, recording the link.
///
/// ## Its blocker was one 36-line function, not a 1205-line file
///
/// This leaf sat in the unported inventory carried as "needs
/// `extsync.parent_issue.recordLink` and `create_remote`, and lands with the
/// adapters" — read as blocked on the whole create/propagate half of
/// `engine_extsync` (3665 lines). Task 6335 measured it by symbol: its ONLY
/// reach into that surface is `recordLink`, 36 lines of SQL with no adapter,
/// transport, credential or template edge, now
/// `engine::external::link::record_mirror_link`. `create_remote` was already
/// in this tree, TU-private to `ext.cpp`; it is now shared. Sixth over-stated
/// blocker of this milestone.
///
/// ## Four refusals, all BEFORE the POST
///
/// Unlike `ext create` (defects 6312/6313), every check here precedes the
/// side effect: unknown plan, unknown system, an EXISTING link on that
/// system, and unresolved workbench conflicts all refuse before anything is
/// sent. The duplicate check makes the verb refuse-on-repeat rather than
/// idempotent-skip — a THIRD shape, distinct from both `ext create`'s
/// duplicate-POST and `ext propagate-one`'s skip. All three ship as measured.
///
/// ## The bundle separator and marker are observable
///
/// Files join with `\n\n---\n\n` and each is preceded by
/// `<!-- planar-workbench: <path> -->\n\n`. The manifest is ordered by
/// `file_path` in SQL, so the body is deterministic. A file over 4 MiB is a
/// read failure, not a truncation.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure.
export auto workbench_publish(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Declare the `workbench` command tree on `root`.
///
/// The CLI declaration for every `workbench` node, colocated with the
/// handlers above (plan 1051, M11.3b — decision 1068). Ten of the thirteen
/// children were hand-declared in `tree.cpp` and three (`publish`,
/// `extract-questions`, `edit`) came from `surface.cpp`'s generated table;
/// the two halves are ONE list here, in catalog order.
///
/// `edit` is declared here even though `workbench_edit` is handled in
/// `handlers/drafting.cpp`: colocation is PER DOMAIN, so a domain's sibling
/// order stays one contiguous list.
///
/// `tree.cpp`'s `add_filter_mode` helper came with this group — it was
/// shared by `push`, `archive`, `restore` and `gc` and by nothing else — so
/// its four call sites are now four `add_string` calls carrying the same
/// description.
/// @param root The root app to attach the `workbench` group to.
export auto declare_workbench(CLI::App& root) -> void;

} // namespace planar::cmd::handlers
