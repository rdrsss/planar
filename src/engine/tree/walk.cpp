/// @file walk.cpp
/// @brief Implementation of `planar.engine.tree.walk`.

module planar.engine.tree.walk;

import std;
import planar.activity_rollup;
import planar.db;
import planar.json_text;
import planar.scope_ref;

namespace planar::engine::tree {

using json_text::append_json_string;

namespace {

// =========================================================================
// Filter helpers
// =========================================================================

/// @brief `true` when the walk must not descend past `depth`.
///
/// ANY `max_depth <= 0` is UNBOUNDED, not "zero levels". Captured:
/// `tree --depth 0` renders the identical bytes to `tree --depth -1`.
auto depth_at_max(std::int64_t depth, std::int64_t max_depth) -> bool {
  if (max_depth <= 0) {
    return false;
  }
  return depth >= max_depth;
}

/// @brief `true` when `kind` passes the kind filter. Empty filter -> all.
auto kind_allowed(std::string_view kind, std::span<const std::string> kinds) -> bool {
  if (kinds.empty()) {
    return true;
  }
  return std::ranges::find(kinds, kind) != kinds.end();
}

/// @brief `true` when `status` passes the status filter. Empty filter -> any.
///
/// Note this is a filter over the empty SET, not the empty STRING: a
/// `--status ""` reaches here as a one-element vector holding "", which
/// matches only rows whose status is literally empty — hence the captured
/// "empty tree, zero counts" behavior, not "everything".
auto status_allowed(std::string_view status, std::span<const std::string> statuses) -> bool {
  if (statuses.empty()) {
    return true;
  }
  return std::ranges::find(statuses, status) != statuses.end();
}

// =========================================================================
// Activity rollup — MOVED to `planar.activity_rollup` at task 6282
//
// The three queries lived here as a deliberate documented duplicate because
// `engine_tree -> engine_runtime` is an engine<->engine edge the architecture
// guard refuses. D19 extraction to layer 1 replaced that duplicate with one
// implementation both buckets can reach downward; the three call sites below
// are now calls, not copies.
// =========================================================================

using planar::activity_rollup::activity_for;

// =========================================================================
// Row vocabulary
// =========================================================================

struct plan_row {
  std::int64_t id = 0;
  std::string  title;
  std::string  slug;
  std::string  status;
  std::string  created_at;
  std::string  updated_at;
};

struct task_row {
  std::int64_t id = 0;
  std::string  title;
  std::string  status;
  std::int64_t priority = 0;
  std::string  created_at;
  std::string  updated_at;
};

// Forward declarations: the plan/task builders are mutually recursive with
// their child queries.
auto build_plan_node(db::connection& conn, const plan_row& row, const tree_filter& filter, std::int64_t depth)
    -> std::expected<std::optional<node>, tree_error>;
auto build_task_node(db::connection& conn, const task_row& row, const tree_filter& filter, std::int64_t depth)
    -> std::expected<std::optional<node>, tree_error>;

// =========================================================================
// Scope label
// =========================================================================

/// @brief Build the scope root's display label: `global`, `assoc:<slug>`
/// or `repo:<slug>`.
///
/// An id that resolves to no row yields `<prefix>unknown`, which is the
/// oracle's behavior and not an error.
auto build_scope_label(db::connection& conn, std::string_view scope_kind, std::optional<std::int64_t> scope_id)
    -> std::expected<std::string, tree_error> {
  if (scope_kind == "global") {
    return std::string{"global"};
  }
  bool const  is_assoc = scope_kind == "association";
  auto const* sql      = is_assoc ? "select slug from associations where id = ?" : "select slug from projects where id = ?";
  std::string prefix   = is_assoc ? "assoc:" : "repo:";

  if (scope_id.has_value()) {
    auto stmt = conn.prepare(sql);
    if (!stmt) {
      return std::unexpected(tree_error::query_failed);
    }
    if (auto bound = stmt->bind_int64(1, *scope_id); !bound) {
      return std::unexpected(tree_error::query_failed);
    }
    auto stepped = stmt->step();
    if (!stepped) {
      return std::unexpected(tree_error::query_failed);
    }
    if (*stepped == db::step_result::row) {
      return prefix + stmt->column_text(0);
    }
  }
  return prefix + "unknown";
}

/// @brief Assemble a scope root around an already-walked child list.
auto make_scope_node(std::string_view scope_kind, std::optional<std::int64_t> scope_id, std::string label,
                     std::vector<node> children) -> node {
  return node{
      .kind        = "scope",
      .title       = label,
      .scope_kind  = std::string{scope_kind},
      .scope_id    = scope_id,
      .scope_label = std::move(label),
      .children    = std::move(children),
  };
}

// =========================================================================
// Derived entities (artifact / decision / test_scenario / question)
// =========================================================================

/// @brief Hydrate one `derives-from` leaf into a node, or absent when the
/// row is gone or its status is filtered out.
auto hydrate_derived(db::connection& conn, std::string_view db_kind, std::string_view display_kind, std::int64_t id,
                     const tree_filter& filter) -> std::expected<std::optional<node>, tree_error> {
  std::string sql;
  bool const  is_artifact = db_kind == "artifact";
  if (is_artifact) {
    sql = "select title, status, kind, coalesce(created_at,''), coalesce(updated_at,'') from artifacts where id = ?";
  } else {
    std::string_view table;
    if (db_kind == "decision") {
      table = "decisions";
    } else if (db_kind == "test_scenario") {
      table = "test_scenarios";
    } else if (db_kind == "question") {
      table = "questions";
    } else {
      return std::optional<node>{};
    }
    sql = std::format("select title, status, coalesce(created_at,''), coalesce(updated_at,'') from {} where id = ?", table);
  }

  auto stmt = conn.prepare(sql);
  if (!stmt) {
    return std::unexpected(tree_error::query_failed);
  }
  if (auto bound = stmt->bind_int64(1, id); !bound) {
    return std::unexpected(tree_error::query_failed);
  }
  auto stepped = stmt->step();
  if (!stepped) {
    return std::unexpected(tree_error::query_failed);
  }
  if (*stepped == db::step_result::done) {
    return std::optional<node>{};
  }

  std::string const title         = stmt->column_text(0);
  std::string const status        = stmt->column_text(1);
  std::string const artifact_kind = is_artifact ? stmt->column_text(2) : std::string{};
  std::string const created_at    = stmt->column_text(is_artifact ? 3 : 2);
  std::string const updated_at    = stmt->column_text(is_artifact ? 4 : 3);

  if (!status_allowed(status, filter.statuses)) {
    return std::optional<node>{};
  }

  return std::optional<node>{node{
      .kind          = std::string{display_kind},
      .id            = id,
      .title         = title,
      .status        = status,
      .artifact_kind = artifact_kind,
      .created_at    = created_at,
      .updated_at    = updated_at,
      .activity      = activity_for(conn, db_kind, id),
  }};
}

/// @brief Query the `derives-from` leaves hanging off one plan.
///
/// Ordered by `el.id` so derived entities surface in CREATION order,
/// interleaved across kinds, preserving the temporal narrative (cluster
/// K-tree-render-drift, plan 351). Not by kind, and not by entity id.
auto query_derived(db::connection& conn, std::int64_t plan_id, const tree_filter& filter)
    -> std::expected<std::vector<node>, tree_error> {
  auto stmt = conn.prepare("select el.from_kind, el.from_id from entity_links el "
                           "where el.relationship = 'derives-from' and el.to_kind = 'plan' and el.to_id = ? "
                           "and el.from_kind in ('artifact','decision','test_scenario','question') order by el.id");
  if (!stmt) {
    return std::unexpected(tree_error::query_failed);
  }
  if (auto bound = stmt->bind_int64(1, plan_id); !bound) {
    return std::unexpected(tree_error::query_failed);
  }

  struct link_row {
    std::string  db_kind;
    std::int64_t id = 0;
  };
  std::vector<link_row> links;
  while (true) {
    auto stepped = stmt->step();
    if (!stepped) {
      return std::unexpected(tree_error::query_failed);
    }
    if (*stepped == db::step_result::done) {
      break;
    }
    links.push_back(link_row{.db_kind = stmt->column_text(0), .id = stmt->column_int64(1)});
  }

  std::vector<node> out;
  for (auto const& link : links) {
    // `test_scenario` is stored under that name but DISPLAYS and FILTERS
    // as `scenario`; `--kind scenario` must match it.
    std::string_view const display_kind = link.db_kind == "test_scenario" ? std::string_view{"scenario"} : link.db_kind;
    if (!kind_allowed(display_kind, filter.kinds)) {
      continue;
    }
    auto hydrated = hydrate_derived(conn, link.db_kind, display_kind, link.id, filter);
    if (!hydrated) {
      return std::unexpected(hydrated.error());
    }
    if (hydrated->has_value()) {
      out.push_back(std::move(**hydrated));
    }
  }
  return out;
}

// =========================================================================
// Tasks
// =========================================================================

/// @brief Read task rows with a prepared, already-bound statement.
auto collect_task_rows(db::statement& stmt) -> std::expected<std::vector<task_row>, tree_error> {
  std::vector<task_row> rows;
  while (true) {
    auto stepped = stmt.step();
    if (!stepped) {
      return std::unexpected(tree_error::query_failed);
    }
    if (*stepped == db::step_result::done) {
      break;
    }
    rows.push_back(task_row{
        .id         = stmt.column_int64(0),
        .title      = stmt.column_text(1),
        .status     = stmt.column_text(2),
        .priority   = stmt.column_int64(3),
        .created_at = stmt.column_text(4),
        .updated_at = stmt.column_text(5),
    });
  }
  return rows;
}

/// @brief Subtasks of one task, `order by id`.
auto query_subtasks(db::connection& conn, std::int64_t parent_task_id, const tree_filter& filter, std::int64_t depth)
    -> std::expected<std::vector<node>, tree_error> {
  auto stmt = conn.prepare("select id, title, coalesce(status,''), priority, coalesce(created_at,''), "
                           "coalesce(updated_at,'') from tasks where parent_task_id = ? order by id");
  if (!stmt) {
    return std::unexpected(tree_error::query_failed);
  }
  if (auto bound = stmt->bind_int64(1, parent_task_id); !bound) {
    return std::unexpected(tree_error::query_failed);
  }
  auto rows = collect_task_rows(*stmt);
  if (!rows) {
    return std::unexpected(rows.error());
  }

  std::vector<node> out;
  for (auto const& row : *rows) {
    auto built = build_task_node(conn, row, filter, depth);
    if (!built) {
      return std::unexpected(built.error());
    }
    if (built->has_value()) {
      out.push_back(std::move(**built));
    }
  }
  return out;
}

/// @brief Top-level tasks of one plan.
///
/// The `left join` widens the plan's task set beyond `tasks.plan_id` to
/// include tasks joined by a `derives-from` edge, and the `distinct` is
/// what keeps a task carrying BOTH links from appearing twice. Transcribed
/// from the oracle's `queryTopTasks`; simplifying either half changes the
/// row set.
auto query_top_tasks(db::connection& conn, std::int64_t plan_id, const tree_filter& filter, std::int64_t depth)
    -> std::expected<std::vector<node>, tree_error> {
  auto stmt = conn.prepare("select distinct t.id, t.title, coalesce(t.status,''), t.priority, "
                           "coalesce(t.created_at,''), coalesce(t.updated_at,'') from tasks t "
                           "left join entity_links el on el.from_kind = 'task' and el.from_id = t.id "
                           "and el.to_kind = 'plan' and el.to_id = ? and el.relationship = 'derives-from' "
                           "where t.parent_task_id is null and (t.plan_id = ? or el.id is not null) order by t.id");
  if (!stmt) {
    return std::unexpected(tree_error::query_failed);
  }
  if (auto bound = stmt->bind_int64(1, plan_id); !bound) {
    return std::unexpected(tree_error::query_failed);
  }
  if (auto bound = stmt->bind_int64(2, plan_id); !bound) {
    return std::unexpected(tree_error::query_failed);
  }
  auto rows = collect_task_rows(*stmt);
  if (!rows) {
    return std::unexpected(rows.error());
  }

  std::vector<node> out;
  for (auto const& row : *rows) {
    auto built = build_task_node(conn, row, filter, depth);
    if (!built) {
      return std::unexpected(built.error());
    }
    if (built->has_value()) {
      out.push_back(std::move(**built));
    }
  }
  return out;
}

/// @brief Build one task node, or absent when it is filtered out and has
/// no surviving children.
///
/// A task kept ONLY as scaffold (filtered out itself but holding surviving
/// subtasks) is retained — that is why `--kind task` still shows the plan
/// spine above the tasks.
auto build_task_node(db::connection& conn, const task_row& row, const tree_filter& filter, std::int64_t depth)
    -> std::expected<std::optional<node>, tree_error> {
  bool const keep_self = kind_allowed("task", filter.kinds) && status_allowed(row.status, filter.statuses);

  std::vector<node> children;
  if (!depth_at_max(depth, filter.max_depth)) {
    auto subs = query_subtasks(conn, row.id, filter, depth + 1);
    if (!subs) {
      return std::unexpected(subs.error());
    }
    children = std::move(*subs);
  }

  if (!keep_self && children.empty()) {
    return std::optional<node>{};
  }

  return std::optional<node>{node{
      .kind       = "task",
      .id         = row.id,
      .title      = row.title,
      .status     = row.status,
      .priority   = row.priority,
      .created_at = row.created_at,
      .updated_at = row.updated_at,
      .children   = std::move(children),
      .activity   = activity_for(conn, "task", row.id),
  }};
}

// =========================================================================
// Plans
// =========================================================================

/// @brief Read plan rows with a prepared, already-bound statement.
auto collect_plan_rows(db::statement& stmt) -> std::expected<std::vector<plan_row>, tree_error> {
  std::vector<plan_row> rows;
  while (true) {
    auto stepped = stmt.step();
    if (!stepped) {
      return std::unexpected(tree_error::query_failed);
    }
    if (*stepped == db::step_result::done) {
      break;
    }
    rows.push_back(plan_row{
        .id         = stmt.column_int64(0),
        .title      = stmt.column_text(1),
        .slug       = stmt.column_text(2),
        .status     = stmt.column_text(3),
        .created_at = stmt.column_text(4),
        .updated_at = stmt.column_text(5),
    });
  }
  return rows;
}

/// @brief Child plans of one plan, `order by id`.
auto query_child_plans(db::connection& conn, std::int64_t parent_id, const tree_filter& filter, std::int64_t depth)
    -> std::expected<std::vector<node>, tree_error> {
  auto stmt = conn.prepare("select id, title, slug, status, coalesce(created_at,''), coalesce(updated_at,'') "
                           "from plans where parent_plan_id = ? order by id");
  if (!stmt) {
    return std::unexpected(tree_error::query_failed);
  }
  if (auto bound = stmt->bind_int64(1, parent_id); !bound) {
    return std::unexpected(tree_error::query_failed);
  }
  auto rows = collect_plan_rows(*stmt);
  if (!rows) {
    return std::unexpected(rows.error());
  }

  std::vector<node> out;
  for (auto const& row : *rows) {
    auto built = build_plan_node(conn, row, filter, depth);
    if (!built) {
      return std::unexpected(built.error());
    }
    if (built->has_value()) {
      out.push_back(std::move(**built));
    }
  }
  return out;
}

/// @brief Build one plan node, or absent when it does not survive.
///
/// The SCAFFOLD rule (the oracle's C-11 fix), and it is subtler than
/// "keep if it has children":
///   - kept on its own merits            -> keep
///   - filtered out, no children         -> drop
///   - filtered out, children present    -> keep ONLY if at least one
///     child is a plan or a task.
/// A plan surviving solely on derived leaves is dropped, which is what
/// stops `--kind artifact` from rendering the whole plan spine.
auto build_plan_node(db::connection& conn, const plan_row& row, const tree_filter& filter, std::int64_t depth)
    -> std::expected<std::optional<node>, tree_error> {
  bool const keep_self = kind_allowed("plan", filter.kinds) && status_allowed(row.status, filter.statuses);

  std::vector<node> children;
  if (!depth_at_max(depth, filter.max_depth)) {
    auto child_plans = query_child_plans(conn, row.id, filter, depth + 1);
    if (!child_plans) {
      return std::unexpected(child_plans.error());
    }
    std::ranges::move(*child_plans, std::back_inserter(children));

    auto tasks = query_top_tasks(conn, row.id, filter, depth + 1);
    if (!tasks) {
      return std::unexpected(tasks.error());
    }
    std::ranges::move(*tasks, std::back_inserter(children));

    // NOTE: derived leaves are NOT depth-gated by their own level in the
    // oracle — they are gathered whenever the plan itself descends.
    auto derived = query_derived(conn, row.id, filter);
    if (!derived) {
      return std::unexpected(derived.error());
    }
    std::ranges::move(*derived, std::back_inserter(children));
  }

  if (!keep_self) {
    if (children.empty()) {
      return std::optional<node>{};
    }
    bool const has_plan_or_task =
        std::ranges::any_of(children, [](node const& c) { return c.kind == "plan" || c.kind == "task"; });
    if (!has_plan_or_task) {
      return std::optional<node>{};
    }
  }

  return std::optional<node>{node{
      .kind       = "plan",
      .id         = row.id,
      .title      = row.title,
      .slug       = row.slug,
      .status     = row.status,
      .created_at = row.created_at,
      .updated_at = row.updated_at,
      .children   = std::move(children),
      .activity   = activity_for(conn, "plan", row.id),
  }};
}

/// @brief Walk the top-level plans (`parent_plan_id is null`) of one scope.
auto walk_scope(db::connection& conn, std::string_view scope_kind, std::optional<std::int64_t> scope_id,
                const tree_filter& filter, std::int64_t depth) -> std::expected<std::vector<node>, tree_error> {
  std::string sql = "select id, title, slug, status, coalesce(created_at,''), coalesce(updated_at,'') "
                    "from plans where parent_plan_id is null and scope_kind = ?";
  sql += scope_id.has_value() ? " and scope_id = ?" : " and scope_id is null";
  sql += " order by id";

  auto stmt = conn.prepare(sql);
  if (!stmt) {
    return std::unexpected(tree_error::query_failed);
  }
  if (auto bound = stmt->bind_text(1, scope_kind); !bound) {
    return std::unexpected(tree_error::query_failed);
  }
  if (scope_id.has_value()) {
    if (auto bound = stmt->bind_int64(2, *scope_id); !bound) {
      return std::unexpected(tree_error::query_failed);
    }
  }
  auto rows = collect_plan_rows(*stmt);
  if (!rows) {
    return std::unexpected(rows.error());
  }

  std::vector<node> out;
  for (auto const& row : *rows) {
    if (filter.root_plan_id.has_value() && row.id != *filter.root_plan_id) {
      continue;
    }
    auto built = build_plan_node(conn, row, filter, depth);
    if (!built) {
      return std::unexpected(built.error());
    }
    if (built->has_value()) {
      out.push_back(std::move(**built));
    }
  }
  return out;
}

/// @brief Walk one scope into a finished root node.
auto walk_one_root(db::connection& conn, std::string_view scope_kind, std::optional<std::int64_t> scope_id,
                   const tree_filter& filter) -> std::expected<node, tree_error> {
  auto label = build_scope_label(conn, scope_kind, scope_id);
  if (!label) {
    return std::unexpected(label.error());
  }
  auto children = walk_scope(conn, scope_kind, scope_id, filter, 0);
  if (!children) {
    return std::unexpected(children.error());
  }
  return make_scope_node(scope_kind, scope_id, std::move(*label), std::move(*children));
}

/// @brief Collect ids from a single-column `order by slug` query.
auto scope_ids(db::connection& conn, std::string_view sql) -> std::expected<std::vector<std::int64_t>, tree_error> {
  auto stmt = conn.prepare(sql);
  if (!stmt) {
    return std::unexpected(tree_error::query_failed);
  }
  std::vector<std::int64_t> ids;
  while (true) {
    auto stepped = stmt->step();
    if (!stepped) {
      return std::unexpected(tree_error::query_failed);
    }
    if (*stepped == db::step_result::done) {
      break;
    }
    ids.push_back(stmt->column_int64(0));
  }
  return ids;
}

/// @brief `--all-scopes`: global, then every association, then every
/// project — associations and projects each ordered BY SLUG, not by id.
///
/// This ordering is a plain SQL `order by slug`, deterministic and
/// reproducible. There is NO hash-map iteration anywhere in this walk (cf.
/// task 6274's `languages` field, which was a Zig `StringHashMap` artifact
/// and could not be pinned); every ordering here comes from an explicit
/// SQL `order by`, so all of it is safe to pin.
/// @brief Order ONE level of siblings in place. Does NOT recurse.
///
/// NOT RECURSIVE, for two reasons.
///
/// The behavioural one: a plan's children are a MIXED-KIND level -- tasks
/// alongside artifacts, decisions, scenarios and questions. Re-ordering that
/// by `updated_at` interleaves the kinds and makes the subtree harder to
/// read, not easier. `tree --sort updated` means "show me the plans, most
/// recently touched first", and that is the level this orders.
///
/// The honest one: a recursive version of this function made
/// `tree::build` throw `std::length_error("vector")` on 3 of this module's
/// fixtures. Sorting one level alone passes; recursing alone passes; doing
/// both throws. That was NOT root-caused, and filed rather than shipped --
/// task 6757. Reordering every level
/// was never the intended behaviour, so nothing is lost by not doing it, but
/// the crash is a real property of this data structure and someone should
/// understand it before a future change reaches for recursion here.
/// @param nodes The level to order, in place.
/// @param key The ordering.
auto sort_level(std::vector<node>& nodes, sort_key key) -> void {
  // STABLE, and that is the whole tie-break story: every query already emits
  // `order by id`, so equal `updated_at` / `created_at` values keep id order
  // instead of landing wherever the sort happens to put them.
  switch (key) {
  case sort_key::id:
    std::ranges::stable_sort(nodes, {}, &node::id);
    break;
  case sort_key::updated:
    std::ranges::stable_sort(nodes, std::ranges::greater{}, &node::updated_at);
    break;
  case sort_key::created:
    std::ranges::stable_sort(nodes, std::ranges::greater{}, &node::created_at);
    break;
  case sort_key::unsorted:
    break;
  }
}

auto build_all_scopes(db::connection& conn, const tree_filter& filter) -> std::expected<std::vector<node>, tree_error> {
  std::vector<node> roots;

  auto global_root = walk_one_root(conn, "global", std::nullopt, filter);
  if (!global_root) {
    return std::unexpected(global_root.error());
  }
  roots.push_back(std::move(*global_root));

  auto assoc_ids = scope_ids(conn, "select id from associations order by slug");
  if (!assoc_ids) {
    return std::unexpected(assoc_ids.error());
  }
  for (auto const id : *assoc_ids) {
    auto root = walk_one_root(conn, "association", id, filter);
    if (!root) {
      return std::unexpected(root.error());
    }
    roots.push_back(std::move(*root));
  }

  auto project_ids = scope_ids(conn, "select id from projects order by slug");
  if (!project_ids) {
    return std::unexpected(project_ids.error());
  }
  for (auto const id : *project_ids) {
    auto root = walk_one_root(conn, "repo", id, filter);
    if (!root) {
      return std::unexpected(root.error());
    }
    roots.push_back(std::move(*root));
  }

  // Each scope root's SUBTREE is ordered; the scope roots themselves keep
  // their global/association/repo sequence. A scope node's `created_at` and
  // `updated_at` are empty strings, so sorting the roots by either would be
  // sorting by nothing while visibly scrambling the sections.
  for (auto& root : roots) {
    sort_level(root.children, filter.sort);
  }

  return roots;
}

// =========================================================================
// Text rendering
// =========================================================================

/// @brief Per-kind rendered counts for the summary footer.
struct node_counts {
  std::size_t plans     = 0;
  std::size_t tasks     = 0;
  std::size_t artifacts = 0;
  std::size_t decisions = 0;
  std::size_t scenarios = 0;
  std::size_t questions = 0;

  /// @brief Count one RENDERED node. Scope roots are never counted.
  auto bump(std::string_view kind) -> void {
    if (kind == "plan") {
      ++plans;
    } else if (kind == "task") {
      ++tasks;
    } else if (kind == "artifact") {
      ++artifacts;
    } else if (kind == "decision") {
      ++decisions;
    } else if (kind == "scenario") {
      ++scenarios;
    } else if (kind == "question") {
      ++questions;
    }
  }
};

constexpr std::size_t k_title_truncate_at = 80;

/// @brief Truncate a title to 80 BYTES with a `…` suffix.
///
/// Byte-oriented, exactly as the oracle: `keep` is 77 bytes and the 3-byte
/// ellipsis follows, so a multi-byte codepoint straddling the boundary is
/// SPLIT. Reproduced rather than corrected — a codepoint-aware truncation
/// would emit different bytes than the oracle for the same title.
auto truncate_title(std::string_view title) -> std::string {
  if (title.size() <= k_title_truncate_at) {
    return std::string{title};
  }
  constexpr std::string_view k_ellipsis = "…";
  auto const                 keep       = k_title_truncate_at - k_ellipsis.size();
  return std::string{title.substr(0, keep)} + std::string{k_ellipsis};
}

/// @brief The per-line label for a non-scope node, byte-for-byte.
///
/// Note the spacing is NOT uniform across kinds: `plan` puts the status
/// before the title with two spaces, every other kind puts the title
/// first. Transcribed from the oracle's `formatNodeLabel`.
auto format_node_label(const node& n) -> std::string {
  std::string const title = truncate_title(n.title);
  if (n.kind == "plan") {
    return std::format("plan:{} [{}]  {}", n.id, n.status, title);
  }
  if (n.kind == "task") {
    return std::format("task:{}  {}  [{}, pri:{}]", n.id, title, n.status, n.priority);
  }
  if (n.kind == "artifact") {
    return std::format("artifact:{}  {}  [{}, {}]", n.id, title, n.artifact_kind, n.status);
  }
  if (n.kind == "decision") {
    return std::format("decision:{}  {}  [{}]", n.id, title, n.status);
  }
  if (n.kind == "scenario") {
    return std::format("scenario:{}  {}  [{}]", n.id, title, n.status);
  }
  if (n.kind == "question") {
    return std::format("question:{}  {}  [{}]", n.id, title, n.status);
  }
  return std::format("{}:{}  {}", n.kind, n.id, title);
}

/// @brief Append the one-line activity sub-line under an entity row.
///
/// An EMPTY `latest_action_kind` (the claim-only fallback) prints the
/// literal word `claim` so the line stays parseable.
auto append_activity_sub_line(std::string& out, const activity_summary& s, std::string_view prefix) -> void {
  std::string_view const kind = s.latest_action_kind.empty() ? std::string_view{"claim"} : s.latest_action_kind;
  out += std::format("{}activity: {} via {} @ {}  [claims:{}]\n", prefix, kind, s.latest_vendor, s.last_event_at,
                     s.active_claim_count);
}

auto append_child_list(std::string& out, std::span<const node> children, std::string_view prefix, node_counts& counts) -> void;

/// @brief Append one node and its subtree.
auto append_node(std::string& out, const node& n, std::string_view prefix, bool is_last, node_counts& counts) -> void {
  counts.bump(n.kind);

  std::string_view const connector = is_last ? "└── " : "├── ";
  out += std::format("{}{}{}\n", prefix, connector, format_node_label(n));

  std::string_view const ext          = is_last ? "    " : "│   ";
  std::string const      child_prefix = std::string{prefix} + std::string{ext};

  // The sub-line sits UNDER the entity and BEFORE its children, at the
  // child indent, so it lines up with the box-drawing column.
  if (n.activity.has_value()) {
    append_activity_sub_line(out, *n.activity, child_prefix);
  }

  append_child_list(out, n.children, child_prefix, counts);
}

/// @brief Append a child list in DIRS-FIRST order: plans, then tasks, then
/// everything else, preserving relative order within each group.
///
/// This regroups what the walk gathered; it does not re-sort within a
/// group, so the derived leaves keep their `entity_links.id` creation
/// order from `query_derived`.
auto append_child_list(std::string& out, std::span<const node> children, std::string_view prefix, node_counts& counts) -> void {
  std::vector<const node*> ordered;
  ordered.reserve(children.size());
  for (auto const& c : children) {
    if (c.kind == "plan") {
      ordered.push_back(&c);
    }
  }
  for (auto const& c : children) {
    if (c.kind == "task") {
      ordered.push_back(&c);
    }
  }
  for (auto const& c : children) {
    if (c.kind != "plan" && c.kind != "task") {
      ordered.push_back(&c);
    }
  }

  for (std::size_t i = 0; i < ordered.size(); ++i) {
    append_node(out, *ordered[i], prefix, i + 1 == ordered.size(), counts);
  }
}

/// @brief English pluralisation for the footer: `""` or `"s"`.
auto plural(std::size_t n) -> std::string_view {
  return n == 1 ? std::string_view{} : std::string_view{"s"};
}

// =========================================================================
// JSON rendering
// =========================================================================

/// @brief Append one node as a JSON object.
///
/// TWO different absence conventions live in this one shape, and both are
/// the oracle's:
///   - `scope_id` on a scope root is emitted as literal `null` when unset.
///   - `activity_summary` is OMITTED ENTIRELY when absent — no key, not a
///     `null` — so a consumer never sees an empty placeholder.
/// A scope root emits only the scope-relevant fields; an entity node emits
/// the entity fields and never the scope ones (task 2377).
auto append_node_json(std::string& out, const node& n) -> void {
  out += "{\"kind\":";
  append_json_string(out, n.kind);

  if (n.kind == "scope") {
    out += ",\"title\":";
    append_json_string(out, n.title);
    out += ",\"scope_kind\":";
    append_json_string(out, n.scope_kind);
    out += ",\"scope_id\":";
    if (n.scope_id.has_value()) {
      out += std::format("{}", *n.scope_id);
    } else {
      out += "null";
    }
    out += ",\"scope_label\":";
    append_json_string(out, n.scope_label);
  } else {
    out += std::format(",\"id\":{},\"title\":", n.id);
    append_json_string(out, n.title);
    out += ",\"slug\":";
    append_json_string(out, n.slug);
    out += ",\"status\":";
    append_json_string(out, n.status);
    out += std::format(",\"priority\":{},\"artifact_kind\":", n.priority);
    append_json_string(out, n.artifact_kind);
    out += ",\"created_at\":";
    append_json_string(out, n.created_at);
    out += ",\"updated_at\":";
    append_json_string(out, n.updated_at);
    if (n.activity.has_value()) {
      out += ",\"activity_summary\":{\"latest_action_kind\":";
      append_json_string(out, n.activity->latest_action_kind);
      out += ",\"latest_vendor\":";
      append_json_string(out, n.activity->latest_vendor);
      out += ",\"last_event_at\":";
      append_json_string(out, n.activity->last_event_at);
      out += std::format(",\"active_claim_count\":{}}}", n.activity->active_claim_count);
    }
  }

  out += ",\"children\":[";
  for (std::size_t i = 0; i < n.children.size(); ++i) {
    if (i > 0) {
      out += ',';
    }
    append_node_json(out, n.children[i]);
  }
  out += "]}";
}

} // namespace

// =========================================================================
// Public surface
// =========================================================================

auto valid_kinds() -> std::span<const std::string_view> {
  static constexpr std::string_view k_kinds[] = {"plan", "task", "question", "scenario", "decision", "artifact"};
  return k_kinds;
}

auto is_valid_kind(std::string_view kind) -> bool {
  return std::ranges::find(valid_kinds(), kind) != valid_kinds().end();
}

auto render_unknown_kind(std::string_view kind) -> std::string {
  return std::format("unknown kind '{}'", kind);
}

auto sort_key_from_text(std::string_view text) -> std::optional<sort_key> {
  if (text == "id") {
    return sort_key::id;
  }
  if (text == "updated") {
    return sort_key::updated;
  }
  if (text == "created") {
    return sort_key::created;
  }
  if (text == "unsorted") {
    return sort_key::unsorted;
  }
  return std::nullopt;
}

auto build(db::connection& conn, const tree_filter& filter) -> std::expected<std::vector<node>, tree_error> {
  // Kind validation happens BEFORE any scope work, so an unknown kind
  // refuses even when the scope would also have failed.
  for (auto const& k : filter.kinds) {
    if (!is_valid_kind(k)) {
      return std::unexpected(tree_error::unknown_kind);
    }
  }

  if (filter.all_scopes) {
    return build_all_scopes(conn, filter);
  }

  std::string_view            scope_kind = "global";
  std::optional<std::int64_t> scope_id;

  if (filter.scope.has_value()) {
    auto ref = scope_ref::resolve(conn, *filter.scope);
    if (!ref) {
      switch (ref.error()) {
      case scope_ref::error::slug_not_found:
        return std::unexpected(tree_error::slug_not_found);
      case scope_ref::error::query_failed:
        return std::unexpected(tree_error::query_failed);
      }
      return std::unexpected(tree_error::query_failed);
    }
    switch (ref->kind) {
    case scope_ref::scope_kind::global:
      scope_kind = "global";
      break;
    case scope_ref::scope_kind::association:
      scope_kind = "association";
      scope_id   = ref->id;
      break;
    case scope_ref::scope_kind::repo:
      scope_kind = "repo";
      scope_id   = ref->id;
      break;
    }
  }

  auto root = walk_one_root(conn, scope_kind, scope_id, filter);
  if (!root) {
    return std::unexpected(root.error());
  }
  std::vector<node> roots;
  roots.push_back(std::move(*root));
  // The scope root itself is the only element, so this orders its SUBTREE.
  sort_level(roots.front().children, filter.sort);
  return roots;
}

auto render_text(std::span<const node> roots) -> std::string {
  std::string out;
  node_counts counts;

  for (std::size_t i = 0; i < roots.size(); ++i) {
    if (i > 0) {
      out += '\n';
    }
    auto const& root = roots[i];
    // A scope root prints its label bare — no connector, no indent.
    out += (root.scope_label.empty() ? root.title : root.scope_label);
    out += '\n';
    append_child_list(out, root.children, "", counts);
  }

  out += '\n';
  out += std::format("{} plan{}, {} task{}, {} artifact{}, {} decision{}, {} scenario{}, {} question{}\n", counts.plans,
                     plural(counts.plans), counts.tasks, plural(counts.tasks), counts.artifacts, plural(counts.artifacts),
                     counts.decisions, plural(counts.decisions), counts.scenarios, plural(counts.scenarios), counts.questions,
                     plural(counts.questions));
  return out;
}

auto render_json(std::span<const node> roots) -> std::string {
  std::string out;
  if (roots.size() == 1) {
    append_node_json(out, roots[0]);
  } else {
    out += '[';
    for (std::size_t i = 0; i < roots.size(); ++i) {
      if (i > 0) {
        out += ',';
      }
      append_node_json(out, roots[i]);
    }
    out += ']';
  }
  out += '\n';
  return out;
}

} // namespace planar::engine::tree
