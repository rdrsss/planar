/// @file contextrecords.cpp
/// @brief SQLite implementation of the run-scoped context-record store.
module planar.engine.runtime.contextrecords;
import std;
import planar.db;
namespace planar::engine::runtime::contextrecords {
namespace {
auto step(db::connection& c, std::string_view sql,
          std::initializer_list<std::variant<std::int64_t, std::string_view, std::nullptr_t>> v)
    -> std::expected<void, db::db_error> {
  auto q = c.prepare(sql);
  if (!q)
    return std::unexpected(q.error());
  int n = 1;
  for (auto x : v) {
    auto r = std::visit(
        [&](auto y) -> std::expected<void, db::db_error> {
          using T = decltype(y);
          if constexpr (std::same_as<T, std::int64_t>)
            return q->bind_int64(n, y);
          else if constexpr (std::same_as<T, std::string_view>)
            return q->bind_text(n, y);
          else
            return q->bind_null(n);
        },
        x);
    if (!r)
      return r;
    ++n;
  }
  auto s = q->step();
  if (!s)
    return std::unexpected(s.error());
  return {};
}
auto row(db::statement const& q) -> record {
  return {.id            = q.column_int64(0),
          .run_id        = q.column_int64(1),
          .session_id    = q.column_int64(3),
          .claim_id      = q.is_null(4) ? std::nullopt : std::optional{q.column_int64(4)},
          .stage         = q.column_text(2),
          .kind          = q.column_text(5),
          .body          = q.column_text(6),
          .status        = q.column_text(7),
          .created_at    = q.column_text(9),
          .compiled_from = q.is_null(8) ? std::nullopt : std::optional{q.column_text(8)}};
}
auto one(db::connection& c) -> std::expected<record, db::db_error> {
  auto q = c.prepare("select id,run_id,stage,session_id,claim_id,kind,body,status,compiled_from,created_at from context_records "
                     "where id=last_insert_rowid()");
  if (!q)
    return std::unexpected(q.error());
  auto s = q->step();
  if (!s)
    return std::unexpected(s.error());
  return row(*q);
}
} // namespace
auto add_from_claim(db::connection& c, const add_input& i) -> std::expected<record, db::db_error> {
  auto q = c.prepare("select id,session_id,run_id,stage from agent_work_claims where claim_token=?");
  if (!q)
    return std::unexpected(q.error());
  if (auto r = q->bind_text(1, i.token); !r)
    return std::unexpected(r.error());
  auto s = q->step();
  if (!s)
    return std::unexpected(s.error());
  if (*s == db::step_result::done)
    return std::unexpected(db::db_error{.message_ = "claim not found"});
  if (q->is_null(2))
    return std::unexpected(db::db_error{.message_ = "claim has no run_id"});
  auto r = step(c,
                "insert into context_records(run_id,stage,session_id,claim_id,kind,body,status,compiled_from) "
                "values(?,?,?,?,?,?,'active',?)",
                {q->column_int64(2), q->column_text(3), q->column_int64(1), q->column_int64(0), i.kind, i.body,
                 i.compiled_from ? std::variant<std::int64_t, std::string_view, std::nullptr_t>{*i.compiled_from} : nullptr});
  if (!r)
    return std::unexpected(r.error());
  return one(c);
}
auto add_capsule(db::connection& c, const capsule_input& i) -> std::expected<record, db::db_error> {
  auto q = c.prepare("select 1 from workflow_runs where id=?");
  if (!q)
    return std::unexpected(q.error());
  if (auto r = q->bind_int64(1, i.run_id); !r)
    return std::unexpected(r.error());
  auto s = q->step();
  if (!s)
    return std::unexpected(s.error());
  if (*s == db::step_result::done)
    return std::unexpected(db::db_error{.message_ = "workflow run not found"});
  auto session = i.session_id;
  if (!session) {
    auto r = step(c, "insert into sessions(vendor) values('compactor')", {});
    if (!r)
      return std::unexpected(r.error());
    auto n = c.prepare("select last_insert_rowid()");
    if (!n)
      return std::unexpected(n.error());
    auto ns = n->step();
    if (!ns)
      return std::unexpected(ns.error());
    session = n->column_int64(0);
  }
  auto r = step(c,
                "insert into context_records(run_id,stage,session_id,claim_id,kind,body,status,compiled_from) "
                "values(?,?,?,null,'capsule',?,'active',?)",
                {i.run_id, i.stage, *session, i.body,
                 i.compiled_from ? std::variant<std::int64_t, std::string_view, std::nullptr_t>{*i.compiled_from} : nullptr});
  if (!r)
    return std::unexpected(r.error());
  return one(c);
}
auto list(db::connection& c, std::int64_t run, std::optional<std::string_view> stage, std::optional<std::string_view> status,
          std::optional<std::string_view> kind) -> std::expected<std::vector<record>, db::db_error> {
  std::string sql =
      "select id,run_id,stage,session_id,claim_id,kind,body,status,compiled_from,created_at from context_records where run_id=?";
  if (stage)
    sql += " and stage=?";
  if (status)
    sql += " and status=?";
  if (kind)
    sql += " and kind=?";
  sql += " order by id asc";
  auto q = c.prepare(sql);
  if (!q)
    return std::unexpected(q.error());
  int n = 1;
  if (auto r = q->bind_int64(n++, run); !r)
    return std::unexpected(r.error());
  for (auto p : {stage, status, kind})
    if (p)
      if (auto r = q->bind_text(n++, *p); !r)
        return std::unexpected(r.error());
  std::vector<record> out;
  while (true) {
    auto s = q->step();
    if (!s)
      return std::unexpected(s.error());
    if (*s == db::step_result::done)
      break;
    out.push_back(row(*q));
  }
  return out;
}
auto get(db::connection& c, std::int64_t id) -> std::expected<record, db::db_error> {
  auto q = c.prepare(
      "select id,run_id,stage,session_id,claim_id,kind,body,status,compiled_from,created_at from context_records where id=?");
  if (!q)
    return std::unexpected(q.error());
  if (auto r = q->bind_int64(1, id); !r)
    return std::unexpected(r.error());
  auto s = q->step();
  if (!s)
    return std::unexpected(s.error());
  if (*s == db::step_result::done)
    return std::unexpected(db::db_error{.message_ = "context record not found"});
  return row(*q);
}
auto resolve_one(db::connection& c, std::int64_t id, std::string_view status) -> std::expected<std::int64_t, db::db_error> {
  auto r = step(c, "update context_records set status=? where id=? and status='active'", {status, id});
  if (!r)
    return std::unexpected(r.error());
  auto q = c.prepare("select count(*) from context_records where id=? and status=?");
  if (!q)
    return std::unexpected(q.error());
  if (auto b = q->bind_int64(1, id); !b)
    return std::unexpected(b.error());
  if (auto b = q->bind_text(2, status); !b)
    return std::unexpected(b.error());
  auto s = q->step();
  if (!s)
    return std::unexpected(s.error());
  return q->column_int64(0);
}
auto resolve_stage(db::connection& c, std::int64_t run, std::string_view stage, std::string_view status)
    -> std::expected<std::int64_t, db::db_error> {
  auto q = c.prepare("select count(*) from context_records where run_id=? and stage=? and status='active'");
  if (!q)
    return std::unexpected(q.error());
  if (auto b = q->bind_int64(1, run); !b)
    return std::unexpected(b.error());
  if (auto b = q->bind_text(2, stage); !b)
    return std::unexpected(b.error());
  auto s = q->step();
  if (!s)
    return std::unexpected(s.error());
  auto count = q->column_int64(0);
  auto r = step(c, "update context_records set status=? where run_id=? and stage=? and status='active'", {status, run, stage});
  if (!r)
    return std::unexpected(r.error());
  return count;
}
} // namespace planar::engine::runtime::contextrecords
