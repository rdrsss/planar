/// @file question.cppm
/// @brief `planar.cmd.planar.handlers.question` — the five `planar
/// question` CRUD/transition leaves (plan 996 roadmap M12 item 4, task
/// 6188).
///
/// Port target: zig/src/cmd/planar/handlers/question/{add,show,list,
/// answer,wontfix}.zig.
///
/// ## The family is TEN leaves; five land here, five are named refusals
///
/// `question edit`, `question view`, `question diff` and `question review`
/// are the workbench DRAFTING quartet every planning entity carries. They
/// do not extend CRUD — they round-trip the row through a file under
/// `$PLANAR_WORKBENCH_ROOT`, diff the file against the row, and gate the
/// re-import. That is `engine_workbench` plumbing plus an editflow layer,
/// not question logic, and none of the four is reachable from what is
/// ported. They stay declared exit-64 refusals that name themselves.
///
/// `question link` is a different kind of hold. It is not question-specific
/// at all: it is one arm of the shared entity-link verb surface, alongside
/// `plan link`, `task link` and `links add/list/remove/trail` — ALL of
/// which still refuse. `engine_entitylink` is ported, but it exports no
/// renderer, so wiring this one arm would mean deriving the link output
/// bytes for a family whose other six arms still refuse. Landing them
/// together in one cycle keeps that surface coherent; landing `question
/// link` alone would make `planar question link 1 plan:2` work while
/// `planar plan link 2 plan:1` refuses, which reads as a bug rather than
/// as a milestone boundary.
///
/// ## Where the scope comes from, and the two verbs that differ
///
/// `question add` cwd-derives its write scope when `--scope` is absent, and
/// — unlike the sibling `plan create` — does NOT refuse inside a project
/// that has no association. `resolved->scope` staying unset is the CORRECT
/// outcome there and lets the engine write `scope_kind='global'`. That is
/// the oracle's behaviour (`handlers/question/add.zig` has no
/// `project_unassociated` arm) and it was confirmed by running `question
/// add` in a registered-but-unassociated project: the row lands `global`.
/// `task add` makes the same choice for the same reason; `plan create` is
/// the outlier, not this.
///
/// `question list` cwd-derives a READ SET when `--scope` is absent, and an
/// EXPLICIT `--scope` bypasses the read set entirely so the engine reports
/// its own `SlugNotFound`. Both `--scope` and `--status` are
/// COMMA-SEPARATED on this verb (`--status open,wontfix` is two statuses),
/// which is `plan list`'s shape rather than `task list`'s single-value one.
/// Verified by running it, not inferred from the flag's declared type —
/// the schema declares both as plain strings and cannot express the
/// difference.
///
/// ## Three refusals with three different exit codes on ONE verb
///
/// `question answer` alone spans the table, and each row was captured from
/// the oracle rather than reasoned about:
///
///   - `--answer` ABSENT          -> exit 2, `--answer is required`
///   - `--answer ""` (present, empty) -> exit 1, `--answer must be non-empty`
///   - `wontfix -> answered`      -> exit 1, `question answer: IllegalTransition`
///
/// The first two differ because zig raises `error.InvalidInput` for the
/// missing flag (mapped to 2) and the engine raises `error.AnswerRequired`
/// for the empty one, which has no arm in `codeFor` and falls to the
/// generic 1 bucket. Collapsing them would be tidier and would diverge.
///
/// A NotFound on `show`/`answer`/`wontfix` has its OWN message — `no
/// question with id 999` — not the generic `question show: NotFound`
/// shape the other engine errors take.
module;

export module planar.cmd.planar.handlers.question;

import std;
import cli11;
import planar.cliapp.args;
import planar.cmd.planar.context;
import planar.cmd.planar.handler;

namespace planar::cmd::handlers {

/// @brief Handle `planar question add <title> [--body --scope --plan --editor --json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure to report.
export auto question_add(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar question show <question-id> [--json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure to report.
export auto question_show(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar question list [--scope --status --touches --plan --json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure to report.
export auto question_list(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar question answer <question-id> --answer <text> [--json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure to report.
export auto question_answer(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar question wontfix <question-id> [--reason --json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure to report.
export auto question_wontfix(context& ctx, const cliapp::parsed_args& args) -> handler_result;

} // namespace planar::cmd::handlers
