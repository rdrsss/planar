/// @file dispatch.cpp
/// @brief CLI adapters for the immutable routing dispatch boundary.
module planar.cmd.planar_agent.handlers.dispatch;
import std;
import planar.cliapp.args;
import planar.engine.routing;
import planar.json_text;
import planar.cmd.planar_agent.context;
import planar.cmd.planar_agent.exit;
import planar.cmd.planar_agent.handler;

namespace planar::cmd::agent::handlers {
namespace rt = engine::routing;
namespace {
auto text(const cliapp::parsed_args& a, std::string_view name) -> std::string {
  return cliapp::flag_string(a, name).value_or(std::string{});
}
auto integer(const cliapp::parsed_args& a, std::string_view name) -> std::optional<std::int64_t> {
  return cliapp::flag_int(a, name);
}
auto opt_text(const cliapp::parsed_args& a, std::string_view name) -> std::optional<std::string> {
  auto value = cliapp::flag_string(a, name);
  return value && !value->empty() ? value : std::nullopt;
}
auto choice(std::string_view raw, std::initializer_list<std::string_view> values, std::string_view flag, bool detailed)
    -> std::expected<void, domain_error> {
  if (std::ranges::find(values, raw) != values.end())
    return {};
  if (!detailed)
    return std::unexpected(error_from_body(domain_error_kind::invalid_input, std::format("invalid {} '{}'", flag, raw)));
  std::string expected;
  for (auto value : values) {
    if (!expected.empty())
      expected += ", ";
    expected += value;
  }
  return std::unexpected(
      error_from_body(domain_error_kind::invalid_input, std::format("invalid {} '{}': expected one of {}", flag, raw, expected)));
}
auto build(const cliapp::parsed_args& a, bool preview) -> std::expected<rt::binding, domain_error> {
  auto project = integer(a, "--project"), candidate = integer(a, "--candidate");
  if (!candidate || (preview && !project))
    return std::unexpected(error_from_body(domain_error_kind::invalid_input, "invalid integer routing binding"));
  rt::binding b{.task_id           = integer(a, "--task"),
                .work_item         = preview ? text(a, "--work-item") : std::string{},
                .validation_policy = text(a, "--validation-policy"),
                .routing_policy    = text(a, "--routing-policy"),
                .profile_rule      = preview ? text(a, "--profile-rule") : std::string{},
                .vendor            = text(a, "--vendor"),
                .role              = text(a, "--role"),
                .tier              = text(a, "--tier"),
                .work_type         = text(a, "--work-type"),
                .complexity        = text(a, "--complexity"),
                .project_id        = project.value_or(0),
                .candidate_id      = *candidate,
                .packet_digest     = text(a, "--packet-digest"),
                .profile_digest    = text(a, "--profile-digest"),
                .policy_digest     = text(a, "--policy-digest"),
                .capability_digest = text(a, "--capability-digest"),
                .host              = preview ? text(a, "--host") : std::string{},
                .assignment_class  = preview ? text(a, "--class") : std::string{},
                .evidence_state    = preview ? text(a, "--evidence-state") : std::string{},
                .experiment_id     = preview ? integer(a, "--experiment") : std::nullopt,
                .claim             = opt_text(a, "--claim"),
                .claim_status      = opt_text(a, "--claim-status")};
  return b;
}
auto failure(std::string_view verb, rt::error e, const rt::stale_reason* why = nullptr) -> handler_result {
  auto body = std::format("{}: {}", verb, rt::error_name(e));
  if (why)
    body += std::format(" ({})", rt::stale_name(*why));
  return std::unexpected(error_from_body(domain_error_kind::generic_failure, std::move(body)));
}
} // namespace
auto dispatch_preview(context& ctx, const cliapp::parsed_args& a) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn)
    return std::unexpected(conn.error());
  auto b = build(a, true);
  if (!b)
    return std::unexpected(b.error());
  for (auto const& check :
       {choice(b->tier, {"small", "medium", "large"}, "--tier", true),
        choice(b->work_type, {"schema", "engine", "architectural", "cli", "feature", "mechanical"}, "--work-type", true),
        choice(b->complexity, {"bounded", "standard", "high-risk"}, "--complexity", true),
        choice(b->assignment_class, {"fallback", "default", "override", "declared_experiment"}, "--class", true),
        choice(b->evidence_state, {"evidential", "observational"}, "--evidence-state", true)})
    if (!check)
      return std::unexpected(check.error());
  auto result = rt::preview(**conn, *b, text(a, "--expires-at"));
  if (!result)
    return failure("dispatch preview", result.error());
  if (cliapp::flag_bool(a, "--json"))
    ctx.out() << std::format("{{\"ok\":true,\"preview_id\":{},\"preview_token\":{},\"expires_at\":{},\"evidence_state\":{},"
                             "\"contract\":\"routing-dispatch-v1\"}}\n",
                             result->id, json_text::json_string(result->token), json_text::json_string(text(a, "--expires-at")),
                             json_text::json_string(b->evidence_state));
  else
    ctx.out() << std::format("preview: {}\n  expires:  {}\n  evidence: {}\n", result->token, text(a, "--expires-at"),
                             b->evidence_state);
  return {};
}
auto dispatch_confirm(context& ctx, const cliapp::parsed_args& a) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn)
    return std::unexpected(conn.error());
  auto b = build(a, false);
  if (!b)
    return std::unexpected(b.error());
  for (auto const& check :
       {choice(b->tier, {"small", "medium", "large"}, "--tier", false),
        choice(b->work_type, {"schema", "engine", "architectural", "cli", "feature", "mechanical"}, "--work-type", false),
        choice(b->complexity, {"bounded", "standard", "high_risk"}, "--complexity", false)})
    if (!check)
      return std::unexpected(check.error());
  rt::stale_reason why{};
  auto             result = rt::confirm(**conn, text(a, "--token"), text(a, "--dispatch-key"), *b, text(a, "--now"),
                                        text(a, "--reviewer").empty() ? "required" : text(a, "--reviewer"),
                                        text(a, "--decision").empty() ? "confirmed" : text(a, "--decision"), &why);
  if (!result) {
    if (result.error() == rt::error::stale_preview) {
      if (cliapp::flag_bool(a, "--json"))
        ctx.out() << std::format("{{\"ok\":false,\"error\":\"stale_preview\",\"reason\":{}}}\n",
                                 json_text::json_string(rt::stale_name(why)));
      else
        ctx.out() << std::format("stale_preview: {}\n", rt::stale_name(why));
      return std::unexpected(
          error_from_body(domain_error_kind::generic_failure, std::format("stale_preview: {}", rt::stale_name(why))));
    }
    if (result.error() == rt::error::unknown_preview)
      return std::unexpected(error_from_body(domain_error_kind::not_found, "unknown preview token"));
    if (result.error() == rt::error::invalid_value)
      return std::unexpected(
          error_from_body(domain_error_kind::invalid_input, "value rejected: identifiers must not contain control characters"));
    return failure("confirm", result.error());
  }
  if (cliapp::flag_bool(a, "--json"))
    ctx.out() << std::format("{{\"ok\":true,\"dispatch_id\":{},\"contract\":\"routing-dispatch-v1\"}}\n", result->dispatch_id);
  else
    ctx.out() << std::format("dispatch:{}\n", result->dispatch_id);
  return {};
}
} // namespace planar::cmd::agent::handlers
