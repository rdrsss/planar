/// @file audit.cpp
/// @brief Implementation of `planar.policy.audit` (see audit.cppm).

module;

module planar.policy.audit;

import std;
import planar.db;

namespace planar::policy::audit {

namespace {

constexpr std::string_view k_insert_sql = "insert into audit_log (verb, entity_kind, entity_id, actor, scope, summary)"
                                          " values (?, ?, ?, ?, ?, ?)";

/// @brief Bind one optional text parameter, using SQL NULL when unset.
///
/// `bind_text` maps an empty `string_view` to the empty STRING, not NULL
/// (db.cppm documents this), so an unset optional has to route through
/// `bind_null` explicitly. Getting this wrong would store `''` where the
/// oracle stores NULL — a difference no exit code or stdout byte shows,
/// and one the `ix_audit_log_scope` partial index (`where scope is not
/// null`) would then silently start including.
/// @param stmt The prepared statement.
/// @param index The 1-based parameter index.
/// @param value The value, or `std::nullopt` for SQL NULL.
/// @return True when the bind succeeded.
auto bind_opt_text(planar::db::statement& stmt, int index, std::optional<std::string_view> value) -> bool {
  if (value.has_value()) {
    return stmt.bind_text(index, *value).has_value();
  }
  return stmt.bind_null(index).has_value();
}

} // namespace

auto verb_to_text(verb v) -> std::string_view {
  switch (v) {
  case verb::create:
    return "create";
  case verb::update:
    return "update";
  case verb::delete_:
    return "delete";
  case verb::status_change:
    return "status_change";
  case verb::link:
    return "link";
  case verb::unlink:
    return "unlink";
  }
  return "create";
}

auto record(db::connection& conn, const record_args& args) -> std::expected<void, audit_error> {
  auto stmt = conn.prepare(k_insert_sql);
  if (!stmt) {
    return std::unexpected(audit_error::write_failed);
  }
  const bool bound = stmt->bind_text(1, verb_to_text(args.verb)).has_value() &&
                     stmt->bind_text(2, args.entity.kind).has_value() && stmt->bind_int64(3, args.entity.id).has_value() &&
                     bind_opt_text(*stmt, 4, args.actor) && bind_opt_text(*stmt, 5, args.scope) &&
                     bind_opt_text(*stmt, 6, args.summary);
  if (!bound) {
    return std::unexpected(audit_error::write_failed);
  }
  auto step = stmt->step();
  if (!step) {
    return std::unexpected(audit_error::write_failed);
  }
  return {};
}

} // namespace planar::policy::audit
