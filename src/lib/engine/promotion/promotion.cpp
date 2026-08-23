/// @file promotion.cpp
/// @brief Implementation of `planar.engine.promotion` (plan 996, task
/// 6094). See promotion.cppm for scope, omissions, and the oracle-derived
/// output shapes.

module planar.engine.promotion;

import std;
import planar.db;
import planar.scope_ref;

namespace planar::engine::promotion {

namespace {

/// @brief Map an entity kind string to its backing table. Mirrors zig's
/// `tableFor`; `std::nullopt` means the caller returns `invalid_scope`.
auto table_for(std::string_view kind) -> std::optional<std::string_view> {
  if (kind == "plan") {
    return "plans";
  }
  if (kind == "task") {
    return "tasks";
  }
  if (kind == "question") {
    return "questions";
  }
  if (kind == "test_scenario") {
    return "test_scenarios";
  }
  if (kind == "artifact") {
    return "artifacts";
  }
  if (kind == "decision") {
    return "decisions";
  }
  return std::nullopt;
}

/// @brief The scope a promotable row currently sits at, as stored.
struct current_scope {
  std::string                 kind;
  std::optional<std::int64_t> id;
};

/// @brief Read `(scope_kind, scope_id)` from `table` for `entity_id`.
auto read_current_scope(db::connection& conn, std::string_view table, std::int64_t entity_id)
    -> std::expected<current_scope, promote_error> {
  auto stmt = conn.prepare(std::format("select scope_kind, scope_id from {} where id = ?", table));
  if (!stmt) {
    return std::unexpected(promote_error::query_failed);
  }
  if (auto bound = stmt->bind_int64(1, entity_id); !bound) {
    return std::unexpected(promote_error::query_failed);
  }
  auto step = stmt->step();
  if (!step) {
    return std::unexpected(promote_error::query_failed);
  }
  if (*step != db::step_result::row) {
    return std::unexpected(promote_error::not_found);
  }
  current_scope out{.kind = stmt->column_text(0), .id = std::nullopt};
  if (!stmt->is_null(1)) {
    out.id = stmt->column_int64(1);
  }
  return out;
}

/// @brief The shared UPDATE both directions issue. `updated_at` is bumped
/// with the same `strftime` expression the Zig original uses, so the
/// stored timestamp format is unchanged.
auto apply_scope_update(db::connection& conn, std::string_view table, std::string_view scope_kind,
                        std::optional<std::int64_t> scope_id, std::int64_t entity_id) -> std::expected<void, promote_error> {
  auto sql  = std::format("update {} set scope_kind = ?, scope_id = ?, "
                          "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') where id = ?",
                          table);
  auto stmt = conn.prepare(sql);
  if (!stmt) {
    return std::unexpected(promote_error::query_failed);
  }
  if (auto b = stmt->bind_text(1, scope_kind); !b) {
    return std::unexpected(promote_error::query_failed);
  }
  auto b2 = scope_id.has_value() ? stmt->bind_int64(2, *scope_id) : stmt->bind_null(2);
  if (!b2) {
    return std::unexpected(promote_error::query_failed);
  }
  if (auto b = stmt->bind_int64(3, entity_id); !b) {
    return std::unexpected(promote_error::query_failed);
  }
  if (auto step = stmt->step(); !step) {
    return std::unexpected(promote_error::query_failed);
  }
  return {};
}

/// @brief Shared demote path used by both `demote` and
/// `promote(to_scope == "global")`. Mirrors zig's `demoteEntity`.
auto demote_entity(db::connection& conn, std::string_view table, std::int64_t id) -> std::expected<void, promote_error> {
  auto current = read_current_scope(conn, table, id);
  if (!current) {
    return std::unexpected(current.error());
  }
  if (current->kind == "global") {
    return std::unexpected(promote_error::scope_unchanged);
  }
  return apply_scope_update(conn, table, "global", std::nullopt, id);
}

/// @brief Render `scope_id`-style optional integers the way the oracle
/// does: the bare digits, or the literal `null`.
auto json_optional_int(std::optional<std::int64_t> value) -> std::string {
  return value.has_value() ? std::format("{}", *value) : std::string{"null"};
}

/// @brief Render a stored scope the way both handlers' text output does:
/// `"<kind>:<id>"` when `scope_id` is set, else just `"<kind>"`.
auto scope_display(const scope_info& s) -> std::string {
  return s.scope_id.has_value() ? std::format("{}:{}", s.scope_kind, *s.scope_id) : s.scope_kind;
}

/// @brief Minimal JSON string escaper matching the subset of
/// `std::json.Stringify.encodeJsonString` the oracle exercises for the
/// two fields this module quotes (`kind`, `scope_kind`) — both of which
/// are closed vocabularies of `[a-z_]` today. Kept honest anyway so a
/// future kind carrying a quote or backslash cannot emit invalid JSON.
auto json_quote(std::string_view s) -> std::string {
  std::string out;
  out.reserve(s.size() + 2);
  out.push_back('"');
  for (const char c : s) {
    switch (c) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      if (static_cast<unsigned char>(c) < 0x20) {
        out += std::format("\\u{:04x}", static_cast<unsigned>(static_cast<unsigned char>(c)));
      } else {
        out.push_back(c);
      }
    }
  }
  out.push_back('"');
  return out;
}

} // namespace

auto read_entity_scope(db::connection& conn, std::string_view kind, std::int64_t id) -> std::expected<scope_info, promote_error> {
  const auto table = table_for(kind);
  if (!table.has_value()) {
    return std::unexpected(promote_error::invalid_scope);
  }
  auto current = read_current_scope(conn, *table, id);
  if (!current) {
    return std::unexpected(current.error());
  }
  return scope_info{.scope_kind = std::move(current->kind), .scope_id = current->id};
}

auto promote(db::connection& conn, const promote_args& args) -> std::expected<void, promote_error> {
  const auto table = table_for(args.kind);
  if (!table.has_value()) {
    return std::unexpected(promote_error::invalid_scope);
  }
  if (args.to_scope.starts_with("repo:")) {
    return std::unexpected(promote_error::unsupported_scope);
  }
  if (args.to_scope == "global") {
    return demote_entity(conn, *table, args.id);
  }

  auto resolved = scope_ref::resolve(conn, args.to_scope);
  if (!resolved) {
    switch (resolved.error()) {
    case scope_ref::error::slug_not_found:
      return std::unexpected(promote_error::slug_not_found);
    case scope_ref::error::query_failed:
      return std::unexpected(promote_error::query_failed);
    }
    return std::unexpected(promote_error::query_failed);
  }
  // `scope_ref::resolve` only ever yields `repo` for a `repo:` prefix,
  // which the guard above already rejected; a `global` ref carries no id.
  // Both leave `id` unset, and the Zig original maps that to QueryFailed
  // (`scope_ref.id orelse return Error.QueryFailed`).
  if (!resolved->id.has_value()) {
    return std::unexpected(promote_error::query_failed);
  }
  const auto assoc_id = *resolved->id;

  auto current = read_current_scope(conn, *table, args.id);
  if (!current) {
    return std::unexpected(current.error());
  }
  if (current->kind == "association" && current->id.has_value() && *current->id == assoc_id) {
    return std::unexpected(promote_error::scope_unchanged);
  }

  return apply_scope_update(conn, *table, "association", assoc_id, args.id);
}

auto demote(db::connection& conn, std::string_view kind, std::int64_t id) -> std::expected<void, promote_error> {
  const auto table = table_for(kind);
  if (!table.has_value()) {
    return std::unexpected(promote_error::invalid_scope);
  }
  return demote_entity(conn, *table, id);
}

auto render_scope_change_json(std::string_view kind, std::int64_t id, const scope_info& current, const scope_info& previous)
    -> std::string {
  return std::format(R"({{"ok":true,"kind":{},"id":{},"scope_kind":{},"scope_id":{},)"
                     R"("previous_scope_kind":{},"previous_scope_id":{}}})",
                     json_quote(kind), id, json_quote(current.scope_kind), json_optional_int(current.scope_id),
                     json_quote(previous.scope_kind), json_optional_int(previous.scope_id));
}

auto render_promote_text(std::string_view kind, std::int64_t id, std::string_view to_scope, const scope_info& previous)
    -> std::string {
  return std::format("{}:{} promoted to association {}  (was: {})", kind, id, to_scope, scope_display(previous));
}

auto render_demote_text(std::string_view kind, std::int64_t id, const scope_info& previous) -> std::string {
  return std::format("{}:{} demoted to global  (was: {})", kind, id, scope_display(previous));
}

} // namespace planar::engine::promotion
