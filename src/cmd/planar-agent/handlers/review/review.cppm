/// @file review.cppm
/// @brief `planar.cmd.planar_agent.handlers.review` — `planar-agent review`, the verb that records a
/// reviewer's verdict on a claim (plan 1142, task mo-review-record).
///
/// A review is evidence that someone other than the coder looked at the work. The verb is refused
/// when the caller's session is the reviewed claim's session, and a refusal writes nothing. A
/// missing or invalid `--verdict` (and a missing `--summary`) is refused at exit 2 before the
/// database is opened, so no review row is ever written with a NULL verdict. The engine side is
/// `agentatomic::record_review`; this handler only validates flags, maps the engine's refusals to
/// exit codes and renders the result.
module;

export module planar.cmd.planar_agent.handlers.review;

import cli11;
import planar.cliapp.args;
import planar.cmd.planar_agent.context;
import planar.cmd.planar_agent.handler;

namespace planar::cmd::agent::handlers {

/// @brief Handle `planar-agent review --claim <token> --verdict <v> --summary <text> [flags]`.
///
/// Exit 2: missing or invalid `--verdict`, missing `--summary`, or neither `--reviewer-claim` nor
/// `--vendor-session`. Exit 1: an unknown claim token, or a review from the reviewed claim's own
/// session.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure to report.
export auto review(context& ctx, const cliapp::parsed_args& args) -> handler_result;

} // namespace planar::cmd::agent::handlers
