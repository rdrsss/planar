/// @file link.cpp
/// @brief Implementation of `planar.engine.external.link`. See link.cppm
/// for the port scope and the per-cut deferral rationale.

module planar.engine.external.link;

import std;
import planar.db;
import planar.json_dom;
import planar.log;

namespace planar::engine::external::link {

namespace {

/// @brief Emit the oracle's inner `<op> exec failed: <ErrorName>` diagnostic
/// ahead of the outer handler error. See `zig/src/engine/external/link.zig`'s
/// `create` for the shape this ports; mirrors `engine::planning::exec_failed`.
auto exec_failed(std::string_view op, std::string_view zig_error_name) -> link_error {
  log::diag_err(std::format("{} exec failed: {}", op, zig_error_name));
  return link_error::query_failed;
}

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

// The same column list `k_select_one` selects, but qualified with the `el`
// alias the filtered `list` query needs for its optional
// `join external_systems es` — the Zig original keeps two spellings for
// exactly this reason (`select_columns` and `select_list_select`).
constexpr std::string_view k_select_columns_aliased =
    "select el.id, el.entity_kind, el.entity_id, el.system_id, el.external_id, el.external_url, el.link_role, "
    "el.sync_direction, el.last_synced_at, el.last_sync_status, el.config_json, el.created_at from external_links el";
constexpr std::string_view k_select_columns =
    "select id, entity_kind, entity_id, system_id, external_id, external_url, link_role, sync_direction, "
    "last_synced_at, last_sync_status, config_json, created_at from external_links";

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
    return std::unexpected(exec_failed("external.link.create", "PrepareFailed"));
  }
  if (!stmt->bind_text(1, external_entity_kind_to_text(args.entity_kind)) || !stmt->bind_int64(2, args.entity_id) ||
      !stmt->bind_int64(3, args.system_id) || !stmt->bind_text(4, args.external_id) ||
      !bind_text_opt(*stmt, 5, args.external_url) || !stmt->bind_text(6, link_role_to_text(args.role)) ||
      !stmt->bind_text(7, sync_direction_to_text(args.direction)) ||
      !stmt->bind_text(8, sync_status_to_text(args.initial_status)) || !bind_text_opt(*stmt, 9, args.config_json)) {
    return std::unexpected(exec_failed("external.link.create", "BindFailed"));
  }
  auto stepped = stmt->step();
  if (!stepped) {
    if (is_unique_violation(stepped.error())) {
      return std::unexpected(link_error::link_exists);
    }
    return std::unexpected(exec_failed("external.link.create", "StepFailed"));
  }
  if (*stepped == db::step_result::done) {
    return std::unexpected(exec_failed("external.link.create", "StepFailed"));
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

namespace {

/// @brief Step a prepared statement to exhaustion, materializing every row.
/// @param stmt The bound statement.
/// @return The rows, or the failure.
auto collect(db::statement& stmt) -> std::expected<std::vector<ext_link>, link_error> {
  std::vector<ext_link> out;
  while (true) {
    auto stepped = stmt.step();
    if (!stepped) {
      return std::unexpected(link_error::query_failed);
    }
    if (*stepped == db::step_result::done) {
      break;
    }
    auto row = read_row(stmt);
    if (!row) {
      return std::unexpected(row.error());
    }
    out.push_back(std::move(*row));
  }
  return out;
}

/// @brief Run one unparameterized SELECT over `external_links`.
/// @param conn The connection.
/// @param sql The statement.
/// @return The rows, or the failure.
auto read_many(db::connection& conn, std::string_view sql) -> std::expected<std::vector<ext_link>, link_error> {
  auto stmt = conn.prepare(sql);
  if (!stmt) {
    return std::unexpected(link_error::query_failed);
  }
  return collect(*stmt);
}

} // namespace

auto list(db::connection& conn, const list_filter& filter) -> std::expected<std::vector<ext_link>, link_error> {
  // Built exactly the way the Zig original builds it: the join is added ONLY
  // when a system slug is filtered on, then a `where 1 = 1` so every
  // subsequent clause can unconditionally start with `and`.
  std::string sql(k_select_columns_aliased);
  if (filter.system_slug.has_value()) {
    sql += " join external_systems es on es.id = el.system_id";
  }
  sql += " where 1 = 1";
  if (filter.system_slug.has_value()) {
    sql += " and es.slug = ?";
  }
  if (filter.entity_kind.has_value()) {
    sql += " and el.entity_kind = ?";
  }
  if (filter.entity_id.has_value()) {
    sql += " and el.entity_id = ?";
  }
  if (filter.system_id.has_value()) {
    sql += " and el.system_id = ?";
  }
  sql += " order by el.id";

  auto stmt = conn.prepare(sql);
  if (!stmt) {
    return std::unexpected(link_error::query_failed);
  }
  // Bind in the SAME order the clauses were appended — the parameter indexes
  // are positional, so a reordering here silently binds the wrong values to
  // the wrong columns rather than failing.
  int index = 1;
  if (filter.system_slug.has_value() && !stmt->bind_text(index++, *filter.system_slug)) {
    return std::unexpected(link_error::query_failed);
  }
  if (filter.entity_kind.has_value() && !stmt->bind_text(index++, external_entity_kind_to_text(*filter.entity_kind))) {
    return std::unexpected(link_error::query_failed);
  }
  if (filter.entity_id.has_value() && !stmt->bind_int64(index++, *filter.entity_id)) {
    return std::unexpected(link_error::query_failed);
  }
  if (filter.system_id.has_value() && !stmt->bind_int64(index++, *filter.system_id)) {
    return std::unexpected(link_error::query_failed);
  }
  return collect(*stmt);
}

auto links_for_entity(db::connection& conn, external_entity_kind entity_kind, std::int64_t entity_id)
    -> std::expected<std::vector<ext_link>, link_error> {
  return list(conn, {.entity_kind = entity_kind, .entity_id = entity_id});
}

auto all_pullable(db::connection& conn) -> std::expected<std::vector<ext_link>, link_error> {
  return read_many(conn, std::format("{} where sync_direction in ('read-only','two-way') order by id", k_select_columns));
}

auto all_pushable(db::connection& conn) -> std::expected<std::vector<ext_link>, link_error> {
  return read_many(conn, std::format("{} where sync_direction in ('write-back','two-way') order by id", k_select_columns));
}

auto update_sync_state(db::connection& conn, std::int64_t link_id, sync_status status) -> std::expected<void, link_error> {
  // `returning id` stands in for the Zig original's `changes() == 0` check,
  // the same substitution `remove` documents at the top of this file.
  auto stmt = conn.prepare("update external_links set last_synced_at = strftime('%Y-%m-%dT%H:%M:%fZ','now'), "
                           "last_sync_status = ? where id = ? returning id");
  if (!stmt) {
    return std::unexpected(link_error::query_failed);
  }
  if (!stmt->bind_text(1, sync_status_to_text(status)) || !stmt->bind_int64(2, link_id)) {
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

auto update_sync_direction(db::connection& conn, std::int64_t link_id, sync_direction direction)
    -> std::expected<sync_direction, link_error> {
  auto tx = conn.begin_transaction(db::lock_mode::immediate);
  if (!tx) {
    return std::unexpected(link_error::query_failed);
  }

  sync_direction prior = sync_direction::two_way;
  {
    auto read = conn.prepare("select sync_direction from external_links where id = ?");
    if (!read || !read->bind_int64(1, link_id)) {
      return std::unexpected(link_error::query_failed);
    }
    auto stepped = read->step();
    if (!stepped) {
      return std::unexpected(link_error::query_failed);
    }
    if (*stepped == db::step_result::done) {
      return std::unexpected(link_error::not_found);
    }
    auto const parsed = sync_direction_from_text(read->column_text(0));
    if (!parsed.has_value()) {
      return std::unexpected(link_error::query_failed);
    }
    prior = *parsed;
  }

  {
    auto write = conn.prepare("update external_links set sync_direction = ? where id = ?");
    if (!write || !write->bind_text(1, sync_direction_to_text(direction)) || !write->bind_int64(2, link_id)) {
      return std::unexpected(link_error::query_failed);
    }
    if (!write->step()) {
      return std::unexpected(link_error::query_failed);
    }
  }

  // Raw interpolation, matching the Zig original — both values come from a
  // closed enum set, so there is nothing to escape. See link.cppm.
  auto const detail = std::format(R"({{"old_direction":"{}","new_direction":"{}","operation":"sync_direction_update"}})",
                                  sync_direction_to_text(prior), sync_direction_to_text(direction));
  {
    auto event = conn.prepare("insert into sync_events (link_id, direction, outcome, fields_changed, detail) "
                              "values (?, 'push', 'ok', ?, ?)");
    if (!event || !event->bind_int64(1, link_id) || !event->bind_text(2, R"(["sync_direction"])") ||
        !event->bind_text(3, detail)) {
      return std::unexpected(link_error::query_failed);
    }
    if (!event->step()) {
      return std::unexpected(link_error::query_failed);
    }
  }

  if (!tx->commit()) {
    return std::unexpected(link_error::query_failed);
  }
  return prior;
}

auto load_baseline(db::connection& conn, std::int64_t link_id) -> std::expected<baseline, link_error> {
  auto stmt = conn.prepare("select baseline_title, baseline_status from external_links where id = ?");
  if (!stmt || !stmt->bind_int64(1, link_id)) {
    return std::unexpected(link_error::query_failed);
  }
  auto stepped = stmt->step();
  if (!stepped) {
    return std::unexpected(link_error::query_failed);
  }
  if (*stepped == db::step_result::done) {
    return std::unexpected(link_error::not_found);
  }
  return baseline{.title = text_opt(*stmt, 0), .status = text_opt(*stmt, 1)};
}

auto store_baseline(db::connection& conn, std::int64_t link_id, std::string_view title, std::string_view status_value)
    -> std::expected<void, link_error> {
  auto stmt = conn.prepare("update external_links set baseline_title = ?, baseline_status = ? where id = ?");
  if (!stmt) {
    return std::unexpected(link_error::query_failed);
  }
  if (!stmt->bind_text(1, title) || !stmt->bind_text(2, status_value) || !stmt->bind_int64(3, link_id)) {
    return std::unexpected(link_error::query_failed);
  }
  if (!stmt->step()) {
    return std::unexpected(link_error::query_failed);
  }
  return {};
}

auto load_existing_mirror(db::connection& conn, std::string_view entity_kind, std::int64_t entity_id, std::int64_t system_id)
    -> std::expected<std::string, link_error> {
  auto stmt = conn.prepare("select coalesce(external_id, '') from external_links "
                           "where entity_kind = ? and entity_id = ? and system_id = ? and link_role = 'mirror' limit 1");
  if (!stmt) {
    return std::unexpected(link_error::query_failed);
  }
  if (!stmt->bind_text(1, entity_kind) || !stmt->bind_int64(2, entity_id) || !stmt->bind_int64(3, system_id)) {
    return std::unexpected(link_error::query_failed);
  }
  auto stepped = stmt->step();
  if (!stepped) {
    return std::unexpected(link_error::query_failed);
  }
  // No row is the EMPTY STRING, not `not_found` — see link.cppm.
  if (*stepped == db::step_result::done) {
    return std::string{};
  }
  return std::string{stmt->column_text(0)};
}

auto record_mirror_link(db::connection& conn, std::string_view entity_kind, std::int64_t entity_id, std::int64_t system_id,
                        std::string_view external_id, std::string_view external_url, sync_direction direction)
    -> std::expected<std::int64_t, link_error> {
  auto tx = conn.begin_transaction(db::lock_mode::immediate);
  if (!tx) {
    return std::unexpected(link_error::query_failed);
  }

  std::int64_t link_id = 0;
  {
    auto stmt = conn.prepare("insert into external_links "
                             "(entity_kind, entity_id, system_id, external_id, external_url, link_role, "
                             "sync_direction, last_sync_status) "
                             "values (?, ?, ?, ?, ?, 'mirror', ?, 'ok') returning id");
    if (!stmt) {
      return std::unexpected(link_error::query_failed);
    }
    if (!stmt->bind_text(1, entity_kind) || !stmt->bind_int64(2, entity_id) || !stmt->bind_int64(3, system_id) ||
        !stmt->bind_text(4, external_id)) {
      return std::unexpected(link_error::query_failed);
    }
    // An EMPTY url is SQL NULL, not an empty string — the oracle's explicit
    // null branch. `list`/`show` then report it as unset.
    auto const url_opt = external_url.empty() ? std::nullopt : std::optional{std::string{external_url}};
    if (!bind_text_opt(*stmt, 5, url_opt) || !stmt->bind_text(6, sync_direction_to_text(direction))) {
      return std::unexpected(link_error::query_failed);
    }
    auto stepped = stmt->step();
    if (!stepped) {
      return std::unexpected(is_unique_violation(stepped.error()) ? link_error::link_exists : link_error::query_failed);
    }
    if (*stepped == db::step_result::done) {
      return std::unexpected(link_error::query_failed);
    }
    link_id = stmt->column_int64(0);
  }

  // The second write is what separates this from `create`. `fields_changed`
  // and `detail` stay NULL — the oracle's three-column insert.
  {
    auto event = conn.prepare("insert into sync_events (link_id, direction, outcome) values (?, 'push', 'ok')");
    if (!event || !event->bind_int64(1, link_id)) {
      return std::unexpected(link_error::query_failed);
    }
    if (!event->step()) {
      return std::unexpected(link_error::query_failed);
    }
  }

  if (!tx->commit()) {
    return std::unexpected(link_error::query_failed);
  }
  return link_id;
}

// ---- strategy stickiness / verify-counterparts -----------------------------

auto read_cached_strategy(db::connection& conn, std::int64_t anchor_plan_id, std::int64_t system_id)
    -> std::expected<std::optional<std::string>, link_error> {
  auto stmt = conn.prepare("select coalesce(config_json, '') from external_links "
                           "where entity_kind = 'plan' and entity_id = ? and system_id = ? and link_role = 'mirror' limit 1");
  if (!stmt || !stmt->bind_int64(1, anchor_plan_id) || !stmt->bind_int64(2, system_id)) {
    return std::unexpected(link_error::query_failed);
  }
  auto stepped = stmt->step();
  if (!stepped) {
    return std::unexpected(link_error::query_failed);
  }
  if (*stepped == db::step_result::done) {
    return std::optional<std::string>{std::nullopt};
  }
  auto const raw = stmt->column_text(0);
  if (raw.empty()) {
    return std::optional<std::string>{std::nullopt};
  }
  auto parsed = json_dom::parse_json(raw);
  if (!parsed || parsed->kind != json_dom::json_kind::object) {
    return std::optional<std::string>{std::nullopt};
  }
  auto const* v = parsed->find(strategy_cache_key);
  if (v == nullptr || v->kind != json_dom::json_kind::string) {
    return std::optional<std::string>{std::nullopt};
  }
  return std::optional<std::string>{v->string};
}

auto list_mirror_links_in_tree(db::connection& conn, std::int64_t anchor_plan_id, std::int64_t system_id)
    -> std::expected<std::vector<mirror_link>, link_error> {
  auto stmt = conn.prepare("with recursive plan_tree(id) as ("
                           "  select ? union all"
                           "  select p.id from plans p join plan_tree pt on p.parent_plan_id = pt.id"
                           ")"
                           "select el.id, el.entity_kind, el.entity_id, el.external_id, coalesce(el.external_url, '') "
                           "from external_links el "
                           "where el.system_id = ? and el.link_role = 'mirror' "
                           "  and ("
                           "    (el.entity_kind = 'plan' and el.entity_id in (select id from plan_tree))"
                           "    or (el.entity_kind = 'task' and el.entity_id in ("
                           "      select t.id from tasks t"
                           "      join entity_links tl on tl.from_kind = 'task' and tl.from_id = t.id"
                           "                          and tl.to_kind = 'plan' and tl.relationship = 'derives-from'"
                           "      where tl.to_id in (select id from plan_tree)"
                           "    ))"
                           "  )"
                           "order by el.id");
  if (!stmt || !stmt->bind_int64(1, anchor_plan_id) || !stmt->bind_int64(2, system_id)) {
    return std::unexpected(link_error::query_failed);
  }
  std::vector<mirror_link> out;
  while (true) {
    auto stepped = stmt->step();
    if (!stepped) {
      return std::unexpected(link_error::query_failed);
    }
    if (*stepped == db::step_result::done) {
      break;
    }
    out.push_back(mirror_link{
        .id           = stmt->column_int64(0),
        .entity_kind  = std::string{stmt->column_text(1)},
        .entity_id    = stmt->column_int64(2),
        .external_id  = std::string{stmt->column_text(3)},
        .external_url = std::string{stmt->column_text(4)},
    });
  }
  return out;
}

auto record_counterpart_missing(db::connection& conn, std::int64_t link_id, std::string_view entity_kind, std::int64_t entity_id,
                                std::string_view external_id, bool unlink_or_recreate) -> std::expected<void, link_error> {
  auto tx = conn.begin_transaction(db::lock_mode::immediate);
  if (!tx) {
    return std::unexpected(link_error::query_failed);
  }

  auto const ctx_json =
      std::format(R"({{"entity_kind":"{}","entity_id":{},"external_id":"{}"}})", entity_kind, entity_id, external_id);
  {
    auto event = conn.prepare("insert into sync_events (link_id, direction, outcome, context_json) "
                              "values (?, 'push', 'counterpart-missing', ?)");
    if (!event || !event->bind_int64(1, link_id) || !event->bind_text(2, ctx_json)) {
      return std::unexpected(link_error::query_failed);
    }
    if (!event->step()) {
      return std::unexpected(link_error::query_failed);
    }
  }

  if (unlink_or_recreate) {
    auto del = conn.prepare("delete from external_links where id = ?");
    if (!del || !del->bind_int64(1, link_id)) {
      return std::unexpected(link_error::query_failed);
    }
    if (!del->step()) {
      return std::unexpected(link_error::query_failed);
    }
  }

  if (!tx->commit()) {
    return std::unexpected(link_error::query_failed);
  }
  return {};
}

auto abandon_counterparts(db::connection& conn, std::int64_t anchor_plan_id, std::int64_t system_id,
                          std::string_view old_strategy, std::string_view new_strategy)
    -> std::expected<std::size_t, link_error> {
  struct row {
    std::int64_t id = 0;
    std::string  entity_kind;
    std::int64_t entity_id = 0;
    std::string  external_id;
  };
  std::vector<row> rows;
  {
    auto stmt = conn.prepare("with recursive plan_tree(id) as ("
                             "  select ? union all"
                             "  select p.id from plans p join plan_tree pt on p.parent_plan_id = pt.id"
                             ")"
                             "select el.id, el.entity_kind, el.entity_id, el.external_id "
                             "from external_links el "
                             "where el.system_id = ? and el.link_role = 'mirror' "
                             "  and ("
                             "    (el.entity_kind = 'plan' and el.entity_id in (select id from plan_tree))"
                             "    or (el.entity_kind = 'task' and el.entity_id in ("
                             "      select t.id from tasks t"
                             "      join entity_links tl on tl.from_kind = 'task' and tl.from_id = t.id"
                             "                          and tl.to_kind = 'plan' and tl.relationship = 'derives-from'"
                             "      where tl.to_id in (select id from plan_tree)"
                             "    ))"
                             "  )"
                             "order by el.id");
    if (!stmt || !stmt->bind_int64(1, anchor_plan_id) || !stmt->bind_int64(2, system_id)) {
      return std::unexpected(link_error::query_failed);
    }
    while (true) {
      auto stepped = stmt->step();
      if (!stepped) {
        return std::unexpected(link_error::query_failed);
      }
      if (*stepped == db::step_result::done) {
        break;
      }
      rows.push_back(row{
          .id          = stmt->column_int64(0),
          .entity_kind = std::string{stmt->column_text(1)},
          .entity_id   = stmt->column_int64(2),
          .external_id = std::string{stmt->column_text(3)},
      });
    }
  }

  for (auto const& r : rows) {
    auto const ctx_json =
        std::format(R"({{"old_strategy":"{}","new_strategy":"{}","external_id":"{}","entity_kind":"{}","entity_id":{}}})",
                    old_strategy, new_strategy, r.external_id, r.entity_kind, r.entity_id);

    auto tx = conn.begin_transaction(db::lock_mode::immediate);
    if (!tx) {
      return std::unexpected(link_error::query_failed);
    }

    {
      auto event = conn.prepare("insert into sync_events (link_id, direction, outcome, context_json) "
                                "values (?, 'push', 'strategy-abandoned', ?)");
      if (!event || !event->bind_int64(1, r.id) || !event->bind_text(2, ctx_json)) {
        return std::unexpected(link_error::query_failed);
      }
      if (!event->step()) {
        return std::unexpected(link_error::query_failed);
      }
    }
    {
      auto del = conn.prepare("delete from external_links where id = ?");
      if (!del || !del->bind_int64(1, r.id)) {
        return std::unexpected(link_error::query_failed);
      }
      if (!del->step()) {
        return std::unexpected(link_error::query_failed);
      }
    }

    if (!tx->commit()) {
      return std::unexpected(link_error::query_failed);
    }
  }

  return rows.size();
}

} // namespace planar::engine::external::link
