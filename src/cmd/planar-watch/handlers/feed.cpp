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
  auto const limit = tail.value_or(cliapp::flag_int(args, "--limit").value_or(100));
  auto       rows  = aa::list_actions(**conn, limit);
  if (!rows)
    return std::unexpected(failure(rows.error()));
  std::reverse(rows->begin(), rows->end());
  bool const  json = cliapp::flag_bool(args, "--json");
  std::string out;
  for (auto const& row : *rows) {
    if (auto vendor = cliapp::flag_string(args, "--vendor"); vendor.has_value() && row.vendor != *vendor)
      continue;
    if (auto task = cliapp::flag_int(args, "--task");
        task.has_value() &&
        (!row.entity_id.has_value() || *row.entity_id != *task || !row.entity.has_value() || aa::to_text(*row.entity) != "task"))
      continue;
    auto const at = row.ended_at.value_or(row.started_at);
    if (auto since = cliapp::flag_string(args, "--since"); since.has_value() && at < *since)
      continue;
    if (json) {
      out.append("{\"event\":");
      json_text::append_json_string(out, row.ended_at.has_value() ? "action_ended" : "action_started");
      out.append(",\"at\":");
      json_text::append_json_string(out, at);
      out.append(",\"action\":");
      ar::append_action(out, row);
      out.append("}\n");
    } else
      out.append(
          std::format("  {}  {}  vendor:{}\n", at, row.ended_at.has_value() ? "action_ended" : "action_started", row.vendor));
  }
  ctx.out() << out;
  return {};
}
} // namespace planar::cmd::watch::handlers
