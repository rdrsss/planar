/// @file action.cppm
/// @brief `planar.cmd.planar_agent.handlers.action` — the nested action
/// lifecycle inside a claim: `action start` and `action end` (plan 996,
/// task 6038).
///
/// Port target: zig/src/cmd/planar-agent/handlers/action/{start,end}.zig.
///
/// An action is a sub-interval of a claim — one tool call, one sub-agent
/// dispatch — and `planar-watch tree` renders the nesting. `action start`
/// attaches the new row under the claim's newest still-open action, so the
/// hierarchy builds itself without the caller tracking parent ids.
///
/// ## Two validation asymmetries that look like bugs and are not
///
/// - `--kind` is STRICT (an unknown kind is refused), while `pull --role`
///   silently degrades to `coder`. The kinds are a closed CHECK set on
///   `agent_actions.action_kind`; roles are a free-text label.
/// - `--entity` here accepts a WIDER set than `claim --entity` does —
///   `question`, `test_scenario`, `artifact` and `decision` on top of the
///   three claimable kinds — because `agent_actions.entity_kind`'s CHECK
///   is wider than `agent_work_claims.entity_kind`'s. It is therefore
///   parsed HERE rather than through `args`' `parse_entity_ref`, and its
///   refusal messages are different strings. Routing both through one
///   parser would either narrow this verb or widen `claim`.
///
/// `--outcome` on `action end` is declared as a plain string with a
/// hand-rolled validator rather than a `choice`, so its legal values
/// appear only in the flag's prose. That is the oracle's declaration and
/// changing it would change the `--help` page.
module;

export module planar.cmd.planar_agent.handlers.action;

import std;
import planar.cli;
import planar.cmd.planar_agent.context;
import planar.cmd.planar_agent.handler;

namespace planar::cmd::agent::handlers {

/// @brief Handle `planar-agent action start --claim <token> --kind <k> [flags]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure to report.
export auto action_start(context& ctx, const cli::match_result& args) -> handler_result;

/// @brief Handle `planar-agent action end --action <id> [--outcome <o>]`.
///
/// Closing an already-closed or absent action is a silent no-op that still
/// exits 0 — the UPDATE simply matches nothing. That is the Zig SQL's
/// behavior despite its own doc comment claiming otherwise.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure to report.
export auto action_end(context& ctx, const cli::match_result& args) -> handler_result;

} // namespace planar::cmd::agent::handlers
