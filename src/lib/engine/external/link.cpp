/// @file link.cpp
/// @brief Implementation of `planar.engine.external.link`. See link.cppm
/// for the port scope and the per-cut deferral rationale.

module planar.engine.external.link;

import std;
import planar.db;

namespace planar::engine::external::link {

namespace {

// SQLITE_CONSTRAINT_UNIQUE — the same constant plan.cpp / task.cpp /
// annotation.cpp / entitylink.cpp already use to detect a UNIQUE violation
// without string-matching the driver's message.
constexpr int k_sqlite_constraint_unique = 2067;

auto is_unique_violation(const db::db_error& err) -> bool {
  return err.code_ == k_sqlite_constraint_unique;
}

constexpr std::string_view k_select_one = "select id, entity_kind, entity_id, system_id, external_id, external_url, "
                                          "link_role, sync_direction, last_synced_at, last_sync_status, config_json, "
                                          "created_at from external_links where id = ?";

/// @brief Read a nullable TEXT column.
/// @param stmt The stepped statement.
/// @param index The zero-based column index.
/// @return The value, or unset when the column is SQL NULL.
auto text_opt(const db::statement& stmt, int index) -> std::optional<std::string> {
  if (stmt.is_null(index)) {
    return std::nullopt;
  }
  return stmt.column_text(index);
}

/// @brief Materialize an `ext_link` from a stepped row.
///
/// Every enum column is re-parsed rather than trusted: the CHECK
/// constraints make an unrecognized value impossible through this binary,
/// but the Zig original still maps one to `QueryFailed` and this port
/// preserves that rather than silently defaulting.
/// @param stmt The statement positioned on a row.
/// @return The row, or `link_error::query_failed` on an unparseable enum.
auto read_row(const db::statement& stmt) -> std::expected<ext_link, link_error> {
  auto const kind = external_entity_kind_from_text(stmt.column_text(1));
  if (!kind.has_value()) {
    return std::unexpected(link_error::query_failed);
  }
  auto const role = link_role_from_text(stmt.column_text(6));
  if (!role.has_value()) {
    return std::unexpected(link_error::query_failed);
  }
  auto const direction = sync_direction_from_text(stmt.column_text(7));
  if (!direction.has_value()) {
    return std::unexpected(link_error::query_failed);
  }
  auto const status = sync_status_from_text(stmt.column_text(9));
  if (!status.has_value()) {
    return std::unexpected(link_error::query_failed);
  }
  return ext_link{
      .id               = stmt.column_int64(0),
      .entity_kind      = *kind,
      .entity_id        = stmt.column_int64(2),
      .system_id        = stmt.column_int64(3),
      .external_id      = stmt.column_text(4),
      .external_url     = text_opt(stmt, 5),
      .role             = *role,
      .direction        = *direction,
      .last_synced_at   = text_opt(stmt, 8),
      .last_sync_status = *status,
      .config_json      = text_opt(stmt, 10),
      .created_at       = stmt.column_text(11),
  };
}

/// @brief Bind an optional TEXT parameter.
///
/// Explicit `bind_null` rather than leaning on `bind_text`'s behavior for a
/// default-constructed `string_view`: `planar.db`'s `bind_text` forwards
/// `value.data()`, which is a null pointer for a default-constructed view,
/// and SQLite binds SQL NULL for it. That happens to be right here and
/// would be wrong for a genuinely-empty string, so the intent is spelled
/// out rather than inherited from a trap (task 6097 owns the root fix).
/// @param stmt The statement to bind on.
/// @param index The one-based parameter index.
/// @param value The value, or unset for SQL NULL.
/// @return Whether the bind succeeded.
auto bind_text_opt(db::statement& stmt, int index, const std::optional<std::string>& value) -> bool {
  if (!value.has_value()) {
    return stmt.bind_null(index).has_value();
  }
  return stmt.bind_text(index, *value).has_value();
}

} // namespace

auto external_entity_kind_from_text(std::string_view s) -> std::optional<external_entity_kind> {
  if (s == "plan") {
    return external_entity_kind::plan;
  }
  if (s == "task") {
    return external_entity_kind::task;
  }
  if (s == "question") {
    return external_entity_kind::question;
  }
  if (s == "test_scenario") {
    return external_entity_kind::test_scenario;
  }
  if (s == "artifact") {
    return external_entity_kind::artifact;
  }
  if (s == "decision") {
    return external_entity_kind::decision;
  }
  if (s == "session") {
    return external_entity_kind::session;
  }
  return std::nullopt;
}

auto external_entity_kind_to_text(external_entity_kind k) -> std::string_view {
  switch (k) {
  case external_entity_kind::plan:
    return "plan";
  case external_entity_kind::task:
    return "task";
  case external_entity_kind::question:
    return "question";
  case external_entity_kind::test_scenario:
    return "test_scenario";
  case external_entity_kind::artifact:
    return "artifact";
  case external_entity_kind::decision:
    return "decision";
  case external_entity_kind::session:
    return "session";
  }
  return "plan";
}

auto link_role_from_text(std::string_view s) -> std::optional<link_role> {
  if (s == "mirror") {
    return link_role::mirror;
  }
  if (s == "parent") {
    return link_role::parent;
  }
  if (s == "child") {
    return link_role::child;
  }
  if (s == "reference") {
    return link_role::reference;
  }
  return std::nullopt;
}

auto link_role_to_text(link_role r) -> std::string_view {
  switch (r) {
  case link_role::mirror:
    return "mirror";
  case link_role::parent:
    return "parent";
  case link_role::child:
    return "child";
  case link_role::reference:
    return "reference";
  }
  return "mirror";
}

auto sync_direction_from_text(std::string_view s) -> std::optional<sync_direction> {
  if (s == "read-only") {
    return sync_direction::read_only;
  }
  if (s == "write-back") {
    return sync_direction::write_back;
  }
  if (s == "two-way") {
    return sync_direction::two_way;
  }
  return std::nullopt;
}

auto sync_direction_to_text(sync_direction d) -> std::string_view {
  switch (d) {
  case sync_direction::read_only:
    return "read-only";
  case sync_direction::write_back:
    return "write-back";
  case sync_direction::two_way:
    return "two-way";
  }
  return "two-way";
}

auto sync_status_from_text(std::string_view s) -> std::optional<sync_status> {
  if (s == "ok") {
    return sync_status::ok;
  }
  if (s == "conflict") {
    return sync_status::conflict;
  }
  if (s == "error") {
    return sync_status::error;
  }
  if (s == "never") {
    return sync_status::never;
  }
  return std::nullopt;
}

auto sync_status_to_text(sync_status s) -> std::string_view {
  switch (s) {
  case sync_status::ok:
    return "ok";
  case sync_status::conflict:
    return "conflict";
  case sync_status::error:
    return "error";
  case sync_status::never:
    return "never";
  }
  return "never";
}

auto create(db::connection& conn, const create_args& args) -> std::expected<ext_link, link_error> {
  auto stmt = conn.prepare("insert into external_links "
                           "(entity_kind, entity_id, system_id, external_id, external_url, link_role, sync_direction, "
                           "last_sync_status, config_json) values (?, ?, ?, ?, ?, ?, ?, ?, ?) returning id");
  if (!stmt) {
    return std::unexpected(link_error::query_failed);
  }
  if (!stmt->bind_text(1, external_entity_kind_to_text(args.entity_kind)) || !stmt->bind_int64(2, args.entity_id) ||
      !stmt->bind_int64(3, args.system_id) || !stmt->bind_text(4, args.external_id) ||
      !bind_text_opt(*stmt, 5, args.external_url) || !stmt->bind_text(6, link_role_to_text(args.role)) ||
      !stmt->bind_text(7, sync_direction_to_text(args.direction)) ||
      !stmt->bind_text(8, sync_status_to_text(args.initial_status)) || !bind_text_opt(*stmt, 9, args.config_json)) {
    return std::unexpected(link_error::query_failed);
  }
  auto stepped = stmt->step();
  if (!stepped) {
    return std::unexpected(is_unique_violation(stepped.error()) ? link_error::link_exists : link_error::query_failed);
  }
  if (*stepped == db::step_result::done) {
    return std::unexpected(link_error::query_failed);
  }
  return show(conn, stmt->column_int64(0));
}

auto show(db::connection& conn, std::int64_t id) -> std::expected<ext_link, link_error> {
  auto stmt = conn.prepare(k_select_one);
  if (!stmt) {
    return std::unexpected(link_error::query_failed);
  }
  if (!stmt->bind_int64(1, id)) {
    return std::unexpected(link_error::query_failed);
  }
  auto stepped = stmt->step();
  if (!stepped) {
    return std::unexpected(link_error::query_failed);
  }
  if (*stepped == db::step_result::done) {
    return std::unexpected(link_error::not_found);
  }
  return read_row(*stmt);
}

auto remove(db::connection& conn, std::int64_t id) -> std::expected<void, link_error> {
  // `returning id` stands in for zig's `changes() == 0` check — a row comes
  // back exactly when one matched. See this module's header.
  auto stmt = conn.prepare("delete from external_links where id = ? returning id");
  if (!stmt) {
    return std::unexpected(link_error::query_failed);
  }
  if (!stmt->bind_int64(1, id)) {
    return std::unexpected(link_error::query_failed);
  }
  auto stepped = stmt->step();
  if (!stepped) {
    return std::unexpected(link_error::query_failed);
  }
  if (*stepped == db::step_result::done) {
    return std::unexpected(link_error::not_found);
  }
  return {};
}

} // namespace planar::engine::external::link
