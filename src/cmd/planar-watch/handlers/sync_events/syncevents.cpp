/// @file syncevents.cpp
/// @brief Implementation of `planar.cmd.planar_watch.handlers.syncevents`.

module planar.cmd.planar_watch.handlers.syncevents;

import std;
import cli11;
import planar.cliapp.args;
import planar.db;
import planar.json_text;
import planar.cmd.planar_watch.context;
import planar.cmd.planar_watch.exit;
import planar.cmd.planar_watch.handler;
import planar.cmd.planar_watch.handlers.format;

namespace planar::cmd::watch::handlers {

using json_text::append_json_string;

namespace {

/// @brief One `sync_events` row.
struct sync_event_row {
  std::int64_t                id = 0;
  std::optional<std::int64_t> link_id;
  std::string                 scope;
  std::string                 direction;
  std::string                 outcome;
  std::optional<std::string>  fields_changed;
  std::optional<std::string>  detail;
  std::optional<std::string>  context_json;
  std::string                 at;
};

/// @brief The `kind:id` decomposition `--entity` accepts.
struct entity_ref {
  std::string  kind;
  std::int64_t id = 0;
};

/// @brief `select ... from sync_events order by at desc, id desc limit N`.
///
/// The limit caps the fetch, NOT the post-filter result set — filters below
/// run over this already-limited slice, matching the oracle. A row past
/// the limit can never be recovered by `--outcome` / `--plan` / etc.
/// @param conn The connection.
/// @param limit The row cap.
/// @return The rows, or the query failure.
auto list_sync_events(db::connection& conn, std::int64_t limit) -> std::expected<std::vector<sync_event_row>, db::db_error> {
  auto stmt = conn.prepare(std::format("select id, link_id, scope, direction, outcome, fields_changed, detail, context_json, at "
                                       "from sync_events order by at desc, id desc limit {}",
                                       limit));
  if (!stmt) {
    return std::unexpected(stmt.error());
  }
  std::vector<sync_event_row> out;
  while (true) {
    auto const step = stmt->step();
    if (!step) {
      return std::unexpected(step.error());
    }
    if (*step == db::step_result::done) {
      break;
    }
    out.push_back(sync_event_row{.id             = stmt->column_int64(0),
                                 .link_id        = stmt->is_null(1) ? std::nullopt : std::optional{stmt->column_int64(1)},
                                 .scope          = stmt->column_text(2),
                                 .direction      = stmt->column_text(3),
                                 .outcome        = stmt->column_text(4),
                                 .fields_changed = stmt->is_null(5) ? std::nullopt : std::optional{stmt->column_text(5)},
                                 .detail         = stmt->is_null(6) ? std::nullopt : std::optional{stmt->column_text(6)},
                                 .context_json   = stmt->is_null(7) ? std::nullopt : std::optional{stmt->column_text(7)},
                                 .at             = stmt->column_text(8)});
  }
  return out;
}

/// @brief Does `external_links` row `link_id` belong to `plan_id`?
/// @param conn The connection.
/// @param link_id The link.
/// @param plan_id The plan.
/// @return True on a match; false on absence or query failure.
auto link_belongs_to_plan(db::connection& conn, std::int64_t link_id, std::int64_t plan_id) -> bool {
  auto stmt = conn.prepare("select 1 from external_links where id = ? and entity_kind = 'plan' and entity_id = ?");
  if (!stmt) {
    return false;
  }
  if (!stmt->bind_int64(1, link_id) || !stmt->bind_int64(2, plan_id)) {
    return false;
  }
  auto const step = stmt->step();
  return step.has_value() && *step == db::step_result::row;
}

/// @brief Does `external_links` row `link_id` join to an `external_systems`
/// row with the given `slug`?
/// @param conn The connection.
/// @param link_id The link.
/// @param slug The system slug.
/// @return True on a match; false on absence or query failure.
auto link_belongs_to_system(db::connection& conn, std::int64_t link_id, std::string_view slug) -> bool {
  auto stmt = conn.prepare("select 1 from external_links el join external_systems es on es.id = el.system_id "
                           "where el.id = ? and es.slug = ?");
  if (!stmt) {
    return false;
  }
  if (!stmt->bind_int64(1, link_id) || !stmt->bind_text(2, slug)) {
    return false;
  }
  auto const step = stmt->step();
  return step.has_value() && *step == db::step_result::row;
}

/// @brief Does `external_links` row `link_id` have `entity_kind = ek` and,
/// when supplied, `entity_id = eid`?
/// @param conn The connection.
/// @param link_id The link.
/// @param entity_kind The entity kind text.
/// @param entity_id Optional entity id.
/// @return True on a match; false on absence or query failure.
auto link_has_entity(db::connection& conn, std::int64_t link_id, std::string_view entity_kind,
                     std::optional<std::int64_t> entity_id) -> bool {
  if (entity_id.has_value()) {
    auto stmt = conn.prepare("select 1 from external_links where id = ? and entity_kind = ? and entity_id = ?");
    if (!stmt) {
      return false;
    }
    if (!stmt->bind_int64(1, link_id) || !stmt->bind_text(2, entity_kind) || !stmt->bind_int64(3, *entity_id)) {
      return false;
    }
    auto const step = stmt->step();
    return step.has_value() && *step == db::step_result::row;
  }
  auto stmt = conn.prepare("select 1 from external_links where id = ? and entity_kind = ?");
  if (!stmt) {
    return false;
  }
  if (!stmt->bind_int64(1, link_id) || !stmt->bind_text(2, entity_kind)) {
    return false;
  }
  auto const step = stmt->step();
  return step.has_value() && *step == db::step_result::row;
}

/// @brief Does `row` survive every supplied filter?
/// @param conn The connection, for the link-join filters.
/// @param row The candidate row.
/// @param outcome Optional `--outcome` value.
/// @param since Optional `--since` value.
/// @param plan_id Optional `--plan` value.
/// @param system_slug Optional `--system` value.
/// @param entity Optional decomposed `--entity` value.
/// @return `true` when the row should be emitted.
auto event_matches(db::connection& conn, const sync_event_row& row, const std::optional<std::string>& outcome,
                   const std::optional<std::string>& since, std::optional<std::int64_t> plan_id,
                   const std::optional<std::string>& system_slug, const std::optional<entity_ref>& entity) -> bool {
  if (outcome.has_value() && row.outcome != *outcome) {
    return false;
  }
  if (since.has_value() && row.at < *since) {
    return false;
  }
  bool const need_link = plan_id.has_value() || system_slug.has_value() || entity.has_value();
  if (need_link) {
    if (!row.link_id.has_value()) {
      return false;
    }
    if (plan_id.has_value() && !link_belongs_to_plan(conn, *row.link_id, *plan_id)) {
      return false;
    }
    if (system_slug.has_value() && !link_belongs_to_system(conn, *row.link_id, *system_slug)) {
      return false;
    }
    if (entity.has_value() && !link_has_entity(conn, *row.link_id, entity->kind, std::optional{entity->id})) {
      return false;
    }
  }
  return true;
}

/// @brief Append one row as a JSON object, in the oracle's field order.
///
/// `context_json` is embedded RAW (it is already a JSON blob), not
/// string-escaped, matching `writeSyncEventJSON`.
/// @param out The buffer.
/// @param row The row.
auto append_sync_event(std::string& out, const sync_event_row& row) -> void {
  out.append(std::format("{{\"id\":{}", row.id));
  if (row.link_id.has_value()) {
    out.append(std::format(",\"link_id\":{}", *row.link_id));
  } else {
    out.append(",\"link_id\":null");
  }
  out.append(",\"scope\":");
  append_json_string(out, row.scope);
  out.append(",\"direction\":");
  append_json_string(out, row.direction);
  out.append(",\"outcome\":");
  append_json_string(out, row.outcome);
  out.append(",\"fields_changed\":");
  if (row.fields_changed.has_value()) {
    append_json_string(out, *row.fields_changed);
  } else {
    out.append("null");
  }
  out.append(",\"detail\":");
  if (row.detail.has_value()) {
    append_json_string(out, *row.detail);
  } else {
    out.append("null");
  }
  out.append(",\"context_json\":");
  if (row.context_json.has_value()) {
    out.append(*row.context_json);
  } else {
    out.append("null");
  }
  out.append(",\"at\":");
  append_json_string(out, row.at);
  out.push_back('}');
}

} // namespace

auto sync_events(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.db().ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  auto const limit = cliapp::flag_int(args, "--limit").value_or(100);

  std::optional<entity_ref> entity;
  if (auto const raw = cliapp::flag_string(args, "--entity"); raw.has_value()) {
    auto const colon = raw->find(':');
    if (colon == std::string::npos) {
      return std::unexpected(error_from_body(domain_error_kind::invalid_input, "sync-events: InvalidInput"));
    }
    auto const id = cliapp::parse_int64_zig(std::string_view{*raw}.substr(colon + 1));
    if (!id.has_value()) {
      return std::unexpected(error_from_body(domain_error_kind::invalid_input, "sync-events: InvalidInput"));
    }
    entity = entity_ref{.kind = raw->substr(0, colon), .id = *id};
  }

  auto const outcome     = cliapp::flag_string(args, "--outcome");
  auto const since       = cliapp::flag_string(args, "--since");
  auto const plan_id     = cliapp::flag_int(args, "--plan");
  auto const system_slug = cliapp::flag_string(args, "--system");

  auto rows = list_sync_events(**conn, limit);
  if (!rows) {
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "sync-events: QueryFailed"));
  }

  bool const  json = cliapp::flag_bool(args, "--json");
  std::string out;
  if (json) {
    out.append("{\"generated_at\":");
    append_json_string(out, format::now_iso());
    out.append(",\"sync_events\":[");
    bool first = true;
    for (auto const& row : *rows) {
      if (!event_matches(**conn, row, outcome, since, plan_id, system_slug, entity)) {
        continue;
      }
      if (!first) {
        out.push_back(',');
      }
      first = false;
      append_sync_event(out, row);
    }
    out.append("]}\n");
    ctx.out() << out;
    return {};
  }

  std::size_t count = 0;
  for (auto const& row : *rows) {
    if (event_matches(**conn, row, outcome, since, plan_id, system_slug, entity)) {
      ++count;
    }
  }
  out.append(std::format("sync_events: {}\n", count));
  for (auto const& row : *rows) {
    if (!event_matches(**conn, row, outcome, since, plan_id, system_slug, entity)) {
      continue;
    }
    out.append(std::format("  id:{}  link:{}  scope:{}  direction:{}  outcome:{}  at:{}\n", row.id, row.link_id.value_or(0),
                           row.scope, row.direction, row.outcome, row.at));
  }
  ctx.out() << out;
  return {};
}

} // namespace planar::cmd::watch::handlers
