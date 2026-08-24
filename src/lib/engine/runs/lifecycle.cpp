/// @file lifecycle.cpp
/// @brief Implementation of `planar.engine.runs.lifecycle` (plan 996, task
/// 6095). See lifecycle.cppm for scope, the oracle-derived semantics, and the
/// cut list.

module planar.engine.runs.lifecycle;

import std;
import planar.db;

namespace planar::engine::runs::lifecycle {

namespace {

// The two extended result codes zig's `Db.lastWasUniqueViolation` treats as a
// uniqueness conflict (zig/src/db/sqlite.zig:208). Both are checked here for
// the same reason: a UNIQUE index reports 2067, an INTEGER PRIMARY KEY
// collision reports 1555, and the caller-facing distinction (duplicate uid /
// duplicate seq) must not depend on which one SQLite picks.
constexpr int k_sqlite_constraint_unique      = 2067;
constexpr int k_sqlite_constraint_primary_key = 1555;

auto is_unique_violation(const db::db_error& err) -> bool {
  return err.code_ == k_sqlite_constraint_unique || err.code_ == k_sqlite_constraint_primary_key;
}

auto opt_text(const db::statement& stmt, int index) -> std::optional<std::string> {
  if (stmt.is_null(index)) {
    return std::nullopt;
  }
  return stmt.column_text(index);
}

constexpr std::string_view k_select_columns = "select id, run_uid, plan_id, arm, base_sha, config_hash, "
                                              "config_json, corpus_repo, status, started_at, ended_at from runs";

auto read_run(const db::statement& stmt) -> run {
  return run{
      .id          = stmt.column_int64(0),
      .run_uid     = stmt.column_text(1),
      .plan_id     = stmt.column_int64(2),
      .arm         = stmt.column_text(3),
      .base_sha    = stmt.column_text(4),
      .config_hash = stmt.column_text(5),
      .config_json = opt_text(stmt, 6),
      .corpus_repo = opt_text(stmt, 7),
      .status      = stmt.column_text(8),
      .started_at  = stmt.column_text(9),
      .ended_at    = opt_text(stmt, 10),
  };
}

/// @brief Bind an optional text parameter as text-or-NULL.
auto bind_opt_text(db::statement& stmt, int index, std::optional<std::string_view> value) -> bool {
  if (value.has_value()) {
    return stmt.bind_text(index, *value).has_value();
  }
  return stmt.bind_null(index).has_value();
}

/// @brief Step a statement once, mapping any SQLite failure to `query_failed`
/// and distinguishing a uniqueness conflict via `on_unique`.
auto step_write(db::statement& stmt, runs_error on_unique) -> std::expected<void, runs_error> {
  auto stepped = stmt.step();
  if (!stepped) {
    if (is_unique_violation(stepped.error())) {
      return std::unexpected(on_unique);
    }
    return std::unexpected(runs_error::query_failed);
  }
  return {};
}

} // namespace

auto touch_kind_from_text(std::string_view text) -> std::optional<touch_kind> {
  if (text == "declared") {
    return touch_kind::declared;
  }
  if (text == "actual") {
    return touch_kind::actual;
  }
  return std::nullopt;
}

auto touch_kind_to_text(touch_kind kind) -> std::string_view {
  return kind == touch_kind::declared ? std::string_view{"declared"} : std::string_view{"actual"};
}

auto start(db::connection& conn, const start_args& args) -> std::expected<start_result, runs_error> {
  // The Zig original uses a named savepoint; `planar.db` exposes only
  // transactions. Equivalent here — `start` is never called from inside an
  // outer transaction. Rollback is automatic on any early return below.
  auto tx = conn.begin_transaction();
  if (!tx) {
    return std::unexpected(runs_error::query_failed);
  }

  std::int64_t new_id = 0;
  {
    auto stmt = conn.prepare("insert into runs "
                             "(run_uid, plan_id, arm, base_sha, config_hash, config_json, corpus_repo, status) "
                             "values (?, ?, ?, ?, ?, ?, ?, coalesce(?, 'running')) returning id");
    if (!stmt) {
      return std::unexpected(runs_error::query_failed);
    }
    const bool bound = stmt->bind_text(1, args.run_uid).has_value() && stmt->bind_int64(2, args.plan_id).has_value() &&
                       stmt->bind_text(3, args.arm).has_value() && stmt->bind_text(4, args.base_sha).has_value() &&
                       stmt->bind_text(5, args.config_hash).has_value() && bind_opt_text(*stmt, 6, args.config_json) &&
                       bind_opt_text(*stmt, 7, args.corpus_repo) && bind_opt_text(*stmt, 8, args.status);
    if (!bound) {
      return std::unexpected(runs_error::query_failed);
    }
    auto stepped = stmt->step();
    if (!stepped) {
      if (is_unique_violation(stepped.error())) {
        return std::unexpected(runs_error::duplicate_run_uid);
      }
      return std::unexpected(runs_error::query_failed);
    }
    if (*stepped != db::step_result::row) {
      return std::unexpected(runs_error::query_failed);
    }
    new_id = stmt->column_int64(0);
  }

  // Snapshot declared touches: copy (task_id, path) pairs from
  // task_touch_paths into run_touches as kind='declared' rows.
  //
  // Unfiltered: every task under the plan, regardless of task status (see the
  // module header's oracle capture — a `done` task is still snapshotted).
  // Filtered: only the listed ids, with the join to `tasks` still constraining
  // them to this plan, so a foreign task id contributes nothing.
  if (args.task_filter.has_value()) {
    auto stmt = conn.prepare("insert into run_touches (run_id, task_id, path, kind) "
                             "select ?, ttp.task_id, ttp.path, 'declared' "
                             "from task_touch_paths ttp join tasks t on t.id = ttp.task_id "
                             "where ttp.task_id = ? and t.plan_id = ?");
    if (!stmt) {
      return std::unexpected(runs_error::query_failed);
    }
    // SQLite has no array-bind, so iterate — one reset+bind cycle per task id.
    for (const std::int64_t task_id : *args.task_filter) {
      if (auto reset = stmt->reset(); !reset) {
        return std::unexpected(runs_error::query_failed);
      }
      const bool bound = stmt->bind_int64(1, new_id).has_value() && stmt->bind_int64(2, task_id).has_value() &&
                         stmt->bind_int64(3, args.plan_id).has_value();
      if (!bound) {
        return std::unexpected(runs_error::query_failed);
      }
      if (auto stepped = step_write(*stmt, runs_error::query_failed); !stepped) {
        return std::unexpected(stepped.error());
      }
    }
  } else {
    auto stmt = conn.prepare("insert into run_touches (run_id, task_id, path, kind) "
                             "select ?, ttp.task_id, ttp.path, 'declared' "
                             "from task_touch_paths ttp join tasks t on t.id = ttp.task_id "
                             "where t.plan_id = ?");
    if (!stmt) {
      return std::unexpected(runs_error::query_failed);
    }
    const bool bound = stmt->bind_int64(1, new_id).has_value() && stmt->bind_int64(2, args.plan_id).has_value();
    if (!bound) {
      return std::unexpected(runs_error::query_failed);
    }
    if (auto stepped = step_write(*stmt, runs_error::query_failed); !stepped) {
      return std::unexpected(stepped.error());
    }
  }

  std::int64_t snapshotted = 0;
  {
    auto stmt = conn.prepare("select count(*) from run_touches where run_id = ? and kind = 'declared'");
    if (!stmt) {
      return std::unexpected(runs_error::query_failed);
    }
    if (auto bound = stmt->bind_int64(1, new_id); !bound) {
      return std::unexpected(runs_error::query_failed);
    }
    auto stepped = stmt->step();
    if (!stepped) {
      return std::unexpected(runs_error::query_failed);
    }
    if (*stepped == db::step_result::row) {
      snapshotted = stmt->column_int64(0);
    }
  }

  if (auto committed = tx->commit(); !committed) {
    return std::unexpected(runs_error::query_failed);
  }

  return start_result{
      .id                   = new_id,
      .run_uid              = std::string{args.run_uid},
      .declared_snapshotted = snapshotted,
  };
}

auto event(db::connection& conn, std::int64_t run_id, std::int64_t seq, std::string_view kind,
           std::optional<std::string_view> payload) -> std::expected<std::int64_t, runs_error> {
  auto stmt = conn.prepare("insert into run_events (run_id, seq, kind, payload) values (?, ?, ?, ?) returning id");
  if (!stmt) {
    return std::unexpected(runs_error::query_failed);
  }
  const bool bound = stmt->bind_int64(1, run_id).has_value() && stmt->bind_int64(2, seq).has_value() &&
                     stmt->bind_text(3, kind).has_value() && bind_opt_text(*stmt, 4, payload);
  if (!bound) {
    return std::unexpected(runs_error::query_failed);
  }
  auto stepped = stmt->step();
  if (!stepped) {
    if (is_unique_violation(stepped.error())) {
      return std::unexpected(runs_error::duplicate_seq);
    }
    return std::unexpected(runs_error::query_failed);
  }
  if (*stepped != db::step_result::row) {
    return std::unexpected(runs_error::query_failed);
  }
  return stmt->column_int64(0);
}

auto next_seq(db::connection& conn, std::int64_t run_id) -> std::expected<std::int64_t, runs_error> {
  auto stmt = conn.prepare("select coalesce(max(seq), 0) + 1 from run_events where run_id = ?");
  if (!stmt) {
    return std::unexpected(runs_error::query_failed);
  }
  if (auto bound = stmt->bind_int64(1, run_id); !bound) {
    return std::unexpected(runs_error::query_failed);
  }
  auto stepped = stmt->step();
  if (!stepped) {
    return std::unexpected(runs_error::query_failed);
  }
  if (*stepped != db::step_result::row) {
    return 1;
  }
  return stmt->column_int64(0);
}

auto touch(db::connection& conn, std::int64_t run_id, std::int64_t task_id, std::string_view path, touch_kind kind)
    -> std::expected<std::int64_t, runs_error> {
  auto stmt = conn.prepare("insert into run_touches (run_id, task_id, path, kind) values (?, ?, ?, ?) returning id");
  if (!stmt) {
    return std::unexpected(runs_error::query_failed);
  }
  const bool bound = stmt->bind_int64(1, run_id).has_value() && stmt->bind_int64(2, task_id).has_value() &&
                     stmt->bind_text(3, path).has_value() && stmt->bind_text(4, touch_kind_to_text(kind)).has_value();
  if (!bound) {
    return std::unexpected(runs_error::query_failed);
  }
  // Deliberately maps a UNIQUE conflict to query_failed, not to a dedicated
  // error: the oracle surfaces a duplicate touch as a bare
  // `error: bench touch: QueryFailed` at exit 1, and that string is the
  // contract. Use `touch_idempotent` when a repeat must be a no-op.
  auto stepped = stmt->step();
  if (!stepped) {
    return std::unexpected(runs_error::query_failed);
  }
  if (*stepped != db::step_result::row) {
    return std::unexpected(runs_error::query_failed);
  }
  return stmt->column_int64(0);
}

auto touch_idempotent(db::connection& conn, std::int64_t run_id, std::int64_t task_id, std::string_view path, touch_kind kind)
    -> std::expected<void, runs_error> {
  auto stmt = conn.prepare("insert or ignore into run_touches (run_id, task_id, path, kind) values (?, ?, ?, ?)");
  if (!stmt) {
    return std::unexpected(runs_error::query_failed);
  }
  const bool bound = stmt->bind_int64(1, run_id).has_value() && stmt->bind_int64(2, task_id).has_value() &&
                     stmt->bind_text(3, path).has_value() && stmt->bind_text(4, touch_kind_to_text(kind)).has_value();
  if (!bound) {
    return std::unexpected(runs_error::query_failed);
  }
  return step_write(*stmt, runs_error::query_failed);
}

auto finish(db::connection& conn, std::int64_t run_id, std::string_view status) -> std::expected<void, runs_error> {
  // Existence check first: a no-op UPDATE on a missing id would otherwise
  // report success, and the oracle reports `run '<uid>' not found` at exit 1.
  {
    auto stmt = conn.prepare("select count(*) from runs where id = ?");
    if (!stmt) {
      return std::unexpected(runs_error::query_failed);
    }
    if (auto bound = stmt->bind_int64(1, run_id); !bound) {
      return std::unexpected(runs_error::query_failed);
    }
    auto stepped = stmt->step();
    if (!stepped) {
      return std::unexpected(runs_error::query_failed);
    }
    const std::int64_t count = (*stepped == db::step_result::row) ? stmt->column_int64(0) : 0;
    if (count == 0) {
      return std::unexpected(runs_error::not_found);
    }
  }

  auto stmt = conn.prepare("update runs set status = ?, "
                           "ended_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') where id = ?");
  if (!stmt) {
    return std::unexpected(runs_error::query_failed);
  }
  const bool bound = stmt->bind_text(1, status).has_value() && stmt->bind_int64(2, run_id).has_value();
  if (!bound) {
    return std::unexpected(runs_error::query_failed);
  }
  return step_write(*stmt, runs_error::query_failed);
}

auto show(db::connection& conn, std::int64_t id) -> std::expected<run, runs_error> {
  auto stmt = conn.prepare(std::string{k_select_columns} + " where id = ?");
  if (!stmt) {
    return std::unexpected(runs_error::query_failed);
  }
  if (auto bound = stmt->bind_int64(1, id); !bound) {
    return std::unexpected(runs_error::query_failed);
  }
  auto stepped = stmt->step();
  if (!stepped) {
    return std::unexpected(runs_error::query_failed);
  }
  if (*stepped != db::step_result::row) {
    return std::unexpected(runs_error::not_found);
  }
  return read_run(*stmt);
}

auto show_by_uid(db::connection& conn, std::string_view run_uid) -> std::expected<run, runs_error> {
  auto stmt = conn.prepare(std::string{k_select_columns} + " where run_uid = ?");
  if (!stmt) {
    return std::unexpected(runs_error::query_failed);
  }
  if (auto bound = stmt->bind_text(1, run_uid); !bound) {
    return std::unexpected(runs_error::query_failed);
  }
  auto stepped = stmt->step();
  if (!stepped) {
    return std::unexpected(runs_error::query_failed);
  }
  if (*stepped != db::step_result::row) {
    return std::unexpected(runs_error::not_found);
  }
  return read_run(*stmt);
}

auto events(db::connection& conn, std::int64_t run_id) -> std::expected<std::vector<event_row>, runs_error> {
  auto stmt = conn.prepare("select id, run_id, seq, kind, payload, created_at from run_events "
                           "where run_id = ? order by seq");
  if (!stmt) {
    return std::unexpected(runs_error::query_failed);
  }
  if (auto bound = stmt->bind_int64(1, run_id); !bound) {
    return std::unexpected(runs_error::query_failed);
  }

  std::vector<event_row> out;
  while (true) {
    auto stepped = stmt->step();
    if (!stepped) {
      return std::unexpected(runs_error::query_failed);
    }
    if (*stepped == db::step_result::done) {
      break;
    }
    out.push_back(event_row{
        .id         = stmt->column_int64(0),
        .run_id     = stmt->column_int64(1),
        .seq        = stmt->column_int64(2),
        .kind       = stmt->column_text(3),
        .payload    = opt_text(*stmt, 4),
        .created_at = stmt->column_text(5),
    });
  }
  return out;
}

auto touches(db::connection& conn, std::int64_t run_id, std::optional<touch_kind> kind)
    -> std::expected<std::vector<touch_row>, runs_error> {
  constexpr std::string_view k_base = "select id, run_id, task_id, path, kind, created_at from run_touches "
                                      "where run_id = ?";
  auto                       stmt   = kind.has_value() ? conn.prepare(std::string{k_base} + " and kind = ? order by id")
                                                       : conn.prepare(std::string{k_base} + " order by id");
  if (!stmt) {
    return std::unexpected(runs_error::query_failed);
  }
  if (auto bound = stmt->bind_int64(1, run_id); !bound) {
    return std::unexpected(runs_error::query_failed);
  }
  if (kind.has_value()) {
    if (auto bound = stmt->bind_text(2, touch_kind_to_text(*kind)); !bound) {
      return std::unexpected(runs_error::query_failed);
    }
  }

  std::vector<touch_row> out;
  while (true) {
    auto stepped = stmt->step();
    if (!stepped) {
      return std::unexpected(runs_error::query_failed);
    }
    if (*stepped == db::step_result::done) {
      break;
    }
    const auto parsed = touch_kind_from_text(stmt->column_text(4));
    if (!parsed.has_value()) {
      return std::unexpected(runs_error::query_failed);
    }
    out.push_back(touch_row{
        .id         = stmt->column_int64(0),
        .run_id     = stmt->column_int64(1),
        .task_id    = stmt->column_int64(2),
        .path       = stmt->column_text(3),
        .kind_      = *parsed,
        .created_at = stmt->column_text(5),
    });
  }
  return out;
}

auto generate_run_uid(db::connection& conn) -> std::expected<std::string, runs_error> {
  auto stmt = conn.prepare("select lower(hex(randomblob(16)))");
  if (!stmt) {
    return std::unexpected(runs_error::query_failed);
  }
  auto stepped = stmt->step();
  if (!stepped) {
    return std::unexpected(runs_error::query_failed);
  }
  if (*stepped != db::step_result::row) {
    return std::unexpected(runs_error::query_failed);
  }
  return stmt->column_text(0);
}

} // namespace planar::engine::runs::lifecycle
