/// @file feedback.cppm
/// @brief `planar.cmd.planar.handlers.feedback` — the `feedback triage`
/// leaves (plan 996, task 6303).
///
/// Port target: `zig/src/cmd/planar/handlers/feedback/triage/{list,show,set}.zig`
/// (20 / 21 / 27 lines). The handlers are the smallest in the inventory; the
/// 302-line `planar.engine.planning.feedback_triage` they call is the work,
/// and sizing this family by its handlers is the mistake task 6299 flagged.
///
/// `feedback` and `feedback triage` are both PURE GROUPS and stay
/// unregistered so they fall to the help path at exit 0, matching every
/// other group node in the tree.
///
/// THREE CONTRACTS CAPTURED FROM THE ORACLE, not inferred from siblings:
///
///   1. REFUSAL ORDER. Everything the handler can reject, it rejects in
///      argv order before touching the engine: finding ref, then severity,
///      then disposition, then reproduction, then `--duplicate-of`. A probe
///      passing a bad ref AND a bad severity gets the REF message, so a
///      port that validates enums first is observably wrong.
///   2. TWO DIFFERENT VERB PREFIXES on `set`'s engine failures.
///      `entity_scope`'s failures read `feedback finding: <Tag>`; the
///      engine `set`'s read `feedback triage set: <Tag>`. They are separate
///      calls in the oracle with separate `die` strings, and collapsing
///      them changes the bytes for a missing task (`feedback finding:
///      NotFound`, exit 1).
///   3. `InvalidInput` FROM THE ENGINE EXITS 2, every other tag this family
///      raises exits 1. `MissingFeedbackPlan`, `DifferentFeedbackPlan`,
///      `AmbiguousFeedbackPlan` and `DuplicateCycle` have no arm in the
///      oracle's `codeFor` and fall to the generic bucket; `InvalidInput`
///      does have one. All five were run against the oracle.
///
/// `feedback triage list` has NO `--scope` flag and does no scope filtering
/// — deliberately not "fixed" here. `feedback triage set` is the only
/// mutating leaf and is the family's second caller of
/// `scope::guard_with_membership`, which moved out of `handlers/sync` for
/// exactly that reason.
module;

export module planar.cmd.planar.handlers.feedback;

import std;
import cli11;
import planar.cliapp.args;
import planar.cmd.planar.context;
import planar.cmd.planar.handler;

namespace planar::cmd::handlers {

/// @brief Handle `planar feedback triage list [--plan --severity --disposition --json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure to report.
export auto feedback_triage_list(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar feedback triage show <finding> [--json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure to report.
export auto feedback_triage_show(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar feedback triage set <finding> --severity --disposition
/// --reproduction [--duplicate-of --evidence --scope --json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure to report.
export auto feedback_triage_set(context& ctx, const cliapp::parsed_args& args) -> handler_result;

} // namespace planar::cmd::handlers
