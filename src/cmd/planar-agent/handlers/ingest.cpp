/// @file ingest.cpp
/// @brief Vendor hook ingestion for `planar-agent ingest`.
///
/// The vendor adapter boundary is deliberately pure: this handler first
/// validates and normalizes the JSON envelope, then performs the matching
/// session/action writes inside one immediate transaction.  A malformed or
/// unknown event never opens a transaction, and an action write failure rolls
/// back the session it may have auto-created.
module;

#include <glaze/json/generic.hpp>

module planar.cmd.planar_agent.handlers.context;

import std;
import cli11;
import planar.cliapp.args;
import planar.db;
import planar.engine.runtime.agentactivity;
import planar.engine.runtime.session;
import planar.cmd.planar_agent.context;
import planar.cmd.planar_agent.exit;
import planar.cmd.planar_agent.handler;
import planar.cmd.planar_agent.handlers.support;

namespace planar::cmd::agent::handlers {
namespace aa = engine::runtime::agentactivity;
namespace se = engine::runtime::session;

namespace {

struct event {
  std::string                type;
  std::string                session_id;
  std::optional<std::string> model;
  std::optional<std::string> role;
  std::optional<std::string> summary;
  aa::action_kind            kind                              = aa::action_kind::other;
  aa::outcome                result                            = aa::outcome::ok;
  enum class form { session_start, session_end, atomic } form_ = form::atomic;
};

auto field(const glz::generic& object, std::string_view name) -> std::optional<std::string> {
  if (!object.is_object() || !object.contains(name) || !object.at(name).is_string())
    return std::nullopt;
  return object.at(name).get<std::string>();
}

auto parse(std::string_view vendor, std::string_view payload) -> std::expected<event, domain_error> {
  auto raw = glz::read_json<glz::generic>(payload);
  if (!raw || !raw->is_object())
    return std::unexpected(invalid_input_error(
        std::format("malformed event payload (vendor={}): JSON parse failed or required envelope field missing", vendor)));
  auto const claude  = vendor == "claude";
  auto const copilot = vendor == "copilot";
  if (!claude && !copilot)
    return std::unexpected(invalid_input_error(std::format("unknown vendor '{}'; supported: claude|codex|copilot", vendor)));
  if (vendor == "codex")
    return std::unexpected(invalid_input_error("vendor 'codex' adapter not wired (claude + copilot are; codex is reserved)"));
  auto type = field(*raw, claude ? "event_type" : "event");
  auto sid  = field(*raw, "session_id");
  if (!type || !sid)
    return std::unexpected(invalid_input_error(
        std::format("malformed event payload (vendor={}): JSON parse failed or required envelope field missing", vendor)));
  event      out{.type       = *type,
                 .session_id = *sid,
                 .model      = field(*raw, "model"),
                 .role       = field(*raw, "role"),
                 .summary    = field(*raw, "summary")};
  auto const starts = claude ? "session_start" : "session.started";
  auto const ends   = claude ? "session_end" : "session.completed";
  if (out.type == starts) {
    out.form_ = event::form::session_start;
    return out;
  }
  if (out.type == ends) {
    out.form_ = event::form::session_end;
    return out;
  }
  if (out.type == (claude ? "user_message" : "turn.user")) {
    out.kind = aa::action_kind::user_message;
  } else if (out.type == (claude ? "assistant_message" : "turn.assistant")) {
    out.kind = aa::action_kind::assistant_message;
  } else if (out.type == (claude ? "tool_call" : "tool.invocation")) {
    out.kind = aa::action_kind::tool_call;
  } else {
    auto const supported = claude ? "session_start | session_end | user_message | assistant_message | tool_call"
                                  : "session.started | session.completed | turn.user | turn.assistant | tool.invocation";
    return std::unexpected(
        invalid_input_error(std::format("unknown event_type in payload (vendor={}); supported: {}", vendor, supported)));
  }
  auto outcome = field(*raw, copilot ? "status" : "outcome");
  if (copilot && !outcome)
    outcome = field(*raw, "outcome");
  if (outcome) {
    auto parsed = aa::outcome_from_text(*outcome);
    if (!parsed)
      return std::unexpected(invalid_input_error(
          std::format("malformed event payload (vendor={}): JSON parse failed or required envelope field missing", vendor)));
    out.result = *parsed;
  }
  return out;
}

auto active_or_open(db::connection& conn, std::string_view vendor, const event& e, std::int64_t& sessions)
    -> std::expected<std::int64_t, domain_error> {
  auto existing = se::active_for_vendor(conn, vendor, e.session_id);
  if (!existing)
    return std::unexpected(verb_error("ingest dispatch", aa::agent_error::query_failed));
  if (existing->has_value())
    return (*existing)->id;
  auto created = se::start_session(conn, {.vendor            = vendor,
                                          .vendor_session_id = e.session_id,
                                          .task_id           = std::nullopt,
                                          .model = e.model ? std::optional<std::string_view>{*e.model} : std::nullopt});
  if (!created)
    return std::unexpected(verb_error("ingest dispatch", aa::agent_error::query_failed));
  ++sessions;
  return created->id;
}

auto read_payload(const context& ctx, std::string_view source) -> std::expected<std::string, domain_error> {
  if (!source.starts_with('@'))
    return std::unexpected(invalid_input_error(std::format("--event must be '@<file>' or '@-' (got '{}')", source)));
  if (source == "@-")
    return std::string{std::istreambuf_iterator<char>{std::cin}, {}};
  auto const    path = ctx.cwd() / std::filesystem::path{source.substr(1)};
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return std::unexpected(
        error_from_body(domain_error_kind::generic_failure, std::format("read event ({}): FileNotFound", source.substr(1))));
  std::string value{std::istreambuf_iterator<char>{in}, {}};
  if (value.size() > (1U << 20))
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "read event: StreamTooLong"));
  return value;
}

} // namespace

auto ingest(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto const vendor  = cliapp::flag_string(args, "--vendor").value_or(std::string{});
  auto       payload = read_payload(ctx, cliapp::flag_string(args, "--event").value_or(std::string{}));
  if (!payload)
    return std::unexpected(payload.error());
  auto parsed = parse(vendor, *payload);
  if (!parsed)
    return std::unexpected(parsed.error());
  auto conn = ctx.ensure_db();
  if (!conn)
    return std::unexpected(conn.error());
  auto tx = (*conn)->begin_transaction(db::lock_mode::immediate);
  if (!tx)
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "BEGIN IMMEDIATE: QueryFailed"));
  std::int64_t sessions = 0, actions = 0;
  if (parsed->form_ == event::form::session_start) {
    auto active = se::active_for_vendor(**conn, vendor, parsed->session_id);
    if (!active)
      return std::unexpected(verb_error("ingest dispatch", aa::agent_error::query_failed));
    std::int64_t id = 0;
    if (active->has_value())
      id = (*active)->id;
    else {
      auto started =
          se::start_session(**conn, {.vendor            = vendor,
                                     .vendor_session_id = parsed->session_id,
                                     .task_id           = std::nullopt,
                                     .model = parsed->model ? std::optional<std::string_view>{*parsed->model} : std::nullopt});
      if (!started)
        return std::unexpected(verb_error("ingest dispatch", aa::agent_error::query_failed));
      id = started->id;
      ++sessions;
    }
    if (parsed->summary && !se::append_entry(**conn, id, "note", *parsed->summary))
      return std::unexpected(verb_error("ingest dispatch", aa::agent_error::query_failed));
  } else if (parsed->form_ == event::form::session_end) {
    auto active = se::active_for_vendor(**conn, vendor, parsed->session_id);
    if (!active)
      return std::unexpected(verb_error("ingest dispatch", aa::agent_error::query_failed));
    if (active->has_value() &&
        !se::end_session(**conn, (*active)->id,
                         parsed->summary ? std::optional<std::string_view>{*parsed->summary} : std::nullopt))
      return std::unexpected(verb_error("ingest dispatch", aa::agent_error::query_failed));
  } else {
    auto sid = active_or_open(**conn, vendor, *parsed, sessions);
    if (!sid)
      return std::unexpected(sid.error());
    auto action =
        aa::start_action(**conn, {.session_id       = *sid,
                                  .session_entry_id = std::nullopt,
                                  .parent_action_id = std::nullopt,
                                  .claim_id         = std::nullopt,
                                  .kind             = parsed->kind,
                                  .entity           = std::nullopt,
                                  .entity_id        = std::nullopt,
                                  .vendor           = vendor,
                                  .vendor_role = parsed->role ? std::optional<std::string_view>{*parsed->role} : std::nullopt,
                                  .model       = parsed->model ? std::optional<std::string_view>{*parsed->model} : std::nullopt,
                                  .loc         = {},
                                  .metadata    = std::nullopt});
    if (!action || !aa::end_action(**conn, *action, parsed->result,
                                   parsed->summary ? std::optional<std::string_view>{*parsed->summary} : std::nullopt))
      return std::unexpected(verb_error("ingest dispatch", aa::agent_error::query_failed));
    ++actions;
  }
  if (!tx->commit())
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "COMMIT: QueryFailed"));
  if (cliapp::flag_bool(args, "--json"))
    ctx.out() << std::format(
        "{{\"ok\":true,\"sessions_created\":{},\"claims_created\":0,\"actions_created\":{},\"events_processed\":1}}\n", sessions,
        actions);
  else
    ctx.out() << std::format("ingest: vendor={} sessions_created={} actions_created={} events_processed=1\n", vendor, sessions,
                             actions);
  return {};
}

} // namespace planar::cmd::agent::handlers
