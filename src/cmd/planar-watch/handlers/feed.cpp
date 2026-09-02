/// @file feed.cpp
/// @brief Implementation of the bounded, read-only `planar-watch feed` view.
module planar.cmd.planar_watch.handlers.feed;
import std;
import planar.cliapp.args;
import planar.engine.runtime.agentactivity;
import planar.engine.runtime.agentrender;
import planar.json_text;
import planar.cmd.planar_watch.context;
import planar.cmd.planar_watch.exit;
namespace planar::cmd::watch::handlers {
namespace aa = engine::runtime::agentactivity;
namespace ar = engine::runtime::agentrender;
namespace {
auto failure(aa::agent_error e) -> domain_error {
  return error_from_body(domain_error_kind::generic_failure, std::format("feed: {}", aa::error_name(e)));
}
} // namespace
auto feed(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  if (cliapp::flag_bool(args, "--follow"))
    return std::unexpected(
        error_from_body(domain_error_kind::not_implemented, "feed: --follow is not implemented in this build"));
  auto conn = ctx.ensure_db();
  if (!conn)
    return std::unexpected(conn.error());
  auto const tail = cliapp::flag_int(args, "--tail");
  if (tail.has_value() && *tail <= 0)
    return std::unexpected(error_from_body(domain_error_kind::invalid_input, "feed: --tail must be a positive integer"));
  auto const limit  = tail.value_or(cliapp::flag_int(args, "--limit").value_or(100));
  auto       rows   = aa::list_actions(**conn, 4096);
  auto       claims = aa::list_claims(**conn, aa::claim_status_filter::all);
  if (!rows)
    return std::unexpected(failure(rows.error()));
  if (!claims)
    return std::unexpected(failure(claims.error()));
  bool const json = cliapp::flag_bool(args, "--json");
  struct event {
    std::string at, kind, body;
  };
  std::vector<event> events;
  auto               include = [&](std::string_view at, std::string_view vendor, std::optional<std::int64_t> task) {
    if (auto filter = cliapp::flag_string(args, "--vendor"); filter.has_value() && vendor != *filter)
      return false;
    if (auto filter = cliapp::flag_int(args, "--task"); filter.has_value() && task != *filter)
      return false;
    if (auto since = cliapp::flag_string(args, "--since"); since.has_value() && at < *since)
      return false;
    return true;
  };
  for (auto const& row : *rows) {
    if (auto plan = cliapp::flag_int(args, "--plan");
        plan.has_value() &&
        (!row.entity || !row.entity_id || !aa::action_belongs_to_plan(**conn, *row.entity, *row.entity_id, *plan)))
      continue;
    auto task = row.entity.has_value() && aa::to_text(*row.entity) == "task" ? row.entity_id : std::nullopt;
    auto add  = [&](std::string_view at, std::string_view kind) {
      if (include(at, row.vendor, task)) {
        std::string body;
        ar::append_action(body, row);
        events.push_back({std::string{at}, std::string{kind}, std::move(body)});
      }
    };
    add(row.started_at, "action_started");
    if (row.ended_at)
      add(*row.ended_at, "action_ended");
  }
  for (auto const& claim : *claims) {
    if (auto plan = cliapp::flag_int(args, "--plan");
        plan.has_value() && !aa::claim_belongs_to_plan(**conn, claim.kind, claim.entity_id, *plan))
      continue;
    auto task = aa::to_text(claim.kind) == "task" ? std::optional{claim.entity_id} : std::nullopt;
    auto add  = [&](std::string_view at, std::string_view kind) {
      if (include(at, claim.vendor, task)) {
        std::string body;
        ar::append_claim_view(body, claim, {});
        events.push_back({std::string{at}, std::string{kind}, std::move(body)});
      }
    };
    add(claim.claimed_at, "claim_acquired");
    if (claim.last_heartbeat_at != claim.claimed_at)
      add(claim.last_heartbeat_at, "heartbeat");
    if (claim.released_at)
      add(*claim.released_at, aa::to_text(claim.status));
  }
  std::ranges::sort(events, {}, &event::at);
  if (events.size() > static_cast<std::size_t>(limit))
    events.erase(events.begin(), events.end() - limit);
  std::string out;
  for (auto const& e : events) {
    if (json) {
      out.append("{\"event\":");
      json_text::append_json_string(out, e.kind);
      out.append(",\"at\":");
      json_text::append_json_string(out, e.at);
      out.append(e.kind.starts_with("action") ? ",\"action\":" : ",\"claim\":");
      out.append(e.body);
      out.append("}\n");
    } else
      out.append(std::format("  {}  {}\n", e.at, e.kind));
  }
  ctx.out() << out;
  return {};
}
} // namespace planar::cmd::watch::handlers
