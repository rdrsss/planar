/// @file snapshot.cpp
/// @brief Implementation of `planar.engine.runtime.snapshot` (plan 996,
/// task 6094). See snapshot.cppm for scope and the NULL-vs-empty contract.

module planar.engine.runtime.snapshot;

import std;
import planar.db;
import planar.policy;

namespace planar::engine::runtime::snapshot {

namespace audit = planar::policy::audit;

namespace {

// `coalesce(body, '')` / `coalesce(next_action, '')` are the Zig
// original's own SELECT list, not a convenience added by this port: the
// insert stores SQL NULL for an empty value and the read normalizes it
// back to `""`.
constexpr std::string_view k_select_columns = "select id, session_id, task_id, vendor, vendor_session_id, "
                                              "coalesce(body, ''), coalesce(next_action, ''), created_at "
                                              "from context_snapshots";

auto read_row(const db::statement& stmt) -> snapshot {
  snapshot out{
      .id                = stmt.column_int64(0),
      .session_id        = stmt.column_int64(1),
      .task_id           = std::nullopt,
      .vendor            = stmt.column_text(3),
      .vendor_session_id = std::nullopt,
      .body              = stmt.column_text(5),
      .next_action       = stmt.column_text(6),
      .created_at        = stmt.column_text(7),
  };
  if (!stmt.is_null(2)) {
    out.task_id = stmt.column_int64(2);
  }
  if (!stmt.is_null(4)) {
    out.vendor_session_id = stmt.column_text(4);
  }
  return out;
}

/// @brief Bind an optional text value the way the Zig insert does: unset
/// OR empty binds SQL NULL, anything else binds the text.
auto bind_text_or_null(db::statement& stmt, int index, std::optional<std::string_view> value) -> bool {
  if (value.has_value() && !value->empty()) {
    return stmt.bind_text(index, *value).has_value();
  }
  return stmt.bind_null(index).has_value();
}

} // namespace

auto show(db::connection& conn, std::int64_t id) -> std::expected<snapshot, snapshot_error> {
  auto stmt = conn.prepare(std::format("{} where id = ?", k_select_columns));
  if (!stmt) {
    return std::unexpected(snapshot_error::query_failed);
  }
  if (auto b = stmt->bind_int64(1, id); !b) {
    return std::unexpected(snapshot_error::query_failed);
  }
  auto step = stmt->step();
  if (!step) {
    return std::unexpected(snapshot_error::query_failed);
  }
  if (*step != db::step_result::row) {
    return std::unexpected(snapshot_error::not_found);
  }
  return read_row(*stmt);
}

auto create(db::connection& conn, const create_args& args) -> std::expected<snapshot, snapshot_error> {
  auto stmt = conn.prepare("insert into context_snapshots "
                           "(session_id, task_id, vendor, vendor_session_id, body, next_action) "
                           "values (?, ?, ?, ?, ?, ?) returning id");
  if (!stmt) {
    return std::unexpected(snapshot_error::query_failed);
  }
  if (auto b = stmt->bind_int64(1, args.session_id); !b) {
    return std::unexpected(snapshot_error::query_failed);
  }
  auto b2 = args.task_id.has_value() ? stmt->bind_int64(2, *args.task_id) : stmt->bind_null(2);
  if (!b2) {
    return std::unexpected(snapshot_error::query_failed);
  }
  if (auto b = stmt->bind_text(3, args.vendor); !b) {
    return std::unexpected(snapshot_error::query_failed);
  }
  auto b4 = args.vendor_session_id.has_value() ? stmt->bind_text(4, *args.vendor_session_id) : stmt->bind_null(4);
  if (!b4) {
    return std::unexpected(snapshot_error::query_failed);
  }
  if (!bind_text_or_null(*stmt, 5, args.body)) {
    return std::unexpected(snapshot_error::query_failed);
  }
  if (!bind_text_or_null(*stmt, 6, args.next_action)) {
    return std::unexpected(snapshot_error::query_failed);
  }
  auto step = stmt->step();
  if (!step || *step != db::step_result::row) {
    return std::unexpected(snapshot_error::query_failed);
  }
  const auto id = stmt->column_int64(0);
  if (auto recorded = audit::record(
          conn, audit::record_args{.verb    = audit::verb::create,
                                   .entity  = {.kind = "context_snapshot", .id = id},
                                   .summary = std::format("create snapshot session={} vendor={}", args.session_id, args.vendor)});
      !recorded) {
    return std::unexpected(snapshot_error::query_failed);
  }
  return show(conn, id);
}

auto get_latest_for_task(db::connection& conn, std::int64_t task_id) -> std::expected<std::optional<snapshot>, snapshot_error> {
  auto stmt = conn.prepare(std::format("{} where task_id = ? order by created_at desc, id desc limit 1", k_select_columns));
  if (!stmt) {
    return std::unexpected(snapshot_error::query_failed);
  }
  if (auto b = stmt->bind_int64(1, task_id); !b) {
    return std::unexpected(snapshot_error::query_failed);
  }
  auto step = stmt->step();
  if (!step) {
    return std::unexpected(snapshot_error::query_failed);
  }
  if (*step != db::step_result::row) {
    return std::optional<snapshot>{};
  }
  return std::optional<snapshot>{read_row(*stmt)};
}

auto list_for_task(db::connection& conn, std::int64_t task_id) -> std::expected<std::vector<snapshot>, snapshot_error> {
  auto stmt = conn.prepare(std::format("{} where task_id = ? order by created_at desc, id desc", k_select_columns));
  if (!stmt) {
    return std::unexpected(snapshot_error::query_failed);
  }
  if (auto b = stmt->bind_int64(1, task_id); !b) {
    return std::unexpected(snapshot_error::query_failed);
  }
  std::vector<snapshot> out;
  while (true) {
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(snapshot_error::query_failed);
    }
    if (*step != db::step_result::row) {
      break;
    }
    out.push_back(read_row(*stmt));
  }
  return out;
}

} // namespace planar::engine::runtime::snapshot
