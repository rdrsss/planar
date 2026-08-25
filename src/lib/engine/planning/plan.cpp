/// @file plan.cpp
/// @brief Implementation of `planar.engine.planning.plan` (see plan.cppm).

module;

module planar.engine.planning.plan;

import std;
import planar.db;
import planar.json_text;
import planar.scope_ref;
import planar.policy;
import planar.engine.planning.transitions;

namespace planar::engine::planning {

using json_text::json_string;

namespace audit = planar::policy::audit;

namespace {

/// @brief Append one `audit_log` row, mapping a write failure into this
/// module's error surface. Called AFTER the plan's own write succeeds --
/// a refused plan mutation writes no audit row in the oracle.
/// @param conn An open, migrated connection.
/// @param args The row to write.
/// @return Nothing, or `audit_write_failed`.
auto record_audit(db::connection& conn, const audit::record_args& args) -> std::expected<void, plan_error> {
  if (auto ok = audit::record(conn, args); !ok) {
    return std::unexpected(plan_error::audit_write_failed);
  }
  return {};
}

// SQLite extended result code this module distinguishes. Mirrored here
// rather than pulling in <sqlite3.h> — this module never touches the raw C
// API, only `planar.db`'s own typed surface.
constexpr int k_sqlite_constraint_unique = 2067; // SQLITE_CONSTRAINT_UNIQUE

auto is_unique_violation(const db::db_error& err) -> bool {
  return err.code_ == k_sqlite_constraint_unique;
}

auto scope_kind_to_text(plan_scope_kind k) -> std::string_view {
  switch (k) {
  case plan_scope_kind::global:
    return "global";
  case plan_scope_kind::association:
    return "association";
  case plan_scope_kind::repo:
    return "repo";
  }
  return "global"; // unreachable
}

/// @brief Translate `planar.scope_ref`'s `scope_kind` onto this module's
/// own `plan_scope_kind`. A 1:1 mapping kept explicit so this module's
/// public API is unaffected by the extraction (plan 996 task 6089, D19).
auto to_plan_kind(scope_ref::scope_kind k) -> plan_scope_kind {
  switch (k) {
  case scope_ref::scope_kind::global:
    return plan_scope_kind::global;
  case scope_ref::scope_kind::association:
    return plan_scope_kind::association;
  case scope_ref::scope_kind::repo:
    return plan_scope_kind::repo;
  }
  return plan_scope_kind::global; // unreachable
}

auto to_plan_error(scope_ref::error e) -> plan_error {
  switch (e) {
  case scope_ref::error::query_failed:
    return plan_error::query_failed;
  case scope_ref::error::slug_not_found:
    return plan_error::slug_not_found;
  }
  return plan_error::query_failed; // unreachable
}

/// @brief Resolve `scope` (when present) to a `(kind,id)` pair, else
/// global. Delegates to `planar.scope_ref::resolve` (plan 996 task 6089,
/// D19) for the `"global"` / `"repo:<slug>"` / `"assoc:<slug>"` /
/// bare-association grammar and its DB lookup — that algorithm now lives
/// in exactly one place, shared with `engine_identity`'s
/// `resolve_slug`. Only the "no scope at all -> global" fold (a plan/task
/// CRUD concern: an absent `--scope` argument, distinct from the literal
/// string `"global"`) stays local to this module.
auto resolve_scope_or_global(db::connection& conn, const std::optional<std::string>& scope)
    -> std::expected<std::pair<plan_scope_kind, std::optional<std::int64_t>>, plan_error> {
  if (!scope.has_value()) {
    return std::make_pair(plan_scope_kind::global, std::optional<std::int64_t>{});
  }
  auto resolved = scope_ref::resolve(conn, *scope);
  if (!resolved) {
    return std::unexpected(to_plan_error(resolved.error()));
  }
  return std::make_pair(to_plan_kind(resolved->kind), resolved->id);
}

auto slugify(std::string_view title) -> std::string {
  std::string out;
  bool        last_dash = true; // suppress leading dashes
  for (const unsigned char ch : title) {
    const unsigned char lower = static_cast<unsigned char>(std::tolower(ch));
    if (std::isalnum(lower) != 0) {
      out.push_back(static_cast<char>(lower));
      last_dash = false;
    } else if (!last_dash) {
      out.push_back('-');
      last_dash = true;
    }
  }
  if (!out.empty() && out.back() == '-') {
    out.pop_back();
  }
  return out;
}

constexpr std::string_view k_select_columns =
    "select id, scope_kind, scope_id, title, slug, summary, status, parent_plan_id, created_at, updated_at from plans";

auto read_row(db::statement& stmt) -> std::expected<plan, plan_error> {
  const auto      scope_kind_text = stmt.column_text(1);
  plan_scope_kind sk;
  if (scope_kind_text == "global") {
    sk = plan_scope_kind::global;
  } else if (scope_kind_text == "association") {
    sk = plan_scope_kind::association;
  } else if (scope_kind_text == "repo") {
    sk = plan_scope_kind::repo;
  } else {
    return std::unexpected(plan_error::query_failed);
  }

  const auto status_text = stmt.column_text(6);
  const auto status      = plan_status_from_text(status_text);
  if (!status.has_value()) {
    return std::unexpected(plan_error::query_failed);
  }

  return plan{
      .id             = stmt.column_int64(0),
      .scope_kind     = sk,
      .scope_id       = stmt.is_null(2) ? std::optional<std::int64_t>{} : std::optional<std::int64_t>{stmt.column_int64(2)},
      .title          = stmt.column_text(3),
      .slug           = stmt.column_text(4),
      .summary        = stmt.is_null(5) ? std::optional<std::string>{} : std::optional<std::string>{stmt.column_text(5)},
      .status         = *status,
      .parent_plan_id = stmt.is_null(7) ? std::optional<std::int64_t>{} : std::optional<std::int64_t>{stmt.column_int64(7)},
      .created_at     = stmt.column_text(8),
      .updated_at     = stmt.column_text(9),
  };
}

auto would_create_parent_cycle(db::connection& conn, std::int64_t plan_id, std::int64_t parent_id)
    -> std::expected<bool, plan_error> {
  auto stmt = conn.prepare("with recursive ancestors(id, parent_plan_id) as ("
                           "  select id, parent_plan_id from plans where id = ?"
                           "  union"
                           "  select p.id, p.parent_plan_id from plans p"
                           "  join ancestors a on p.id = a.parent_plan_id"
                           ") select count(*) from ancestors where id = ?");
  if (!stmt) {
    return std::unexpected(plan_error::query_failed);
  }
  if (auto b1 = stmt->bind_int64(1, parent_id); !b1) {
    return std::unexpected(plan_error::query_failed);
  }
  if (auto b2 = stmt->bind_int64(2, plan_id); !b2) {
    return std::unexpected(plan_error::query_failed);
  }
  auto step = stmt->step();
  if (!step) {
    return std::unexpected(plan_error::query_failed);
  }
  if (*step != db::step_result::row) {
    return false;
  }
  return stmt->column_int64(0) > 0;
}

struct task_aggregate {
  std::int64_t todo      = 0;
  std::int64_t doing     = 0;
  std::int64_t blocked   = 0;
  std::int64_t done      = 0;
  std::int64_t cancelled = 0;

  [[nodiscard]] auto total() const -> std::int64_t {
    return todo + doing + blocked + done + cancelled;
  }
  [[nodiscard]] auto all_terminal() const -> bool {
    return total() > 0 && todo == 0 && doing == 0 && blocked == 0;
  }
  [[nodiscard]] auto any_active() const -> bool {
    return doing > 0 || blocked > 0;
  }
};

auto read_task_aggregate(db::connection& conn, std::int64_t plan_id) -> std::expected<task_aggregate, plan_error> {
  auto stmt = conn.prepare("select status, count(*) from tasks where plan_id = ? group by status");
  if (!stmt) {
    return std::unexpected(plan_error::query_failed);
  }
  if (auto bound = stmt->bind_int64(1, plan_id); !bound) {
    return std::unexpected(plan_error::query_failed);
  }
  task_aggregate agg;
  for (;;) {
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(plan_error::query_failed);
    }
    if (*step == db::step_result::done) {
      break;
    }
    const auto st = stmt->column_text(0);
    const auto n  = stmt->column_int64(1);
    if (st == "todo") {
      agg.todo = n;
    } else if (st == "doing") {
      agg.doing = n;
    } else if (st == "blocked") {
      agg.blocked = n;
    } else if (st == "done") {
      agg.done = n;
    } else if (st == "cancelled") {
      agg.cancelled = n;
    }
  }
  return agg;
}

/// @brief Compute the auto-promotion target. `std::nullopt` means no
/// transition is warranted. Mirrors zig's `computeTarget`.
auto compute_target(plan_status current, const task_aggregate& agg, bool is_anchor) -> std::optional<plan_status> {
  if (agg.total() == 0) {
    return std::nullopt;
  }
  switch (current) {
  case plan_status::draft:
    if (agg.all_terminal()) {
      return is_anchor ? plan_status::active : plan_status::done;
    }
    if (agg.any_active()) {
      return plan_status::active;
    }
    return std::nullopt;
  case plan_status::active:
    if (agg.all_terminal()) {
      return is_anchor ? std::optional<plan_status>{} : plan_status::done;
    }
    return std::nullopt;
  case plan_status::done:
    if (!agg.all_terminal()) {
      return plan_status::active;
    }
    return std::nullopt;
  default:
    return std::nullopt;
  }
}

} // namespace

auto plan_status_from_text(std::string_view s) -> std::optional<plan_status> {
  if (s == "draft") {
    return plan_status::draft;
  }
  if (s == "active") {
    return plan_status::active;
  }
  if (s == "paused") {
    return plan_status::paused;
  }
  if (s == "done") {
    return plan_status::done;
  }
  if (s == "abandoned") {
    return plan_status::abandoned;
  }
  return std::nullopt;
}

auto plan_status_to_text(plan_status s) -> std::string_view {
  switch (s) {
  case plan_status::draft:
    return "draft";
  case plan_status::active:
    return "active";
  case plan_status::paused:
    return "paused";
  case plan_status::done:
    return "done";
  case plan_status::abandoned:
    return "abandoned";
  }
  return "draft"; // unreachable
}

auto create_plan(db::connection& conn, const plan_create_args& args) -> std::expected<plan, plan_error> {
  auto scope_ref = resolve_scope_or_global(conn, args.scope);
  if (!scope_ref) {
    return std::unexpected(scope_ref.error());
  }
  const auto slug = args.slug.value_or(slugify(args.title));

  auto stmt = conn.prepare("insert into plans (scope_kind, scope_id, title, slug, summary, status, parent_plan_id) "
                           "values (?, ?, ?, ?, ?, ?, ?) returning id");
  if (!stmt) {
    return std::unexpected(plan_error::query_failed);
  }
  if (auto b = stmt->bind_text(1, scope_kind_to_text(scope_ref->first)); !b) {
    return std::unexpected(plan_error::query_failed);
  }
  auto b2 = scope_ref->second.has_value() ? stmt->bind_int64(2, *scope_ref->second) : stmt->bind_null(2);
  if (!b2) {
    return std::unexpected(plan_error::query_failed);
  }
  if (auto b = stmt->bind_text(3, args.title); !b) {
    return std::unexpected(plan_error::query_failed);
  }
  if (auto b = stmt->bind_text(4, slug); !b) {
    return std::unexpected(plan_error::query_failed);
  }
  auto b5 = args.summary.has_value() ? stmt->bind_text(5, *args.summary) : stmt->bind_null(5);
  if (!b5) {
    return std::unexpected(plan_error::query_failed);
  }
  if (auto b = stmt->bind_text(6, plan_status_to_text(args.status)); !b) {
    return std::unexpected(plan_error::query_failed);
  }
  auto b7 = args.parent_plan_id.has_value() ? stmt->bind_int64(7, *args.parent_plan_id) : stmt->bind_null(7);
  if (!b7) {
    return std::unexpected(plan_error::query_failed);
  }

  auto step = stmt->step();
  if (!step) {
    if (is_unique_violation(step.error())) {
      return std::unexpected(plan_error::slug_conflict);
    }
    return std::unexpected(plan_error::query_failed);
  }

  const auto id = stmt->column_int64(0);
  // ORACLE: `create|plan|1|create plan 'Plan One'` -- the TITLE, not the
  // slug, in single quotes.
  if (auto a = record_audit(conn, audit::record_args{.verb    = audit::verb::create,
                                                     .entity  = {.kind = "plan", .id = id},
                                                     .summary = std::format("create plan '{}'", args.title)});
      !a) {
    return std::unexpected(a.error());
  }
  return show_plan(conn, id);
}

auto show_plan(db::connection& conn, std::int64_t id) -> std::expected<plan, plan_error> {
  auto stmt = conn.prepare(std::format("{} where id = ?", k_select_columns));
  if (!stmt) {
    return std::unexpected(plan_error::query_failed);
  }
  if (auto bound = stmt->bind_int64(1, id); !bound) {
    return std::unexpected(plan_error::query_failed);
  }
  auto step = stmt->step();
  if (!step) {
    return std::unexpected(plan_error::query_failed);
  }
  if (*step != db::step_result::row) {
    return std::unexpected(plan_error::not_found);
  }
  return read_row(*stmt);
}

auto list_plans(db::connection& conn, const plan_list_filter& filter) -> std::expected<std::vector<plan>, plan_error> {
  // `scope` and every entry of `scopes` resolve into ONE disjunction, in
  // that order — mirrors zig's `list`, which appends `filter.scope` first
  // and then walks `filter.scopes`. An unresolvable slug anywhere in the
  // set fails the whole call rather than being skipped: a read verb that
  // quietly drops one member of its scope set returns a short list that
  // looks complete.
  std::vector<std::pair<plan_scope_kind, std::optional<std::int64_t>>> scope_refs;
  if (filter.scope.has_value()) {
    auto resolved = resolve_scope_or_global(conn, filter.scope);
    if (!resolved) {
      return std::unexpected(resolved.error());
    }
    scope_refs.push_back(*resolved);
  }
  for (const auto& s : filter.scopes) {
    auto resolved = resolve_scope_or_global(conn, std::optional<std::string>{s});
    if (!resolved) {
      return std::unexpected(resolved.error());
    }
    scope_refs.push_back(*resolved);
  }

  std::string sql = "select id, scope_kind, scope_id, title, slug, summary, status, parent_plan_id, created_at, "
                    "updated_at from plans where 1 = 1";
  if (filter.statuses.empty()) {
    // An EMPTY status filter is not "no filter" — it defaults to the three
    // OPEN statuses, exactly as `list_tasks` below defaults to
    // ('todo','doing','blocked'). This arm was missing until task 6141 and
    // the omission was invisible until `plan list` and `plan
    // recompute-status --all` were wired: no unit test distinguished
    // "matches everything" from "matches the open set" because no caller
    // existed. Two operator-visible consequences, both caught by the
    // oracle-differential run rather than by any exit code:
    //   - `plan list` listed done/abandoned plans the oracle hides.
    //   - `plan recompute-status --all` walked terminal plans and
    //     RESURRECTED them (`plan 1: done -> active`) — a wrong WRITE, not
    //     merely a wrong listing.
    sql += " and status in ('draft','active','paused')";
  } else {
    sql += " and status in (";
    for (std::size_t i = 0; i < filter.statuses.size(); ++i) {
      if (i > 0) {
        sql += ", ";
      }
      sql += "?";
    }
    sql += ")";
  }
  if (filter.parent_plan_id.has_value()) {
    sql += " and parent_plan_id = ?";
  }
  if (!scope_refs.empty()) {
    // `global` matches on scope_kind ALONE — the oracle emits a bare
    // `scope_kind = 'global'` with no `scope_id is null` conjunct
    // (plan.zig / task.zig's `list`). Reproduced rather than tightened:
    // adding the null check would change which rows a corrupt
    // global-with-a-scope_id row matches, and D2 says reproduce.
    sql += " and (";
    for (std::size_t i = 0; i < scope_refs.size(); ++i) {
      if (i > 0) {
        sql += " or ";
      }
      if (scope_refs[i].first == plan_scope_kind::global) {
        sql += "scope_kind = 'global'";
      } else {
        sql += "(scope_kind = ? and scope_id = ?)";
      }
    }
    sql += ")";
  }
  sql += " order by id";

  auto stmt = conn.prepare(sql);
  if (!stmt) {
    return std::unexpected(plan_error::query_failed);
  }
  int idx = 1;
  for (const auto s : filter.statuses) {
    if (auto b = stmt->bind_text(idx++, plan_status_to_text(s)); !b) {
      return std::unexpected(plan_error::query_failed);
    }
  }
  if (filter.parent_plan_id.has_value()) {
    if (auto b = stmt->bind_int64(idx++, *filter.parent_plan_id); !b) {
      return std::unexpected(plan_error::query_failed);
    }
  }
  for (const auto& ref : scope_refs) {
    if (ref.first == plan_scope_kind::global) {
      continue; // literal in the SQL; no placeholder to fill.
    }
    if (!ref.second.has_value()) {
      // A non-global ref with no row id cannot be turned into a predicate.
      // The oracle unwraps the optional unconditionally here and would
      // panic; refuse instead of binding a placeholder id, which would
      // silently match scope_id 0 (i.e. nothing) and return a short list.
      return std::unexpected(plan_error::query_failed);
    }
    if (auto b = stmt->bind_text(idx++, scope_kind_to_text(ref.first)); !b) {
      return std::unexpected(plan_error::query_failed);
    }
    if (auto b = stmt->bind_int64(idx++, *ref.second); !b) {
      return std::unexpected(plan_error::query_failed);
    }
  }

  std::vector<plan> out;
  for (;;) {
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(plan_error::query_failed);
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

auto update_plan(db::connection& conn, std::int64_t id, const plan_update_args& patch) -> std::expected<plan, plan_error> {
  std::optional<std::pair<plan_scope_kind, std::optional<std::int64_t>>> scope_ref;
  if (patch.scope.has_value()) {
    auto resolved = resolve_scope_or_global(conn, patch.scope);
    if (!resolved) {
      return std::unexpected(resolved.error());
    }
    scope_ref = *resolved;
  }

  auto tx = conn.begin_transaction();
  if (!tx) {
    return std::unexpected(plan_error::query_failed);
  }

  auto current = show_plan(conn, id);
  if (!current) {
    return std::unexpected(current.error());
  }

  if (patch.status.has_value()) {
    auto checked =
        check_transition(transition_kind::plan, plan_status_to_text(current->status), plan_status_to_text(*patch.status), false);
    if (!checked) {
      return std::unexpected(checked.error() == transition_error::unknown_status ? plan_error::unknown_status
                                                                                 : plan_error::illegal_transition);
    }
  }
  if (patch.parent_plan_id.has_value()) {
    auto cycle = would_create_parent_cycle(conn, id, *patch.parent_plan_id);
    if (!cycle) {
      return std::unexpected(cycle.error());
    }
    if (*cycle) {
      return std::unexpected(plan_error::invalid_parent_cycle);
    }
  }

  std::string               sql = "update plans set ";
  std::vector<std::string>  text_params;
  std::vector<std::int64_t> int_params;
  bool                      first = true;
  auto                      sep   = [&] {
    if (!first) {
      sql += ", ";
    }
    first = false;
  };

  // Bind order tracked as an ordered list of (kind, index-into-text/int).
  enum class param_kind : std::uint8_t { text, int64, null };
  std::vector<param_kind> order;

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
  if (patch.slug.has_value()) {
    sep();
    sql += "slug = ?";
    order.push_back(param_kind::text);
    text_params.push_back(*patch.slug);
  }
  if (patch.summary.has_value()) {
    sep();
    sql += "summary = ?";
    order.push_back(param_kind::text);
    text_params.push_back(*patch.summary);
  }
  if (patch.status.has_value()) {
    sep();
    sql += "status = ?";
    order.push_back(param_kind::text);
    text_params.push_back(std::string(plan_status_to_text(*patch.status)));
  }
  if (patch.parent_plan_id.has_value()) {
    sep();
    sql += "parent_plan_id = ?";
    order.push_back(param_kind::int64);
    int_params.push_back(*patch.parent_plan_id);
  } else if (patch.clear_parent) {
    sep();
    sql += "parent_plan_id = null";
  }

  if (first) {
    // No-op patch: mirrors zig's early-return-with-fresh-show. This
    // returns BEFORE the audit write below on purpose -- a patch that
    // changes nothing writes no `audit_log` row in the oracle either.
    return show_plan(conn, id);
  }

  sep();
  sql += "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') where id = ?";

  auto stmt = conn.prepare(sql);
  if (!stmt) {
    return std::unexpected(plan_error::query_failed);
  }
  int         bind_idx = 1;
  std::size_t text_i   = 0;
  std::size_t int_i    = 0;
  for (const auto k : order) {
    switch (k) {
    case param_kind::text:
      if (auto b = stmt->bind_text(bind_idx++, text_params[text_i++]); !b) {
        return std::unexpected(plan_error::query_failed);
      }
      break;
    case param_kind::int64:
      if (auto b = stmt->bind_int64(bind_idx++, int_params[int_i++]); !b) {
        return std::unexpected(plan_error::query_failed);
      }
      break;
    case param_kind::null:
      break;
    }
  }
  if (auto b = stmt->bind_int64(bind_idx++, id); !b) {
    return std::unexpected(plan_error::query_failed);
  }

  auto step = stmt->step();
  if (!step) {
    if (is_unique_violation(step.error())) {
      return std::unexpected(plan_error::slug_conflict);
    }
    return std::unexpected(plan_error::query_failed);
  }

  // ORACLE: a patch carrying a status writes `status_change`; any other
  // patch writes `update`. BOTH carry a NULL summary -- captured as
  // `update|plan|1|<NULL>`. Inside the transaction, so a failed commit
  // takes the audit row with it.
  if (auto a = record_audit(
          conn, audit::record_args{.verb   = patch.status.has_value() ? audit::verb::status_change : audit::verb::update,
                                   .entity = {.kind = "plan", .id = id}});
      !a) {
    return std::unexpected(a.error());
  }

  auto updated = show_plan(conn, id);
  if (!updated) {
    return std::unexpected(updated.error());
  }
  if (auto committed = tx->commit(); !committed) {
    return std::unexpected(plan_error::query_failed);
  }
  return updated;
}

auto recompute_status(db::connection& conn, std::int64_t plan_id) -> std::expected<recompute_result, plan_error> {
  plan_status current_status{};
  bool        is_anchor = false;
  {
    auto stmt = conn.prepare("select status, parent_plan_id from plans where id = ?");
    if (!stmt) {
      return std::unexpected(plan_error::query_failed);
    }
    if (auto bound = stmt->bind_int64(1, plan_id); !bound) {
      return std::unexpected(plan_error::query_failed);
    }
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(plan_error::query_failed);
    }
    if (*step != db::step_result::row) {
      return std::unexpected(plan_error::not_found);
    }
    const auto status_text = stmt->column_text(0);
    const auto status      = plan_status_from_text(status_text);
    if (!status.has_value()) {
      return std::unexpected(plan_error::query_failed);
    }
    current_status = *status;
    is_anchor      = stmt->is_null(1);
  }

  if (current_status == plan_status::paused || current_status == plan_status::abandoned) {
    return recompute_result{
        .plan_id       = plan_id,
        .status_before = current_status,
        .status_after  = current_status,
        .flipped       = false,
    };
  }

  auto agg = read_task_aggregate(conn, plan_id);
  if (!agg) {
    return std::unexpected(agg.error());
  }

  const auto target = compute_target(current_status, *agg, is_anchor);
  if (!target.has_value() || *target == current_status) {
    return recompute_result{
        .plan_id       = plan_id,
        .status_before = current_status,
        .status_after  = current_status,
        .flipped       = false,
    };
  }

  // INTENTIONAL bypass of check_transition — see this function's doc
  // comment and zig plan.zig:760-771: this is an engine-internal
  // aggregate roll-up whose target is computed by compute_target(), which
  // only emits edges the aggregate matrix considers valid.
  auto stmt = conn.prepare("update plans set status = ?, updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') where id = ?");
  if (!stmt) {
    return std::unexpected(plan_error::query_failed);
  }
  if (auto b = stmt->bind_text(1, plan_status_to_text(*target)); !b) {
    return std::unexpected(plan_error::query_failed);
  }
  if (auto b = stmt->bind_int64(2, plan_id); !b) {
    return std::unexpected(plan_error::query_failed);
  }
  auto step = stmt->step();
  if (!step) {
    return std::unexpected(plan_error::query_failed);
  }

  // ORACLE, captured verbatim from a `task update --status doing` that
  // rolled its plan draft -> active:
  //   recompute plan 1: draft -> active; tasks todo=1 doing=1 blocked=0 done=0 cancelled=0
  // The arrow is U+2192 RIGHTWARDS ARROW, not "->", and every one of the
  // five counters is present even at zero.
  if (auto a = record_audit(conn, audit::record_args{.verb    = audit::verb::status_change,
                                                     .entity  = {.kind = "plan", .id = plan_id},
                                                     .summary = std::format("recompute plan {}: {} → {}; tasks todo={} doing={} "
                                                                            "blocked={} done={} cancelled={}",
                                                                            plan_id, plan_status_to_text(current_status),
                                                                            plan_status_to_text(*target), agg->todo, agg->doing,
                                                                            agg->blocked, agg->done, agg->cancelled)});
      !a) {
    return std::unexpected(a.error());
  }

  return recompute_result{
      .plan_id       = plan_id,
      .status_before = current_status,
      .status_after  = *target,
      .flipped       = true,
  };
}

auto list_plans_touching(db::connection& conn, std::int64_t repo_id, const plan_list_filter& filter)
    -> std::expected<std::vector<plan>, plan_error> {
  std::vector<std::pair<plan_scope_kind, std::optional<std::int64_t>>> scope_refs;
  if (filter.scope.has_value()) {
    auto resolved = resolve_scope_or_global(conn, filter.scope);
    if (!resolved) {
      return std::unexpected(resolved.error());
    }
    scope_refs.push_back(*resolved);
  }
  for (const auto& s : filter.scopes) {
    auto resolved = resolve_scope_or_global(conn, std::optional<std::string>{s});
    if (!resolved) {
      return std::unexpected(resolved.error());
    }
    scope_refs.push_back(*resolved);
  }

  // The direct-repo-scope branch is ALL-OR-NOTHING against the scope set —
  // see the header. With no scope set at all it stays on.
  bool branch_direct = true;
  if (!scope_refs.empty()) {
    branch_direct = false;
    for (const auto& ref : scope_refs) {
      if (ref.first == plan_scope_kind::repo && (!ref.second.has_value() || *ref.second == repo_id)) {
        branch_direct = true;
        break;
      }
    }
  }

  // The status predicate is emitted per BRANCH, not once for the union, so
  // both arms default to the open set independently. Losing it on either
  // arm resurrects terminal plans into the listing.
  auto const status_clause = [&]() -> std::string {
    if (filter.statuses.empty()) {
      return " and status in ('draft','active','paused')";
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
  }();

  std::string sql = "select * from (";
  sql += k_select_columns;
  sql += " where 1 = 1";
  if (branch_direct) {
    sql += " and scope_kind = 'repo' and scope_id = ?";
    if (filter.parent_plan_id.has_value()) {
      sql += " and parent_plan_id = ?";
    }
    sql += status_clause;
  } else {
    // A branch that is off is emitted as `1 = 0` rather than dropped, so
    // the UNION keeps both arms and the column list stays identical.
    sql += " and 1 = 0";
  }
  sql += " union ";
  sql += k_select_columns;
  sql += " where 1 = 1";
  sql += " and id in (select from_id from entity_links where from_kind = 'plan' and to_kind = 'repo' and to_id = ? and "
         "relationship = 'touches')";
  if (filter.parent_plan_id.has_value()) {
    sql += " and parent_plan_id = ?";
  }
  sql += status_clause;
  if (!scope_refs.empty()) {
    sql += " and (";
    for (std::size_t i = 0; i < scope_refs.size(); ++i) {
      if (i > 0) {
        sql += " or ";
      }
      if (scope_refs[i].first == plan_scope_kind::global) {
        sql += "scope_kind = 'global'";
      } else {
        sql += "(scope_kind = ? and scope_id = ?)";
      }
    }
    sql += ")";
  }
  sql += ") order by id";

  auto stmt = conn.prepare(sql);
  if (!stmt) {
    return std::unexpected(plan_error::query_failed);
  }
  int  idx       = 1;
  auto bind_int  = [&](std::int64_t v) { return stmt->bind_int64(idx++, v).has_value(); };
  auto bind_text = [&](std::string_view v) { return stmt->bind_text(idx++, v).has_value(); };

  if (branch_direct) {
    if (!bind_int(repo_id)) {
      return std::unexpected(plan_error::query_failed);
    }
    if (filter.parent_plan_id.has_value() && !bind_int(*filter.parent_plan_id)) {
      return std::unexpected(plan_error::query_failed);
    }
    for (const auto s : filter.statuses) {
      if (!bind_text(plan_status_to_text(s))) {
        return std::unexpected(plan_error::query_failed);
      }
    }
  }
  if (!bind_int(repo_id)) {
    return std::unexpected(plan_error::query_failed);
  }
  if (filter.parent_plan_id.has_value() && !bind_int(*filter.parent_plan_id)) {
    return std::unexpected(plan_error::query_failed);
  }
  for (const auto s : filter.statuses) {
    if (!bind_text(plan_status_to_text(s))) {
      return std::unexpected(plan_error::query_failed);
    }
  }
  for (const auto& ref : scope_refs) {
    if (ref.first == plan_scope_kind::global) {
      continue;
    }
    if (!ref.second.has_value()) {
      return std::unexpected(plan_error::query_failed);
    }
    if (!bind_text(scope_kind_to_text(ref.first)) || !bind_int(*ref.second)) {
      return std::unexpected(plan_error::query_failed);
    }
  }

  std::vector<plan> out;
  for (;;) {
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(plan_error::query_failed);
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

auto render_text(const plan& p) -> std::string {
  std::string out;
  out += std::format("id:       {}\n", p.id);
  out += std::format("title:    {}\n", p.title);
  out += std::format("slug:     {}\n", p.slug);
  out += std::format("status:   {}\n", plan_status_to_text(p.status));
  out += std::format("scope:    {}", scope_kind_to_text(p.scope_kind));
  if (p.scope_id.has_value()) {
    out += std::format(":{}", *p.scope_id);
  }
  out += "\n";
  // `parent` BEFORE `summary`, which is not the struct's field order —
  // it is the oracle's print order (plan.zig:573-580) and was captured
  // from a run carrying both.
  if (p.parent_plan_id.has_value()) {
    out += std::format("parent:   {}\n", *p.parent_plan_id);
  }
  if (p.summary.has_value()) {
    out += std::format("summary:  {}\n", *p.summary);
  }
  out += std::format("created:  {}\n", p.created_at);
  out += std::format("updated:  {}\n", p.updated_at);
  return out;
}

auto render_json(const plan& p) -> std::string {
  auto const opt_int = [](std::optional<std::int64_t> v) -> std::string {
    return v.has_value() ? std::format("{}", *v) : std::string{"null"};
  };
  auto const opt_str = [](const std::optional<std::string>& v) -> std::string {
    return v.has_value() ? json_string(*v) : std::string{"null"};
  };
  return std::format(R"({{"id":{},"scope_kind":"{}","scope_id":{},"title":{},"slug":{},"summary":{},)"
                     R"("status":"{}","parent_plan_id":{},"created_at":{},"updated_at":{}}})",
                     p.id, scope_kind_to_text(p.scope_kind), opt_int(p.scope_id), json_string(p.title), json_string(p.slug),
                     opt_str(p.summary), plan_status_to_text(p.status), opt_int(p.parent_plan_id), json_string(p.created_at),
                     json_string(p.updated_at));
}

auto render_list_text(std::span<const plan> plans) -> std::string {
  if (plans.empty()) {
    return "(no plans)\n";
  }
  std::string out;
  for (const auto& p : plans) {
    out += std::format("{:>5}  {:<10}  {:<24}  {}\n", p.id, plan_status_to_text(p.status), p.slug, p.title);
  }
  return out;
}

auto render_list_json(std::span<const plan> plans) -> std::string {
  std::string out = "[";
  for (std::size_t i = 0; i < plans.size(); ++i) {
    if (i > 0) {
      out += ",";
    }
    out += render_json(plans[i]);
  }
  out += "]";
  return out;
}

} // namespace planar::engine::planning
