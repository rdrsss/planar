/// @file handoff.cpp
/// @brief Implementation of `planar.engine.runtime.handoff` (plan 996,
/// task 6040). See handoff.cppm for the injected-guard argument and the
/// oracle-exact field order.

module planar.engine.runtime.handoff;

import std;
import planar.db;
import planar.json_text;
import planar.policy;

namespace planar::engine::runtime::handoff {

namespace audit = planar::policy::audit;

namespace {

/// @brief Bind an optional text parameter the way the Zig insert does:
/// unset OR EMPTY binds SQL NULL, anything else binds the text.
auto bind_text_or_null(db::statement& stmt, int index, std::optional<std::string_view> value) -> bool {
  if (value.has_value() && !value->empty()) {
    return stmt.bind_text(index, *value).has_value();
  }
  return stmt.bind_null(index).has_value();
}

constexpr std::string_view k_columns = "id, from_snapshot_id, to_session_id, from_vendor, to_vendor, "
                                       "status, validated_at, consumed_at, created_at, "
                                       "worktree_path, repo_root, branch";

constexpr std::string_view k_columns_qualified = "h.id, h.from_snapshot_id, h.to_session_id, h.from_vendor, h.to_vendor, "
                                                 "h.status, h.validated_at, h.consumed_at, h.created_at, "
                                                 "h.worktree_path, h.repo_root, h.branch";

/// @brief Read one row in `k_columns` order.
/// @return The row, or unset when its `status` text is not one of the four
/// CHECK-constrained values — which the caller maps to `query_failed`, the
/// Zig original's own `orelse return Error.QueryFailed`.
auto read_row(const db::statement& stmt) -> std::optional<handoff> {
  auto const parsed = status_from_text(stmt.column_text(5));
  if (!parsed) {
    return std::nullopt;
  }
  handoff out{
      .id               = stmt.column_int64(0),
      .from_snapshot_id = stmt.column_int64(1),
      .to_session_id    = std::nullopt,
      .from_vendor      = stmt.column_text(3),
      .to_vendor        = std::nullopt,
      .state            = *parsed,
      .validated_at     = std::nullopt,
      .consumed_at      = std::nullopt,
      .created_at       = stmt.column_text(8),
      .worktree_path    = std::nullopt,
      .repo_root        = std::nullopt,
      .branch           = std::nullopt,
  };
  if (!stmt.is_null(2)) {
    out.to_session_id = stmt.column_int64(2);
  }
  if (!stmt.is_null(4)) {
    out.to_vendor = stmt.column_text(4);
  }
  if (!stmt.is_null(6)) {
    out.validated_at = stmt.column_text(6);
  }
  if (!stmt.is_null(7)) {
    out.consumed_at = stmt.column_text(7);
  }
  if (!stmt.is_null(9)) {
    out.worktree_path = stmt.column_text(9);
  }
  if (!stmt.is_null(10)) {
    out.repo_root = stmt.column_text(10);
  }
  if (!stmt.is_null(11)) {
    out.branch = stmt.column_text(11);
  }
  return out;
}

/// @brief The shared body of `validate` / `consume` / `abandon`: read the
/// current row, run the INJECTED guard, then apply `update_sql`.
///
/// The read-guard-write order is the point, and it is why the guard is a
/// parameter: a test passes a recording probe and observes that the check
/// happened before the UPDATE rather than after it.
auto guarded_update(db::connection& conn, std::int64_t id, status target, const transition_check& allowed,
                    std::string_view update_sql, std::optional<std::int64_t> extra_int, std::string summary)
    -> std::expected<handoff, handoff_error> {
  auto const current = show(conn, id);
  if (!current) {
    return std::unexpected(current.error());
  }
  if (!allowed(current->state, target)) {
    return std::unexpected(handoff_error::illegal_transition);
  }

  auto stmt = conn.prepare(update_sql);
  if (!stmt) {
    return std::unexpected(handoff_error::query_failed);
  }
  int index = 1;
  if (extra_int.has_value()) {
    if (auto bound = stmt->bind_int64(index, *extra_int); !bound) {
      return std::unexpected(handoff_error::query_failed);
    }
    ++index;
  }
  if (auto bound = stmt->bind_int64(index, id); !bound) {
    return std::unexpected(handoff_error::query_failed);
  }
  if (auto stepped = stmt->step(); !stepped) {
    return std::unexpected(handoff_error::query_failed);
  }
  if (auto recorded = audit::record(
          conn,
          audit::record_args{.verb = audit::verb::status_change, .entity = {.kind = "handoff", .id = id}, .summary = summary});
      !recorded) {
    return std::unexpected(handoff_error::query_failed);
  }
  return show(conn, id);
}

} // namespace

auto status_from_text(std::string_view text) -> std::optional<status> {
  if (text == "pending") {
    return status::pending;
  }
  if (text == "validated") {
    return status::validated;
  }
  if (text == "consumed") {
    return status::consumed;
  }
  if (text == "abandoned") {
    return status::abandoned;
  }
  return std::nullopt;
}

auto to_text(status value) -> std::string_view {
  switch (value) {
  case status::pending:
    return "pending";
  case status::validated:
    return "validated";
  case status::consumed:
    return "consumed";
  case status::abandoned:
    return "abandoned";
  }
  return "pending";
}

auto is_terminal(status value) -> bool {
  return value == status::consumed || value == status::abandoned;
}

auto create(db::connection& conn, const create_args& args) -> std::expected<handoff, handoff_error> {
  auto stmt = conn.prepare("insert into handoffs (from_snapshot_id, from_vendor, to_vendor, status, "
                           "                      worktree_path, repo_root, branch) "
                           "values (?, ?, ?, 'pending', ?, ?, ?) returning id");
  if (!stmt) {
    return std::unexpected(handoff_error::query_failed);
  }
  if (auto bound = stmt->bind_int64(1, args.from_snapshot_id); !bound) {
    return std::unexpected(handoff_error::query_failed);
  }
  if (auto bound = stmt->bind_text(2, args.from_vendor); !bound) {
    return std::unexpected(handoff_error::query_failed);
  }
  if (!bind_text_or_null(*stmt, 3, args.to_vendor)) {
    return std::unexpected(handoff_error::query_failed);
  }
  if (!bind_text_or_null(*stmt, 4, args.worktree_path)) {
    return std::unexpected(handoff_error::query_failed);
  }
  if (!bind_text_or_null(*stmt, 5, args.repo_root)) {
    return std::unexpected(handoff_error::query_failed);
  }
  if (!bind_text_or_null(*stmt, 6, args.branch)) {
    return std::unexpected(handoff_error::query_failed);
  }
  auto stepped = stmt->step();
  if (!stepped || *stepped != db::step_result::row) {
    return std::unexpected(handoff_error::query_failed);
  }
  const auto id = stmt->column_int64(0);
  if (auto recorded = audit::record(conn, audit::record_args{.verb    = audit::verb::create,
                                                             .entity  = {.kind = "handoff", .id = id},
                                                             .summary = std::format("create handoff snapshot={} from={}",
                                                                                    args.from_snapshot_id, args.from_vendor)});
      !recorded) {
    return std::unexpected(handoff_error::query_failed);
  }
  return show(conn, id);
}

auto show(db::connection& conn, std::int64_t id) -> std::expected<handoff, handoff_error> {
  auto stmt = conn.prepare(std::format("select {} from handoffs where id = ?", k_columns));
  if (!stmt) {
    return std::unexpected(handoff_error::query_failed);
  }
  if (auto bound = stmt->bind_int64(1, id); !bound) {
    return std::unexpected(handoff_error::query_failed);
  }
  auto stepped = stmt->step();
  if (!stepped) {
    return std::unexpected(handoff_error::query_failed);
  }
  if (*stepped != db::step_result::row) {
    return std::unexpected(handoff_error::not_found);
  }
  auto row = read_row(*stmt);
  if (!row) {
    return std::unexpected(handoff_error::query_failed);
  }
  return *row;
}

auto validate(db::connection& conn, std::int64_t id, const transition_check& allowed) -> std::expected<handoff, handoff_error> {
  return guarded_update(conn, id, status::validated, allowed,
                        "update handoffs set status = 'validated', "
                        "    validated_at = strftime('%Y-%m-%dT%H:%M:%fZ','now') "
                        "where id = ?",
                        std::nullopt, std::format("validate handoff id={}", id));
}

auto consume(db::connection& conn, std::int64_t id, std::optional<std::int64_t> session_id, const transition_check& allowed)
    -> std::expected<handoff, handoff_error> {
  // Two statements, not one with a conditional binding: an absent
  // `--session` must leave `to_session_id` UNTOUCHED, not overwrite it
  // with NULL. The Zig original branches the same way.
  if (session_id.has_value()) {
    return guarded_update(conn, id, status::consumed, allowed,
                          "update handoffs set status = 'consumed', "
                          "    to_session_id = ?, "
                          "    consumed_at = strftime('%Y-%m-%dT%H:%M:%fZ','now') "
                          "where id = ?",
                          session_id, std::format("consume handoff id={}", id));
  }
  return guarded_update(conn, id, status::consumed, allowed,
                        "update handoffs set status = 'consumed', "
                        "    consumed_at = strftime('%Y-%m-%dT%H:%M:%fZ','now') "
                        "where id = ?",
                        std::nullopt, std::format("consume handoff id={}", id));
}

auto abandon(db::connection& conn, std::int64_t id, const transition_check& allowed, std::optional<std::string_view> reason)
    -> std::expected<handoff, handoff_error> {
  // Only `status` moves. `validated_at` survives, which the oracle's own
  // `abandon --json` on a validated handoff shows.
  const auto summary = reason.has_value() && !reason->empty() ? std::format("abandon handoff id={} reason={}", id, *reason)
                                                              : std::format("abandon handoff id={}", id);
  return guarded_update(conn, id, status::abandoned, allowed, "update handoffs set status = 'abandoned' where id = ?",
                        std::nullopt, summary);
}

auto list(db::connection& conn, const list_filter& filter) -> std::expected<std::vector<handoff>, handoff_error> {
  std::string sql = std::format("select {} from handoffs h", k_columns_qualified);
  if (filter.task_id.has_value()) {
    sql += " join context_snapshots cs on cs.id = h.from_snapshot_id where cs.task_id = ?";
  } else {
    sql += " where 1=1";
  }
  if (!filter.statuses.empty()) {
    sql += " and h.status in (";
    for (std::size_t i = 0; i < filter.statuses.size(); ++i) {
      sql += (i > 0) ? ",?" : "?";
    }
    sql += ")";
  } else {
    // The Go-parity default. See `list_filter::statuses`.
    sql += " and h.status = 'pending'";
  }
  // `created_at` alone, with no id tiebreak — see this function's contract.
  sql += " order by h.created_at desc";

  auto stmt = conn.prepare(sql);
  if (!stmt) {
    return std::unexpected(handoff_error::query_failed);
  }
  int index = 1;
  if (filter.task_id.has_value()) {
    if (auto bound = stmt->bind_int64(index, *filter.task_id); !bound) {
      return std::unexpected(handoff_error::query_failed);
    }
    ++index;
  }
  for (auto const state : filter.statuses) {
    if (auto bound = stmt->bind_text(index, to_text(state)); !bound) {
      return std::unexpected(handoff_error::query_failed);
    }
    ++index;
  }

  std::vector<handoff> out;
  while (true) {
    auto stepped = stmt->step();
    if (!stepped) {
      return std::unexpected(handoff_error::query_failed);
    }
    if (*stepped != db::step_result::row) {
      break;
    }
    auto row = read_row(*stmt);
    if (!row) {
      return std::unexpected(handoff_error::query_failed);
    }
    out.push_back(std::move(*row));
  }
  return out;
}

auto get_latest_with_worktree_for_task(db::connection& conn, std::int64_t task_id)
    -> std::expected<std::optional<handoff>, handoff_error> {
  auto stmt = conn.prepare(std::format("select {} from handoffs h "
                                       "join context_snapshots cs on cs.id = h.from_snapshot_id "
                                       "where cs.task_id = ? "
                                       "  and h.worktree_path is not null "
                                       "  and h.status != 'abandoned' "
                                       "order by h.created_at desc, h.id desc limit 1",
                                       k_columns_qualified));
  if (!stmt) {
    return std::unexpected(handoff_error::query_failed);
  }
  if (auto bound = stmt->bind_int64(1, task_id); !bound) {
    return std::unexpected(handoff_error::query_failed);
  }
  auto stepped = stmt->step();
  if (!stepped) {
    return std::unexpected(handoff_error::query_failed);
  }
  if (*stepped != db::step_result::row) {
    return std::optional<handoff>{};
  }
  auto row = read_row(*stmt);
  if (!row) {
    return std::unexpected(handoff_error::query_failed);
  }
  return std::optional<handoff>{std::move(*row)};
}

auto get_pending_for_snapshot(db::connection& conn, std::int64_t snapshot_id)
    -> std::expected<std::optional<handoff>, handoff_error> {
  auto stmt = conn.prepare(std::format("select {} from handoffs "
                                       "where from_snapshot_id = ? and status in ('pending','validated') "
                                       "order by created_at desc limit 1",
                                       k_columns));
  if (!stmt) {
    return std::unexpected(handoff_error::query_failed);
  }
  if (auto bound = stmt->bind_int64(1, snapshot_id); !bound) {
    return std::unexpected(handoff_error::query_failed);
  }
  auto stepped = stmt->step();
  if (!stepped) {
    return std::unexpected(handoff_error::query_failed);
  }
  if (*stepped != db::step_result::row) {
    return std::optional<handoff>{};
  }
  auto row = read_row(*stmt);
  if (!row) {
    return std::unexpected(handoff_error::query_failed);
  }
  return std::optional<handoff>{std::move(*row)};
}

auto render_json(const handoff& value) -> std::string {
  std::string out = std::format("{{\"id\":{},\"from_snapshot_id\":{},\"from_vendor\":", value.id, value.from_snapshot_id);
  json_text::append_json_string(out, value.from_vendor);
  out += ",\"status\":";
  json_text::append_json_string(out, to_text(value.state));
  out += ",\"created_at\":";
  json_text::append_json_string(out, value.created_at);
  // Optionals, in the oracle's order, OMITTED when unset.
  if (value.to_session_id.has_value()) {
    out += std::format(",\"to_session_id\":{}", *value.to_session_id);
  }
  auto const optional_field = [&out](std::string_view key, const std::optional<std::string>& field) {
    if (!field.has_value()) {
      return;
    }
    out += std::format(",\"{}\":", key);
    json_text::append_json_string(out, *field);
  };
  optional_field("to_vendor", value.to_vendor);
  optional_field("validated_at", value.validated_at);
  optional_field("consumed_at", value.consumed_at);
  optional_field("worktree_path", value.worktree_path);
  optional_field("repo_root", value.repo_root);
  optional_field("branch", value.branch);
  out += "}";
  return out;
}

auto render_text(const handoff& value) -> std::string {
  std::string out = std::format("handoff {}  [{}]\n", value.id, to_text(value.state));
  out += std::format("  from_snapshot: {}\n", value.from_snapshot_id);
  out += std::format("  from_vendor:   {}\n", value.from_vendor);
  if (value.to_vendor.has_value()) {
    out += std::format("  to_vendor:     {}\n", *value.to_vendor);
  }
  if (value.to_session_id.has_value()) {
    out += std::format("  to_session:    {}\n", *value.to_session_id);
  }
  if (value.validated_at.has_value()) {
    out += std::format("  validated_at:  {}\n", *value.validated_at);
  }
  if (value.consumed_at.has_value()) {
    out += std::format("  consumed_at:   {}\n", *value.consumed_at);
  }
  out += std::format("  created_at:    {}\n", value.created_at);
  if (value.worktree_path.has_value()) {
    out += std::format("  worktree_path: {}\n", *value.worktree_path);
  }
  if (value.repo_root.has_value()) {
    out += std::format("  repo_root:     {}\n", *value.repo_root);
  }
  if (value.branch.has_value()) {
    out += std::format("  branch:        {}\n", *value.branch);
  }
  return out;
}

auto render_list_json(std::span<const handoff> items) -> std::string {
  // Zero handoffs => ZERO BYTES. Not `[]`, not a bare newline.
  std::string out;
  for (auto const& item : items) {
    out += render_json(item);
    out += "\n";
  }
  return out;
}

auto render_list_text(std::span<const handoff> items) -> std::string {
  if (items.empty()) {
    return "no handoffs\n";
  }
  // Column widths are the Zig format string's, exactly: {:<4} {:<8} {:<11}
  // {:<11} then the unpadded status, separated by two spaces each.
  std::string out = std::format("{:<4}  {:<8}  {:<11}  {:<11}  {}\n", "id", "snapshot", "from-vendor", "to-vendor", "status");
  for (auto const& item : items) {
    out += std::format("{:<4}  {:<8}  {:<11}  {:<11}  {}\n", item.id, item.from_snapshot_id, item.from_vendor,
                       item.to_vendor.value_or("-"), to_text(item.state));
  }
  return out;
}

} // namespace planar::engine::runtime::handoff
