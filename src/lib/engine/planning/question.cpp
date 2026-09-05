/// @file question.cpp
/// @brief Implementation of `planar.engine.planning.question` (see
/// question.cppm).

module;

module planar.engine.planning.question;

import std;
import planar.db;
import planar.json_text;
import planar.log;
import planar.scope_ref;
import planar.policy;
import planar.engine.planning.transitions;

namespace planar::engine::planning {

using json_text::json_string;

namespace audit = planar::policy::audit;

namespace {

/// @brief Emit the oracle's inner `<op> exec failed: <ErrorName>`
/// diagnostic ahead of the outer handler error. See
/// `zig/src/engine/planning/question.zig`'s `create` for the shape this
/// ports; mirrors `exec_failed` in task.cpp (same bucket, separate TU).
auto exec_failed(std::string_view op, std::string_view zig_error_name) -> question_error {
  log::diag_err(std::format("{} exec failed: {}", op, zig_error_name));
  return question_error::query_failed;
}

/// @brief Append one `audit_log` row, mapping a write failure into this
/// module's error surface. Called AFTER the question's own write succeeds —
/// a refused question mutation writes no audit row in the oracle (verified:
/// a `wontfix` refused as `IllegalTransition` leaves `audit_log` untouched).
/// @param conn An open, migrated connection.
/// @param args The row to write.
/// @return Nothing, or `audit_write_failed`.
auto record_audit(db::connection& conn, const audit::record_args& args) -> std::expected<void, question_error> {
  if (auto ok = audit::record(conn, args); !ok) {
    return std::unexpected(question_error::audit_write_failed);
  }
  return {};
}

auto scope_kind_to_text(question_scope_kind k) -> std::string_view {
  switch (k) {
  case question_scope_kind::global:
    return "global";
  case question_scope_kind::association:
    return "association";
  case question_scope_kind::repo:
    return "repo";
  }
  return "global"; // unreachable
}

/// @brief Translate `planar.scope_ref`'s `scope_kind` onto this module's own
/// `question_scope_kind`. A 1:1 mapping kept explicit, matching `plan.cpp`.
/// @param k The layer-1 scope kind.
/// @return This module's corresponding scope kind.
auto to_question_kind(scope_ref::scope_kind k) -> question_scope_kind {
  switch (k) {
  case scope_ref::scope_kind::global:
    return question_scope_kind::global;
  case scope_ref::scope_kind::association:
    return question_scope_kind::association;
  case scope_ref::scope_kind::repo:
    return question_scope_kind::repo;
  }
  return question_scope_kind::global; // unreachable
}

/// @brief Map a `planar.scope_ref` failure onto this module's error surface.
/// @param e The layer-1 failure.
/// @return The corresponding `question_error`.
auto to_question_error(scope_ref::error e) -> question_error {
  switch (e) {
  case scope_ref::error::query_failed:
    return question_error::query_failed;
  case scope_ref::error::slug_not_found:
    return question_error::slug_not_found;
  }
  return question_error::query_failed; // unreachable
}

/// @brief A resolved scope: the kind plus the row id (unset for global).
using resolved_scope = std::pair<question_scope_kind, std::optional<std::int64_t>>;

/// @brief Resolve `scope` (when present) to a `(kind,id)` pair, else global.
///
/// Delegates the `"global"` / `"repo:<slug>"` / `"assoc:<slug>"` /
/// bare-association grammar to layer-1 `planar.scope_ref::resolve` (D19).
/// Only the "no scope argument at all -> global" fold — distinct from the
/// literal string `"global"` — stays local, exactly as `plan.cpp` has it.
/// @param conn An open, migrated connection.
/// @param scope The scope slug, or unset.
/// @return The resolved pair, or the mapped failure.
auto resolve_scope_or_global(db::connection& conn, const std::optional<std::string>& scope)
    -> std::expected<resolved_scope, question_error> {
  if (!scope.has_value()) {
    return std::make_pair(question_scope_kind::global, std::optional<std::int64_t>{});
  }
  auto resolved = scope_ref::resolve(conn, *scope);
  if (!resolved) {
    return std::unexpected(to_question_error(resolved.error()));
  }
  return std::make_pair(to_question_kind(resolved->kind), resolved->id);
}

/// @brief Resolve `filter.scope` followed by every member of `filter.scopes`
/// into ONE ordered disjunction.
///
/// An unresolvable slug ANYWHERE in the set fails the whole call rather
/// than being skipped: a read verb that quietly drops one member of its
/// scope set returns a short list that looks complete.
/// @param conn An open, migrated connection.
/// @param filter The filter carrying the scope members.
/// @return The resolved refs in order, or the first failure.
auto resolve_scope_set(db::connection& conn, const question_list_filter& filter)
    -> std::expected<std::vector<resolved_scope>, question_error> {
  std::vector<resolved_scope> refs;
  if (filter.scope.has_value()) {
    auto resolved = resolve_scope_or_global(conn, filter.scope);
    if (!resolved) {
      return std::unexpected(resolved.error());
    }
    refs.push_back(*resolved);
  }
  for (const auto& s : filter.scopes) {
    auto resolved = resolve_scope_or_global(conn, std::optional<std::string>{s});
    if (!resolved) {
      return std::unexpected(resolved.error());
    }
    refs.push_back(*resolved);
  }
  return refs;
}

constexpr std::string_view k_select_columns = "select id, scope_kind, scope_id, title, body, status, answer_body, "
                                              "answered_at, created_at, updated_at from questions";

auto read_row(db::statement& stmt) -> std::expected<question, question_error> {
  const auto          scope_kind_text = stmt.column_text(1);
  question_scope_kind sk{};
  if (scope_kind_text == "global") {
    sk = question_scope_kind::global;
  } else if (scope_kind_text == "association") {
    sk = question_scope_kind::association;
  } else if (scope_kind_text == "repo") {
    sk = question_scope_kind::repo;
  } else {
    return std::unexpected(question_error::query_failed);
  }

  const auto status = question_status_from_text(stmt.column_text(5));
  if (!status.has_value()) {
    return std::unexpected(question_error::query_failed);
  }

  return question{
      .id          = stmt.column_int64(0),
      .scope_kind  = sk,
      .scope_id    = stmt.is_null(2) ? std::optional<std::int64_t>{} : std::optional<std::int64_t>{stmt.column_int64(2)},
      .title       = stmt.column_text(3),
      .body        = stmt.is_null(4) ? std::optional<std::string>{} : std::optional<std::string>{stmt.column_text(4)},
      .status      = *status,
      .answer_body = stmt.is_null(6) ? std::optional<std::string>{} : std::optional<std::string>{stmt.column_text(6)},
      .answered_at = stmt.is_null(7) ? std::optional<std::string>{} : std::optional<std::string>{stmt.column_text(7)},
      .created_at  = stmt.column_text(8),
      .updated_at  = stmt.column_text(9),
  };
}

/// @brief The `status in (...)` predicate for a list query.
///
/// The empty arm is a LITERAL `status = 'open'`, not "no predicate". See
/// `question_list_filter::statuses`.
/// @param filter The filter to read `statuses` from.
/// @return The SQL fragment, leading space included.
auto status_clause(const question_list_filter& filter) -> std::string {
  if (filter.statuses.empty()) {
    return " and status = 'open'";
  }
  std::string out = " and status in (";
  for (std::size_t i = 0; i < filter.statuses.size(); ++i) {
    if (i > 0) {
      out += ", ";
    }
    out += "?";
  }
  out += ")";
  return out;
}

/// @brief The scope disjunction for a list query.
///
/// `global` matches on `scope_kind` ALONE — the oracle emits a bare
/// `scope_kind = 'global'` with no `scope_id is null` conjunct. Reproduced
/// rather than tightened (D2): adding the null check would change which
/// rows a corrupt global-with-a-scope_id row matches.
/// @param refs The resolved scope set; an empty set yields an empty string.
/// @return The SQL fragment, leading space included.
auto scope_clause(std::span<const resolved_scope> refs) -> std::string {
  if (refs.empty()) {
    return {};
  }
  std::string out = " and (";
  for (std::size_t i = 0; i < refs.size(); ++i) {
    if (i > 0) {
      out += " or ";
    }
    if (refs[i].first == question_scope_kind::global) {
      out += "scope_kind = 'global'";
    } else {
      out += "(scope_kind = ? and scope_id = ?)";
    }
  }
  out += ")";
  return out;
}

/// @brief Read every remaining row of `stmt` into a vector.
/// @param stmt A bound, un-stepped statement over `k_select_columns`.
/// @return The rows in statement order, or the first decode/step failure.
auto collect_rows(db::statement& stmt) -> std::expected<std::vector<question>, question_error> {
  std::vector<question> out;
  for (;;) {
    auto step = stmt.step();
    if (!step) {
      return std::unexpected(question_error::query_failed);
    }
    if (*step == db::step_result::done) {
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

/// @brief Shared body of `answer_question` / `wontfix_question`.
///
/// Both verbs read the current row, validate the transition, run their own
/// UPDATE, write one `status_change` audit row and re-read — inside one
/// transaction so a failed audit write takes the status flip with it. The
/// only differences are the SET list and the summary, which the caller
/// supplies.
/// @param conn An open, migrated connection.
/// @param id The question's row id.
/// @param target The status being moved to.
/// @param apply Runs the verb's own UPDATE; returns false on any failure.
/// @param summary The `audit_log` summary to record.
/// @return The updated row, or the first failure.
auto transition(db::connection& conn, std::int64_t id, question_status target, const std::function<bool(db::connection&)>& apply,
                std::string summary) -> std::expected<question, question_error> {
  auto tx = conn.begin_transaction();
  if (!tx) {
    return std::unexpected(question_error::query_failed);
  }

  auto current = show_question(conn, id);
  if (!current) {
    return std::unexpected(current.error());
  }

  // The question arm never reports `unknown_status`: every non-`open`
  // source is refused as `illegal_transition` (see transitions.cpp), and
  // `from == to` short-circuits before the arm is consulted at all — which
  // is why re-answering an answered question succeeds.
  if (auto checked = check_transition(transition_kind::question, question_status_to_text(current->status),
                                      question_status_to_text(target), false);
      !checked) {
    return std::unexpected(question_error::illegal_transition);
  }

  if (!apply(conn)) {
    return std::unexpected(question_error::query_failed);
  }

  if (auto a = record_audit(conn, audit::record_args{.verb    = audit::verb::status_change,
                                                     .entity  = {.kind = "question", .id = id},
                                                     .summary = std::move(summary)});
      !a) {
    return std::unexpected(a.error());
  }

  auto updated = show_question(conn, id);
  if (!updated) {
    return std::unexpected(updated.error());
  }
  if (auto committed = tx->commit(); !committed) {
    return std::unexpected(question_error::query_failed);
  }
  return updated;
}

} // namespace

auto question_status_from_text(std::string_view s) -> std::optional<question_status> {
  if (s == "open") {
    return question_status::open;
  }
  if (s == "answered") {
    return question_status::answered;
  }
  if (s == "wontfix") {
    return question_status::wontfix;
  }
  return std::nullopt;
}

auto question_status_to_text(question_status s) -> std::string_view {
  switch (s) {
  case question_status::open:
    return "open";
  case question_status::answered:
    return "answered";
  case question_status::wontfix:
    return "wontfix";
  }
  return "open"; // unreachable
}

auto create_question(db::connection& conn, const question_create_args& args) -> std::expected<question, question_error> {
  auto scope = resolve_scope_or_global(conn, args.scope);
  if (!scope) {
    return std::unexpected(scope.error());
  }

  // `--plan` is validated BEFORE the INSERT, so a dangling plan id leaves
  // neither a `questions` row nor an `audit_log` row. Oracle-confirmed.
  if (args.plan_id.has_value()) {
    auto stmt = conn.prepare("select count(*) from plans where id = ?");
    if (!stmt) {
      return std::unexpected(question_error::query_failed);
    }
    if (auto b = stmt->bind_int64(1, *args.plan_id); !b) {
      return std::unexpected(question_error::query_failed);
    }
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(question_error::query_failed);
    }
    if (*step != db::step_result::row) {
      return std::unexpected(question_error::query_failed);
    }
    if (stmt->column_int64(0) == 0) {
      return std::unexpected(question_error::not_found);
    }
  }

  std::int64_t id = 0;
  {
    auto stmt = conn.prepare("insert into questions (scope_kind, scope_id, title, body) values (?, ?, ?, ?) returning id");
    if (!stmt) {
      return std::unexpected(exec_failed("question.create", "PrepareFailed"));
    }
    if (auto b = stmt->bind_text(1, scope_kind_to_text(scope->first)); !b) {
      return std::unexpected(exec_failed("question.create", "BindFailed"));
    }
    auto b2 = scope->second.has_value() ? stmt->bind_int64(2, *scope->second) : stmt->bind_null(2);
    if (!b2) {
      return std::unexpected(exec_failed("question.create", "BindFailed"));
    }
    if (auto b = stmt->bind_text(3, args.title); !b) {
      return std::unexpected(exec_failed("question.create", "BindFailed"));
    }
    // `body` binds SQL NULL when absent, NOT the empty string: `render_json`
    // emits `null` vs `""` and the two are operator-visible.
    auto b4 = args.body.has_value() ? stmt->bind_text(4, *args.body) : stmt->bind_null(4);
    if (!b4) {
      return std::unexpected(exec_failed("question.create", "BindFailed"));
    }
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(exec_failed("question.create", "StepFailed"));
    }
    if (*step != db::step_result::row) {
      return std::unexpected(exec_failed("question.create", "StepFailed"));
    }
    id = stmt->column_int64(0);
  }

  // ORACLE: `create|question|1|create question 'first question'` — the
  // TITLE in single quotes.
  if (auto a = record_audit(conn, audit::record_args{.verb    = audit::verb::create,
                                                     .entity  = {.kind = "question", .id = id},
                                                     .summary = std::format("create question '{}'", args.title)});
      !a) {
    return std::unexpected(a.error());
  }

  if (args.plan_id.has_value()) {
    // No `link` audit row here — the oracle writes exactly one row for
    // `question add --plan`, and its verb is `create`.
    auto stmt = conn.prepare("insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) "
                             "values ('question', ?, 'plan', ?, 'derives-from')");
    if (!stmt) {
      return std::unexpected(question_error::query_failed);
    }
    if (auto b = stmt->bind_int64(1, id); !b) {
      return std::unexpected(question_error::query_failed);
    }
    if (auto b = stmt->bind_int64(2, *args.plan_id); !b) {
      return std::unexpected(question_error::query_failed);
    }
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(question_error::query_failed);
    }
  }

  return show_question(conn, id);
}

auto show_question(db::connection& conn, std::int64_t id) -> std::expected<question, question_error> {
  auto stmt = conn.prepare(std::format("{} where id = ?", k_select_columns));
  if (!stmt) {
    return std::unexpected(question_error::query_failed);
  }
  if (auto bound = stmt->bind_int64(1, id); !bound) {
    return std::unexpected(question_error::query_failed);
  }
  auto step = stmt->step();
  if (!step) {
    return std::unexpected(question_error::query_failed);
  }
  if (*step != db::step_result::row) {
    return std::unexpected(question_error::not_found);
  }
  return read_row(*stmt);
}

auto list_questions(db::connection& conn, const question_list_filter& filter)
    -> std::expected<std::vector<question>, question_error> {
  auto refs = resolve_scope_set(conn, filter);
  if (!refs) {
    return std::unexpected(refs.error());
  }

  std::string sql = std::string(k_select_columns) + " where 1 = 1";
  sql += status_clause(filter);
  sql += scope_clause(*refs);
  if (filter.plan_id.has_value()) {
    sql += " and id in (select from_id from entity_links where from_kind = 'question' and to_kind = 'plan' and "
           "relationship = 'derives-from' and to_id = ?)";
  }
  sql += " order by id";

  auto stmt = conn.prepare(sql);
  if (!stmt) {
    return std::unexpected(question_error::query_failed);
  }
  int idx = 1;
  for (const auto s : filter.statuses) {
    if (auto b = stmt->bind_text(idx++, question_status_to_text(s)); !b) {
      return std::unexpected(question_error::query_failed);
    }
  }
  for (const auto& ref : *refs) {
    if (ref.first == question_scope_kind::global) {
      continue; // literal in the SQL; no placeholder to fill.
    }
    if (!ref.second.has_value()) {
      // A non-global ref with no row id cannot become a predicate. The
      // oracle unwraps the optional unconditionally and would panic;
      // refuse rather than bind a placeholder id, which would silently
      // match nothing and return a short list.
      return std::unexpected(question_error::query_failed);
    }
    if (auto b = stmt->bind_text(idx++, scope_kind_to_text(ref.first)); !b) {
      return std::unexpected(question_error::query_failed);
    }
    if (auto b = stmt->bind_int64(idx++, *ref.second); !b) {
      return std::unexpected(question_error::query_failed);
    }
  }
  if (filter.plan_id.has_value()) {
    if (auto b = stmt->bind_int64(idx++, *filter.plan_id); !b) {
      return std::unexpected(question_error::query_failed);
    }
  }

  return collect_rows(*stmt);
}

auto list_questions_touching(db::connection& conn, std::int64_t repo_id, const question_list_filter& filter)
    -> std::expected<std::vector<question>, question_error> {
  auto refs = resolve_scope_set(conn, filter);
  if (!refs) {
    return std::unexpected(refs.error());
  }

  // The direct-repo-scope branch is ALL-OR-NOTHING against the scope set —
  // see the header. With no scope set at all it stays on.
  bool branch_direct = true;
  if (!refs->empty()) {
    branch_direct = false;
    for (const auto& ref : *refs) {
      if (ref.first == question_scope_kind::repo && (!ref.second.has_value() || *ref.second == repo_id)) {
        branch_direct = true;
        break;
      }
    }
  }

  const auto status_sql = status_clause(filter);

  std::string sql = "select * from (";
  sql += k_select_columns;
  sql += " where 1 = 1";
  if (branch_direct) {
    sql += " and scope_kind = 'repo' and scope_id = ?";
    sql += status_sql;
  } else {
    // A branch that is off is emitted as `1 = 0` rather than dropped, so
    // the UNION keeps both arms and the column list stays identical.
    sql += " and 1 = 0";
  }
  sql += " union ";
  sql += k_select_columns;
  sql += " where 1 = 1";
  sql += " and id in (select from_id from entity_links where from_kind = 'question' and to_kind = 'repo' and to_id = ? and "
         "relationship = 'touches')";
  sql += status_sql;
  sql += scope_clause(*refs);
  sql += ") order by id";

  auto stmt = conn.prepare(sql);
  if (!stmt) {
    return std::unexpected(question_error::query_failed);
  }
  int  idx       = 1;
  auto bind_int  = [&](std::int64_t v) { return stmt->bind_int64(idx++, v).has_value(); };
  auto bind_text = [&](std::string_view v) { return stmt->bind_text(idx++, v).has_value(); };

  if (branch_direct) {
    if (!bind_int(repo_id)) {
      return std::unexpected(question_error::query_failed);
    }
    for (const auto s : filter.statuses) {
      if (!bind_text(question_status_to_text(s))) {
        return std::unexpected(question_error::query_failed);
      }
    }
  }
  if (!bind_int(repo_id)) {
    return std::unexpected(question_error::query_failed);
  }
  for (const auto s : filter.statuses) {
    if (!bind_text(question_status_to_text(s))) {
      return std::unexpected(question_error::query_failed);
    }
  }
  for (const auto& ref : *refs) {
    if (ref.first == question_scope_kind::global) {
      continue;
    }
    if (!ref.second.has_value()) {
      return std::unexpected(question_error::query_failed);
    }
    if (!bind_text(scope_kind_to_text(ref.first)) || !bind_int(*ref.second)) {
      return std::unexpected(question_error::query_failed);
    }
  }

  return collect_rows(*stmt);
}

auto answer_question(db::connection& conn, std::int64_t id, std::string_view answer) -> std::expected<question, question_error> {
  // Checked BEFORE the row is read, matching the oracle: `question answer
  // <nonexistent> --answer ""` reports the empty-answer refusal, not
  // NotFound.
  if (answer.empty()) {
    return std::unexpected(question_error::answer_required);
  }

  auto apply = [&](db::connection& c) {
    auto stmt = c.prepare("update questions set status = 'answered', answer_body = ?, "
                          "answered_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now'), "
                          "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') where id = ?");
    if (!stmt) {
      return false;
    }
    if (auto b = stmt->bind_text(1, answer); !b) {
      return false;
    }
    if (auto b = stmt->bind_int64(2, id); !b) {
      return false;
    }
    return stmt->step().has_value();
  };

  // ORACLE: `status_change|question|1|answer: the answer`.
  return transition(conn, id, question_status::answered, apply, std::format("answer: {}", answer));
}

auto wontfix_question(db::connection& conn, std::int64_t id, std::optional<std::string_view> reason)
    -> std::expected<question, question_error> {
  auto apply = [&](db::connection& c) {
    auto stmt = c.prepare("update questions set status = 'wontfix', "
                          "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') where id = ?");
    if (!stmt) {
      return false;
    }
    if (auto b = stmt->bind_int64(1, id); !b) {
      return false;
    }
    return stmt->step().has_value();
  };

  // ORACLE: `wontfix: <reason>` with a reason, the bare word `wontfix`
  // without one. An absent reason is NOT `wontfix: ` with a trailing space.
  auto summary = reason.has_value() ? std::format("wontfix: {}", *reason) : std::string{"wontfix"};
  return transition(conn, id, question_status::wontfix, apply, std::move(summary));
}

auto render_text(const question& q) -> std::string {
  std::string out;
  out += std::format("id:        {}\n", q.id);
  out += std::format("title:     {}\n", q.title);
  out += std::format("status:    {}\n", question_status_to_text(q.status));
  out += std::format("scope:     {}", scope_kind_to_text(q.scope_kind));
  if (q.scope_id.has_value()) {
    out += std::format(":{}", *q.scope_id);
  }
  out += "\n";
  if (q.body.has_value()) {
    out += std::format("body:      {}\n", *q.body);
  }
  if (q.answer_body.has_value()) {
    out += std::format("answer:    {}\n", *q.answer_body);
  }
  if (q.answered_at.has_value()) {
    out += std::format("answered:  {}\n", *q.answered_at);
  }
  out += std::format("created:   {}\n", q.created_at);
  out += std::format("updated:   {}\n", q.updated_at);
  return out;
}

auto render_json(const question& q) -> std::string {
  auto const opt_str = [](const std::optional<std::string>& v) -> std::string {
    return v.has_value() ? json_string(*v) : std::string{"null"};
  };
  return std::format(R"({{"id":{},"scope_kind":"{}","scope_id":{},"title":{},"body":{},"status":"{}",)"
                     R"("answer_body":{},"answered_at":{},"created_at":{},"updated_at":{}}})",
                     q.id, scope_kind_to_text(q.scope_kind),
                     q.scope_id.has_value() ? std::format("{}", *q.scope_id) : std::string{"null"}, json_string(q.title),
                     opt_str(q.body), question_status_to_text(q.status), opt_str(q.answer_body), opt_str(q.answered_at),
                     json_string(q.created_at), json_string(q.updated_at));
}

auto render_list_text(std::span<const question> items) -> std::string {
  if (items.empty()) {
    return "(no questions)\n";
  }
  std::string out;
  for (const auto& q : items) {
    out += std::format("{:>5}  {:<10}  {}\n", q.id, question_status_to_text(q.status), q.title);
  }
  return out;
}

auto render_list_json(std::span<const question> items) -> std::string {
  std::string out = "[";
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (i > 0) {
      out += ",";
    }
    out += render_json(items[i]);
  }
  out += "]";
  return out;
}

} // namespace planar::engine::planning
