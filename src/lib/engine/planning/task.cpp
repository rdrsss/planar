/// @file task.cpp
/// @brief Implementation of `planar.engine.planning.task` (see task.cppm).

module;

module planar.engine.planning.task;

import std;
import planar.db;
import planar.engine.planning.plan;
import planar.engine.planning.transitions;

namespace planar::engine::planning {

namespace {

constexpr int k_sqlite_constraint_unique = 2067; // SQLITE_CONSTRAINT_UNIQUE

auto is_unique_violation(const db::db_error& err) -> bool {
  return err.code_ == k_sqlite_constraint_unique;
}

auto scope_kind_to_text(task_scope_kind k) -> std::string_view {
  switch (k) {
  case task_scope_kind::global:
    return "global";
  case task_scope_kind::association:
    return "association";
  case task_scope_kind::repo:
    return "repo";
  }
  return "global"; // unreachable
}

/// @brief Strip an optional leading `"assoc:"` prefix. Mirrors
/// `planar.engine.identity.scope`'s (module-private) `normalize_assoc` —
/// duplicated here (also duplicated in plan.cpp) rather than imported;
/// see task.cppm / plan.cppm's CMakeLists.txt for why a dependency on
/// `engine_identity` is not available at this layer.
auto normalize_assoc(std::string_view scope) -> std::string_view {
  constexpr std::string_view prefix = "assoc:";
  if (scope.starts_with(prefix)) {
    return scope.substr(prefix.size());
  }
  return scope;
}

/// @brief Resolve `scope` (when present) to a `(kind,id)` pair, else
/// global. Mirrors `planar.engine.identity.scope`'s `resolve_slug`
/// algorithm at the SQL level — see this module's CMakeLists.txt for why
/// this is a local duplication rather than a call into `engine_identity`.
auto resolve_scope_or_global(db::connection& conn, const std::optional<std::string>& scope)
    -> std::expected<std::pair<task_scope_kind, std::optional<std::int64_t>>, task_error> {
  if (!scope.has_value()) {
    return std::make_pair(task_scope_kind::global, std::optional<std::int64_t>{});
  }
  const std::string_view s = *scope;
  if (s == "global") {
    return std::make_pair(task_scope_kind::global, std::optional<std::int64_t>{});
  }

  constexpr std::string_view repo_prefix = "repo:";
  if (s.starts_with(repo_prefix)) {
    const auto bare_repo = s.substr(repo_prefix.size());
    if (bare_repo.empty()) {
      return std::unexpected(task_error::slug_not_found);
    }
    auto stmt = conn.prepare("select id from projects where slug = ?");
    if (!stmt) {
      return std::unexpected(task_error::query_failed);
    }
    if (auto bound = stmt->bind_text(1, bare_repo); !bound) {
      return std::unexpected(task_error::query_failed);
    }
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(task_error::query_failed);
    }
    if (*step != db::step_result::row) {
      return std::unexpected(task_error::slug_not_found);
    }
    return std::make_pair(task_scope_kind::repo, std::optional<std::int64_t>{stmt->column_int64(0)});
  }

  const auto bare = normalize_assoc(s);
  if (bare.empty()) {
    return std::unexpected(task_error::slug_not_found);
  }
  auto stmt = conn.prepare("select id from associations where slug = ?");
  if (!stmt) {
    return std::unexpected(task_error::query_failed);
  }
  if (auto bound = stmt->bind_text(1, bare); !bound) {
    return std::unexpected(task_error::query_failed);
  }
  auto step = stmt->step();
  if (!step) {
    return std::unexpected(task_error::query_failed);
  }
  if (*step != db::step_result::row) {
    return std::unexpected(task_error::slug_not_found);
  }
  return std::make_pair(task_scope_kind::association, std::optional<std::int64_t>{stmt->column_int64(0)});
}

auto map_plan_error(plan_error e) -> task_error {
  switch (e) {
  case plan_error::not_found:
    return task_error::not_found;
  case plan_error::slug_conflict:
    return task_error::slug_conflict;
  case plan_error::slug_not_found:
    return task_error::slug_not_found;
  case plan_error::invalid_parent_cycle:
  case plan_error::illegal_transition:
  case plan_error::unknown_status:
  case plan_error::query_failed:
    return task_error::query_failed;
  }
  return task_error::query_failed;
}

constexpr std::string_view k_select_columns =
    "select id, scope_kind, scope_id, plan_id, parent_task_id, title, body, slug, status, priority, "
    "next_action, due_at, created_at, updated_at from tasks";

auto read_row(db::statement& stmt) -> std::expected<task, task_error> {
  const auto      scope_kind_text = stmt.column_text(1);
  task_scope_kind sk;
  if (scope_kind_text == "global") {
    sk = task_scope_kind::global;
  } else if (scope_kind_text == "association") {
    sk = task_scope_kind::association;
  } else if (scope_kind_text == "repo") {
    sk = task_scope_kind::repo;
  } else {
    return std::unexpected(task_error::query_failed);
  }

  const auto status_text = stmt.column_text(8);
  const auto status      = task_status_from_text(status_text);
  if (!status.has_value()) {
    return std::unexpected(task_error::query_failed);
  }

  return task{
      .id             = stmt.column_int64(0),
      .scope_kind     = sk,
      .scope_id       = stmt.is_null(2) ? std::optional<std::int64_t>{} : std::optional<std::int64_t>{stmt.column_int64(2)},
      .plan_id        = stmt.is_null(3) ? std::optional<std::int64_t>{} : std::optional<std::int64_t>{stmt.column_int64(3)},
      .parent_task_id = stmt.is_null(4) ? std::optional<std::int64_t>{} : std::optional<std::int64_t>{stmt.column_int64(4)},
      .title          = stmt.column_text(5),
      .body           = stmt.is_null(6) ? std::optional<std::string>{} : std::optional<std::string>{stmt.column_text(6)},
      .slug           = stmt.is_null(7) ? std::optional<std::string>{} : std::optional<std::string>{stmt.column_text(7)},
      .status         = *status,
      .priority       = stmt.column_int64(9),
      .next_action    = stmt.is_null(10) ? std::optional<std::string>{} : std::optional<std::string>{stmt.column_text(10)},
      .due_at         = stmt.is_null(11) ? std::optional<std::string>{} : std::optional<std::string>{stmt.column_text(11)},
      .created_at     = stmt.column_text(12),
      .updated_at     = stmt.column_text(13),
  };
}

/// @brief Insert a `task_reopens` row. Only called when the from-status
/// is terminal (done/cancelled) and the to-status is an open status
/// (todo/doing/blocked) — mirrors zig's guard at both call sites.
auto record_reopen(db::connection& conn, std::int64_t task_id, task_status from, task_status to, std::string_view source,
                   std::optional<std::string_view> reason) -> std::expected<void, task_error> {
  auto stmt = conn.prepare("insert into task_reopens (task_id, from_status, to_status, source, reason) "
                           "values (?, ?, ?, ?, ?)");
  if (!stmt) {
    return std::unexpected(task_error::query_failed);
  }
  if (auto b = stmt->bind_int64(1, task_id); !b) {
    return std::unexpected(task_error::query_failed);
  }
  if (auto b = stmt->bind_text(2, task_status_to_text(from)); !b) {
    return std::unexpected(task_error::query_failed);
  }
  if (auto b = stmt->bind_text(3, task_status_to_text(to)); !b) {
    return std::unexpected(task_error::query_failed);
  }
  if (auto b = stmt->bind_text(4, source); !b) {
    return std::unexpected(task_error::query_failed);
  }
  auto b5 = reason.has_value() ? stmt->bind_text(5, *reason) : stmt->bind_null(5);
  if (!b5) {
    return std::unexpected(task_error::query_failed);
  }
  auto step = stmt->step();
  if (!step) {
    return std::unexpected(task_error::query_failed);
  }
  return {};
}

auto recompute_plan(db::connection& conn, std::int64_t plan_id) -> std::expected<void, task_error> {
  auto result = recompute_status(conn, plan_id);
  if (!result) {
    return std::unexpected(map_plan_error(result.error()));
  }
  return {};
}

auto set_status(db::connection& conn, std::int64_t id, task_status status) -> std::expected<void, task_error> {
  auto stmt = conn.prepare("update tasks set status = ?, updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') where id = ?");
  if (!stmt) {
    return std::unexpected(task_error::query_failed);
  }
  if (auto b = stmt->bind_text(1, task_status_to_text(status)); !b) {
    return std::unexpected(task_error::query_failed);
  }
  if (auto b = stmt->bind_int64(2, id); !b) {
    return std::unexpected(task_error::query_failed);
  }
  auto step = stmt->step();
  if (!step) {
    return std::unexpected(task_error::query_failed);
  }
  return {};
}

auto is_terminal(task_status s) -> bool {
  return s == task_status::done || s == task_status::cancelled;
}
auto is_open(task_status s) -> bool {
  return s == task_status::todo || s == task_status::doing || s == task_status::blocked;
}

} // namespace

auto task_status_from_text(std::string_view s) -> std::optional<task_status> {
  if (s == "todo") {
    return task_status::todo;
  }
  if (s == "doing") {
    return task_status::doing;
  }
  if (s == "blocked") {
    return task_status::blocked;
  }
  if (s == "done") {
    return task_status::done;
  }
  if (s == "cancelled") {
    return task_status::cancelled;
  }
  return std::nullopt;
}

auto task_status_to_text(task_status s) -> std::string_view {
  switch (s) {
  case task_status::todo:
    return "todo";
  case task_status::doing:
    return "doing";
  case task_status::blocked:
    return "blocked";
  case task_status::done:
    return "done";
  case task_status::cancelled:
    return "cancelled";
  }
  return "todo"; // unreachable
}

auto create_task(db::connection& conn, const task_create_args& args) -> std::expected<task, task_error> {
  auto scope_ref = resolve_scope_or_global(conn, args.scope);
  if (!scope_ref) {
    return std::unexpected(scope_ref.error());
  }

  auto stmt = conn.prepare("insert into tasks (scope_kind, scope_id, plan_id, parent_task_id, title, body, slug, "
                           "status, priority, next_action, due_at) values (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
                           "returning id");
  if (!stmt) {
    return std::unexpected(task_error::query_failed);
  }
  if (auto b = stmt->bind_text(1, scope_kind_to_text(scope_ref->first)); !b) {
    return std::unexpected(task_error::query_failed);
  }
  auto b2 = scope_ref->second.has_value() ? stmt->bind_int64(2, *scope_ref->second) : stmt->bind_null(2);
  if (!b2) {
    return std::unexpected(task_error::query_failed);
  }
  auto b3 = args.plan_id.has_value() ? stmt->bind_int64(3, *args.plan_id) : stmt->bind_null(3);
  if (!b3) {
    return std::unexpected(task_error::query_failed);
  }
  auto b4 = args.parent_task_id.has_value() ? stmt->bind_int64(4, *args.parent_task_id) : stmt->bind_null(4);
  if (!b4) {
    return std::unexpected(task_error::query_failed);
  }
  if (auto b = stmt->bind_text(5, args.title); !b) {
    return std::unexpected(task_error::query_failed);
  }
  auto b6 = args.body.has_value() ? stmt->bind_text(6, *args.body) : stmt->bind_null(6);
  if (!b6) {
    return std::unexpected(task_error::query_failed);
  }
  auto b7 = args.slug.has_value() ? stmt->bind_text(7, *args.slug) : stmt->bind_null(7);
  if (!b7) {
    return std::unexpected(task_error::query_failed);
  }
  if (auto b = stmt->bind_text(8, task_status_to_text(args.status)); !b) {
    return std::unexpected(task_error::query_failed);
  }
  if (auto b = stmt->bind_int64(9, args.priority); !b) {
    return std::unexpected(task_error::query_failed);
  }
  auto b10 = args.next_action.has_value() ? stmt->bind_text(10, *args.next_action) : stmt->bind_null(10);
  if (!b10) {
    return std::unexpected(task_error::query_failed);
  }
  auto b11 = args.due_at.has_value() ? stmt->bind_text(11, *args.due_at) : stmt->bind_null(11);
  if (!b11) {
    return std::unexpected(task_error::query_failed);
  }

  auto step = stmt->step();
  if (!step) {
    if (is_unique_violation(step.error())) {
      return std::unexpected(task_error::slug_conflict);
    }
    return std::unexpected(task_error::query_failed);
  }
  const auto id = stmt->column_int64(0);

  if (!args.no_auto_promote && args.plan_id.has_value()) {
    if (auto r = recompute_plan(conn, *args.plan_id); !r) {
      return std::unexpected(r.error());
    }
  }

  return show_task(conn, id);
}

auto show_task(db::connection& conn, std::int64_t id) -> std::expected<task, task_error> {
  auto stmt = conn.prepare(std::format("{} where id = ?", k_select_columns));
  if (!stmt) {
    return std::unexpected(task_error::query_failed);
  }
  if (auto bound = stmt->bind_int64(1, id); !bound) {
    return std::unexpected(task_error::query_failed);
  }
  auto step = stmt->step();
  if (!step) {
    return std::unexpected(task_error::query_failed);
  }
  if (*step != db::step_result::row) {
    return std::unexpected(task_error::not_found);
  }
  return read_row(*stmt);
}

auto list_tasks(db::connection& conn, const task_list_filter& filter) -> std::expected<std::vector<task>, task_error> {
  std::optional<std::pair<task_scope_kind, std::optional<std::int64_t>>> scope_ref;
  if (filter.scope.has_value()) {
    auto resolved = resolve_scope_or_global(conn, filter.scope);
    if (!resolved) {
      return std::unexpected(resolved.error());
    }
    scope_ref = *resolved;
  }

  std::string sql(k_select_columns);
  sql += " where 1 = 1";
  if (filter.status.has_value()) {
    sql += " and status = ?";
  } else {
    sql += " and status in ('todo','doing','blocked')";
  }
  if (filter.plan_id.has_value()) {
    sql += " and plan_id = ?";
  }
  if (filter.priority_max.has_value()) {
    sql += " and priority <= ?";
  }
  if (scope_ref.has_value()) {
    sql += " and scope_kind = ?";
    if (scope_ref->second.has_value()) {
      sql += " and scope_id = ?";
    } else {
      sql += " and scope_id is null";
    }
  }
  sql += " order by priority, updated_at desc, id";

  auto stmt = conn.prepare(sql);
  if (!stmt) {
    return std::unexpected(task_error::query_failed);
  }
  int idx = 1;
  if (filter.status.has_value()) {
    if (auto b = stmt->bind_text(idx++, task_status_to_text(*filter.status)); !b) {
      return std::unexpected(task_error::query_failed);
    }
  }
  if (filter.plan_id.has_value()) {
    if (auto b = stmt->bind_int64(idx++, *filter.plan_id); !b) {
      return std::unexpected(task_error::query_failed);
    }
  }
  if (filter.priority_max.has_value()) {
    if (auto b = stmt->bind_int64(idx++, *filter.priority_max); !b) {
      return std::unexpected(task_error::query_failed);
    }
  }
  if (scope_ref.has_value()) {
    if (auto b = stmt->bind_text(idx++, scope_kind_to_text(scope_ref->first)); !b) {
      return std::unexpected(task_error::query_failed);
    }
    if (scope_ref->second.has_value()) {
      if (auto b = stmt->bind_int64(idx++, *scope_ref->second); !b) {
        return std::unexpected(task_error::query_failed);
      }
    }
  }

  std::vector<task> out;
  for (;;) {
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(task_error::query_failed);
    }
    if (*step == db::step_result::done) {
      break;
    }
    auto row = read_row(*stmt);
    if (!row) {
      return std::unexpected(row.error());
    }
    out.push_back(std::move(*row));
  }
  return out;
}

auto update_task(db::connection& conn, std::int64_t id, const task_update_args& patch) -> std::expected<task, task_error> {
  std::optional<std::pair<task_scope_kind, std::optional<std::int64_t>>> scope_ref;
  if (patch.scope.has_value()) {
    auto resolved = resolve_scope_or_global(conn, patch.scope);
    if (!resolved) {
      return std::unexpected(resolved.error());
    }
    scope_ref = *resolved;
  }

  auto tx = conn.begin_transaction();
  if (!tx) {
    return std::unexpected(task_error::query_failed);
  }

  auto current = show_task(conn, id);
  if (!current) {
    return std::unexpected(current.error());
  }

  if (patch.status.has_value()) {
    auto checked = check_transition(transition_kind::task, task_status_to_text(current->status),
                                    task_status_to_text(*patch.status), patch.force);
    if (!checked) {
      return std::unexpected(checked.error() == transition_error::unknown_status ? task_error::unknown_status
                                                                                 : task_error::illegal_transition);
    }
  }

  enum class param_kind : std::uint8_t { text, int64, null };
  std::string               sql = "update tasks set ";
  std::vector<param_kind>   order;
  std::vector<std::string>  text_params;
  std::vector<std::int64_t> int_params;
  bool                      first = true;
  auto                      sep   = [&] {
    if (!first) {
      sql += ", ";
    }
    first = false;
  };

  if (scope_ref.has_value()) {
    sep();
    sql += "scope_kind = ?";
    order.push_back(param_kind::text);
    text_params.push_back(std::string(scope_kind_to_text(scope_ref->first)));
    sep();
    sql += "scope_id = ?";
    if (scope_ref->second.has_value()) {
      order.push_back(param_kind::int64);
      int_params.push_back(*scope_ref->second);
    } else {
      order.push_back(param_kind::null);
    }
  }
  if (patch.title.has_value()) {
    sep();
    sql += "title = ?";
    order.push_back(param_kind::text);
    text_params.push_back(*patch.title);
  }
  if (patch.body.has_value()) {
    sep();
    sql += "body = ?";
    order.push_back(param_kind::text);
    text_params.push_back(*patch.body);
  }
  if (patch.status.has_value()) {
    sep();
    sql += "status = ?";
    order.push_back(param_kind::text);
    text_params.push_back(std::string(task_status_to_text(*patch.status)));
  }
  if (patch.priority.has_value()) {
    sep();
    sql += "priority = ?";
    order.push_back(param_kind::int64);
    int_params.push_back(*patch.priority);
  }
  if (patch.plan_id.has_value()) {
    sep();
    sql += "plan_id = ?";
    order.push_back(param_kind::int64);
    int_params.push_back(*patch.plan_id);
  } else if (patch.clear_plan) {
    sep();
    sql += "plan_id = null";
  }
  if (patch.next_action.has_value()) {
    sep();
    sql += "next_action = ?";
    order.push_back(param_kind::text);
    text_params.push_back(*patch.next_action);
  }
  if (patch.due_at.has_value()) {
    sep();
    sql += "due_at = ?";
    order.push_back(param_kind::text);
    text_params.push_back(*patch.due_at);
  }
  if (patch.slug.has_value()) {
    sep();
    sql += "slug = ?";
    order.push_back(param_kind::text);
    text_params.push_back(*patch.slug);
  }

  if (first) {
    return show_task(conn, id);
  }

  sep();
  sql += "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') where id = ?";

  auto stmt = conn.prepare(sql);
  if (!stmt) {
    return std::unexpected(task_error::query_failed);
  }
  int         bind_idx = 1;
  std::size_t text_i   = 0;
  std::size_t int_i    = 0;
  for (const auto k : order) {
    switch (k) {
    case param_kind::text:
      if (auto b = stmt->bind_text(bind_idx++, text_params[text_i++]); !b) {
        return std::unexpected(task_error::query_failed);
      }
      break;
    case param_kind::int64:
      if (auto b = stmt->bind_int64(bind_idx++, int_params[int_i++]); !b) {
        return std::unexpected(task_error::query_failed);
      }
      break;
    case param_kind::null:
      break;
    }
  }
  if (auto b = stmt->bind_int64(bind_idx++, id); !b) {
    return std::unexpected(task_error::query_failed);
  }

  auto step = stmt->step();
  if (!step) {
    if (is_unique_violation(step.error())) {
      return std::unexpected(task_error::slug_conflict);
    }
    return std::unexpected(task_error::query_failed);
  }

  if (patch.force && patch.status.has_value()) {
    if (is_terminal(current->status) && is_open(*patch.status)) {
      if (auto r = record_reopen(conn, id, current->status, *patch.status, "task-update-force", patch.reason); !r) {
        return std::unexpected(r.error());
      }
    }
  }

  auto updated = show_task(conn, id);
  if (!updated) {
    return std::unexpected(updated.error());
  }

  if (!patch.no_auto_promote) {
    if (current->plan_id.has_value()) {
      if (auto r = recompute_plan(conn, *current->plan_id); !r) {
        return std::unexpected(r.error());
      }
    }
    if (updated->plan_id.has_value() && (!current->plan_id.has_value() || *current->plan_id != *updated->plan_id)) {
      if (auto r = recompute_plan(conn, *updated->plan_id); !r) {
        return std::unexpected(r.error());
      }
    }
  }

  if (auto committed = tx->commit(); !committed) {
    return std::unexpected(task_error::query_failed);
  }
  return updated;
}

auto mark_done(db::connection& conn, std::int64_t id, bool force) -> std::expected<task, task_error> {
  auto tx = conn.begin_transaction();
  if (!tx) {
    return std::unexpected(task_error::query_failed);
  }
  auto current = show_task(conn, id);
  if (!current) {
    return std::unexpected(current.error());
  }
  auto checked = check_transition(transition_kind::task, task_status_to_text(current->status), "done", force);
  if (!checked) {
    return std::unexpected(checked.error() == transition_error::unknown_status ? task_error::unknown_status
                                                                               : task_error::illegal_transition);
  }
  if (auto r = set_status(conn, id, task_status::done); !r) {
    return std::unexpected(r.error());
  }
  auto updated = show_task(conn, id);
  if (!updated) {
    return std::unexpected(updated.error());
  }
  if (updated->plan_id.has_value()) {
    if (auto r = recompute_plan(conn, *updated->plan_id); !r) {
      return std::unexpected(r.error());
    }
  }
  if (auto committed = tx->commit(); !committed) {
    return std::unexpected(task_error::query_failed);
  }
  return updated;
}

auto mark_cancelled(db::connection& conn, std::int64_t id) -> std::expected<task, task_error> {
  auto tx = conn.begin_transaction();
  if (!tx) {
    return std::unexpected(task_error::query_failed);
  }
  auto current = show_task(conn, id);
  if (!current) {
    return std::unexpected(current.error());
  }
  auto checked = check_transition(transition_kind::task, task_status_to_text(current->status), "cancelled", false);
  if (!checked) {
    return std::unexpected(checked.error() == transition_error::unknown_status ? task_error::unknown_status
                                                                               : task_error::illegal_transition);
  }
  if (auto r = set_status(conn, id, task_status::cancelled); !r) {
    return std::unexpected(r.error());
  }
  auto updated = show_task(conn, id);
  if (!updated) {
    return std::unexpected(updated.error());
  }
  if (updated->plan_id.has_value()) {
    if (auto r = recompute_plan(conn, *updated->plan_id); !r) {
      return std::unexpected(r.error());
    }
  }
  if (auto committed = tx->commit(); !committed) {
    return std::unexpected(task_error::query_failed);
  }
  return updated;
}

auto mark_blocked(db::connection& conn, std::int64_t id, std::int64_t blocked_on_id, std::optional<std::string_view> reason,
                  bool force) -> std::expected<task, task_error> {
  (void)reason; // audit summary text not ported — see task.cppm file header.

  // Verify the blocker exists (mirrors zig: read-only, before the transaction).
  auto blocker = show_task(conn, blocked_on_id);
  if (!blocker) {
    return std::unexpected(blocker.error());
  }

  auto tx = conn.begin_transaction();
  if (!tx) {
    return std::unexpected(task_error::query_failed);
  }
  auto current = show_task(conn, id);
  if (!current) {
    return std::unexpected(current.error());
  }
  auto checked = check_transition(transition_kind::task, task_status_to_text(current->status), "blocked", force);
  if (!checked) {
    return std::unexpected(checked.error() == transition_error::unknown_status ? task_error::unknown_status
                                                                               : task_error::illegal_transition);
  }
  if (auto r = set_status(conn, id, task_status::blocked); !r) {
    return std::unexpected(r.error());
  }

  {
    auto stmt = conn.prepare("insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) "
                             "values ('task', ?, 'task', ?, 'depends-on')");
    if (!stmt) {
      return std::unexpected(task_error::query_failed);
    }
    if (auto b = stmt->bind_int64(1, id); !b) {
      return std::unexpected(task_error::query_failed);
    }
    if (auto b = stmt->bind_int64(2, blocked_on_id); !b) {
      return std::unexpected(task_error::query_failed);
    }
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(task_error::query_failed);
    }
  }

  auto updated = show_task(conn, id);
  if (!updated) {
    return std::unexpected(updated.error());
  }
  if (updated->plan_id.has_value()) {
    if (auto r = recompute_plan(conn, *updated->plan_id); !r) {
      return std::unexpected(r.error());
    }
  }
  if (auto committed = tx->commit(); !committed) {
    return std::unexpected(task_error::query_failed);
  }
  return updated;
}

auto reopen(db::connection& conn, std::int64_t id, task_status new_status, std::string_view reason)
    -> std::expected<task, task_error> {
  auto tx = conn.begin_transaction();
  if (!tx) {
    return std::unexpected(task_error::query_failed);
  }
  auto current = show_task(conn, id);
  if (!current) {
    return std::unexpected(current.error());
  }
  // `reopen` is the verb-gated escape from terminal status — bypasses the
  // matrix (force=true), mirrors zig task.zig:1149.
  auto checked =
      check_transition(transition_kind::task, task_status_to_text(current->status), task_status_to_text(new_status), true);
  if (!checked) {
    return std::unexpected(checked.error() == transition_error::unknown_status ? task_error::unknown_status
                                                                               : task_error::illegal_transition);
  }
  if (auto r = set_status(conn, id, new_status); !r) {
    return std::unexpected(r.error());
  }

  if (is_terminal(current->status) && is_open(new_status)) {
    if (auto r = record_reopen(conn, id, current->status, new_status, "task-reopen", reason); !r) {
      return std::unexpected(r.error());
    }
  }

  auto updated = show_task(conn, id);
  if (!updated) {
    return std::unexpected(updated.error());
  }
  if (updated->plan_id.has_value()) {
    if (auto r = recompute_plan(conn, *updated->plan_id); !r) {
      return std::unexpected(r.error());
    }
  }
  if (auto committed = tx->commit(); !committed) {
    return std::unexpected(task_error::query_failed);
  }
  return updated;
}

} // namespace planar::engine::planning
