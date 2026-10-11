/// @file review.cpp
/// @brief Implementation of `planar.cmd.planar_agent.handlers.review`.

module planar.cmd.planar_agent.handlers.review;

import std;
import cli11;
import planar.cliapp.args;
import planar.engine.runtime.agentactivity;
import planar.engine.runtime.agentatomic;
import planar.engine.runtime.agentrender;
import planar.cmd.planar_agent.context;
import planar.cmd.planar_agent.exit;
import planar.cmd.planar_agent.handler;
import planar.cmd.planar_agent.handlers.support;

namespace planar::cmd::agent::handlers {

namespace aa     = engine::runtime::agentactivity;
namespace atomic = engine::runtime::agentatomic;
namespace render = engine::runtime::agentrender;

namespace {

/// @brief A refusal that exits 1 and carries a CamelCase tag, like `TaskNotReady`.
/// @param tag The tag.
/// @param body The message body.
/// @return The domain error.
auto refusal(std::string_view tag, std::string body) -> domain_error {
  auto err = error_from_body(domain_error_kind::generic_failure, std::move(body));
  err.tag  = std::string{tag};
  return err;
}

} // namespace

auto review(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  // Flag validation first: a refusal here opens no database and writes nothing.
  auto const verdict_text = cliapp::flag_string(args, "--verdict");
  if (!verdict_text.has_value()) {
    return std::unexpected(invalid_input_error("review: --verdict is required: approve or request-changes"));
  }
  auto const verdict = aa::review_verdict_from_text(*verdict_text);
  if (!verdict.has_value()) {
    return std::unexpected(
        invalid_input_error(std::format("review: invalid --verdict '{}': expected approve or request-changes", *verdict_text)));
  }
  auto const summary = cliapp::flag_string(args, "--summary");
  if (!summary.has_value() || summary->empty()) {
    return std::unexpected(invalid_input_error("review: --summary is required: say what the review found"));
  }
  auto const reviewer_claim = cliapp::flag_string(args, "--reviewer-claim");
  auto const session        = cliapp::flag_string(args, "--vendor-session");
  if (!reviewer_claim.has_value() && !session.has_value()) {
    return std::unexpected(
        invalid_input_error("review: say who you are: pass --reviewer-claim <token> or --vendor-session <id>"));
  }

  auto conn = ctx.db().ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  auto const reviewed_token = cliapp::flag_string(args, "--claim").value_or(std::string{});
  auto const vendor         = cliapp::flag_string(args, "--vendor").value_or(std::string{"planar-agent"});

  auto const result = atomic::record_review(
      **conn,
      atomic::review_args{
          .reviewed_claim_token    = reviewed_token,
          .reviewer_claim_token    = reviewer_claim.has_value() ? std::optional<std::string_view>{*reviewer_claim} : std::nullopt,
          .reviewer_vendor_session = session.has_value() ? std::optional<std::string_view>{*session} : std::nullopt,
          .vendor                  = vendor,
          .verdict                 = *verdict,
          .summary                 = *summary,
      });
  if (!result) {
    switch (result.error()) {
    case aa::agent_error::claim_not_found:
      return std::unexpected(refusal("ClaimNotFound", std::format("review: no claim with token {}", reviewed_token)));
    case aa::agent_error::reviewer_claim_not_found:
      return std::unexpected(
          refusal("ReviewerClaimNotFound", std::format("review: no reviewer claim with token {}", reviewer_claim.value_or(""))));
    case aa::agent_error::reviewer_unidentified:
      return std::unexpected(
          invalid_input_error("review: say who you are: pass --reviewer-claim <token> or --vendor-session <id>"));
    case aa::agent_error::reviewer_not_independent: {
      std::string body = "review: refused: the reviewer's session is the reviewed claim's session; run the review under a "
                         "different claim or session than the coder's";
      auto const  held = aa::get_claim_by_token(**conn, reviewed_token);
      if (held.has_value() && !held->vendor_session_id.has_value()) {
        body += "\n  the reviewed claim has no vendor session, so it cannot be compared: pass --reviewer-claim with a claim "
                "whose vendor session is set";
      }
      return std::unexpected(refusal("ReviewerNotIndependent", std::move(body)));
    }
    default:
      return std::unexpected(verb_error("review", result.error()));
    }
  }

  ctx.out() << (cliapp::flag_bool(args, "--json") ? render::review_json(*result) : render::review_text(*result));
  return {};
}

} // namespace planar::cmd::agent::handlers
