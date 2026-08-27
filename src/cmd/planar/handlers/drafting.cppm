/// @file drafting.cppm
/// @brief `planar.cmd.planar.handlers.drafting` — the TWENTY-FOUR
/// `edit | view | diff | review` leaves across `plan`, `task`, `question`,
/// `decision`, `scenario` and `artifact` (plan 996 roadmap M12; the
/// link-anchored sixteen at task 6205, `plan` and `task` at task 6208).
///
/// Port target: `zig/src/cmd/planar/handlers/{plan,task,question,decision,
/// scenario,artifact}/{edit,view,diff,review}.zig` — twenty-four files
/// whose combined body is four distinct shapes repeated six times.
///
/// ## Why ONE module and not six family additions
///
/// The twenty-four Zig shims are byte-identical modulo the family noun and
/// its positional name. Not "similar": identical. `question/diff.zig` and
/// `artifact/diff.zig` differ in exactly three string literals and the
/// `editflow` enum arm, and `plan`'s and `task`'s differ from them in the
/// same three places — which is why the eight added at 6208 needed no new
/// handler body at all, only entry points and the oracle run that proved
/// the SHARED body was right for them.
///
/// So they land here together, following the precedent
/// `handlers/links.cppm` set for the seven-arm entity-link surface and
/// which `handlers/question.cppm`'s header cites: a shared body plus thin
/// named entry points, rather than six copies of the same twenty lines in
/// six family files. D19's rule — a second copy of a RULE drifts — applies
/// with force here, because the rules being copied are the refusal wordings
/// and the id-validation asymmetry below, and a drifted copy of either is
/// invisible until an operator hits that one family.
///
/// ## `plan` and `task` REACH AN ARM THE OTHER FOUR CANNOT
///
/// The shared body is unchanged for them, but the failure prose is not,
/// and it is the one thing about these eight a reader must not assume
/// transfers. On a NONEXISTENT id the four link-anchored families resolve
/// their anchor through `entity_links` and report `no_plan_link`; `plan`
/// walks `plans.parent_plan_id` and `task` reads `tasks.plan_id`, so both
/// report `not_found` instead:
///
///   `question diff 999`  ->  `question 999 is not linked to a plan; ...`
///   `plan diff 999`      ->  `no plan with id 999`
///   `task diff 999`      ->  `no task with id 999`
///
/// So `prose_error`'s `not_found` arm — which `editflow.cpp` documents as
/// unreachable for the original four — is the ONLY arm these two reach.
/// Oracle-confirmed on both families and pinned in `drafting_leaves.t.cpp`.
///
/// ## THE ASYMMETRY BETWEEN THE FOUR VERBS, which is the whole contract
///
/// It would be natural to give all four verbs the same preamble. The oracle
/// does not, and the difference is operator-visible on every failing
/// invocation:
///
///                     id <= 0 checked?   failures mapped to prose?
///   `view`                   NO                    NO
///   `edit`                   NO                    NO
///   `diff`                   YES                   YES
///   `review`                 YES                   YES
///
/// `question diff 0`   -> exit 2, `question id must be a positive integer, got 0`
/// `question view 0`   -> falls through to the resolver and reports NoPlanLink
///
/// `view` and `edit` have NO `catch` arms in the oracle at all, so a failure
/// reaches the Zig runtime and prints the bare error tag plus a stack trace.
/// `diff` and `review` map `NotFound` and `NoPlanLink` to prose and
/// everything else to `<family> <verb> failed: <Tag>`.
///
/// Both shapes are reproduced. See `planar.cmd.planar.editflow`'s header for
/// the ONE deliberate divergence — the trace frames, which this build omits.
///
/// ## `--no-pull` and `--json` on `edit` are DECLARED AND INERT
///
/// `question edit` declares both; the oracle's handler reads neither. The
/// flags exist in the surface because the oracle declares them, and a tree
/// that dropped them would fail catalog parity. Accepting-and-ignoring is
/// normally the defect this port keeps closing — but here it is what the
/// oracle does, and the alternative (refusing `--no-pull`) would refuse an
/// invocation the oracle accepts. Recorded rather than silently mirrored.
module;

export module planar.cmd.planar.handlers.drafting;

import std;
import cli11;
import planar.cliapp.args;
import planar.cmd.planar.context;
import planar.cmd.planar.editflow;
import planar.cmd.planar.handler;

namespace planar::cmd::handlers {

/// @brief Handle `planar plan edit <plan-id> [--no-pull] [--json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure to report.
export auto plan_edit(context& ctx, const cliapp::parsed_args& args) -> handler_result;
/// @brief Handle `planar plan view <plan-id>`.
///
/// The ANCHOR plan renders to the feature's `README.md`; every other plan
/// renders to `plans/<slug>.md`. Oracle-confirmed on a three-level chain.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure to report.
export auto plan_view(context& ctx, const cliapp::parsed_args& args) -> handler_result;
/// @brief Handle `planar plan diff <plan-id>`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure to report.
export auto plan_diff(context& ctx, const cliapp::parsed_args& args) -> handler_result;
/// @brief Handle `planar plan review <plan-id> [--approve]
/// [--request-changes] [--json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure to report.
export auto plan_review(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar task edit <task-id> [--no-pull] [--json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure to report.
export auto task_edit(context& ctx, const cliapp::parsed_args& args) -> handler_result;
/// @brief Handle `planar task view <task-id>`.
///
/// Renders under `tasks/<dir>/`, where `<dir>` is the task's repo-scope
/// slug, else its first `touches` repo, else `cross`. Oracle-confirmed per
/// arm, precedence case included.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure to report.
export auto task_view(context& ctx, const cliapp::parsed_args& args) -> handler_result;
/// @brief Handle `planar task diff <task-id>`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure to report.
export auto task_diff(context& ctx, const cliapp::parsed_args& args) -> handler_result;
/// @brief Handle `planar task review <task-id> [--approve]
/// [--request-changes] [--json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure to report.
export auto task_review(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar question edit <question-id> [--no-pull] [--json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure to report.
export auto question_edit(context& ctx, const cliapp::parsed_args& args) -> handler_result;
/// @brief Handle `planar question view <question-id>`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure to report.
export auto question_view(context& ctx, const cliapp::parsed_args& args) -> handler_result;
/// @brief Handle `planar question diff <question-id>`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure to report.
export auto question_diff(context& ctx, const cliapp::parsed_args& args) -> handler_result;
/// @brief Handle `planar question review <question-id> [--approve]
/// [--request-changes] [--json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure to report.
export auto question_review(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar decision edit <decision-id> [--no-pull] [--json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure to report.
export auto decision_edit(context& ctx, const cliapp::parsed_args& args) -> handler_result;
/// @brief Handle `planar decision view <decision-id>`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure to report.
export auto decision_view(context& ctx, const cliapp::parsed_args& args) -> handler_result;
/// @brief Handle `planar decision diff <decision-id>`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure to report.
export auto decision_diff(context& ctx, const cliapp::parsed_args& args) -> handler_result;
/// @brief Handle `planar decision review <decision-id> [--approve]
/// [--request-changes] [--json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure to report.
export auto decision_review(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar scenario edit <scenario-id> [--no-pull] [--json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure to report.
export auto scenario_edit(context& ctx, const cliapp::parsed_args& args) -> handler_result;
/// @brief Handle `planar scenario view <scenario-id>`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure to report.
export auto scenario_view(context& ctx, const cliapp::parsed_args& args) -> handler_result;
/// @brief Handle `planar scenario diff <scenario-id>`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure to report.
export auto scenario_diff(context& ctx, const cliapp::parsed_args& args) -> handler_result;
/// @brief Handle `planar scenario review <scenario-id> [--approve]
/// [--request-changes] [--json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure to report.
export auto scenario_review(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar artifact edit <artifact-id> [--no-pull] [--json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure to report.
export auto artifact_edit(context& ctx, const cliapp::parsed_args& args) -> handler_result;
/// @brief Handle `planar artifact view <artifact-id>`.
///
/// Writes under `<feature>/artifacts/`, where `artifact diff` reads from
/// the feature ROOT. That divergence is the oracle's and is preserved; see
/// `planar.cmd.planar.editflow`'s header.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure to report.
export auto artifact_view(context& ctx, const cliapp::parsed_args& args) -> handler_result;
/// @brief Handle `planar artifact diff <artifact-id>`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure to report.
export auto artifact_diff(context& ctx, const cliapp::parsed_args& args) -> handler_result;
/// @brief Handle `planar artifact review <artifact-id> [--approve]
/// [--request-changes] [--json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure to report.
export auto artifact_review(context& ctx, const cliapp::parsed_args& args) -> handler_result;

} // namespace planar::cmd::handlers
