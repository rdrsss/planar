/// @file builder.cpp
/// @brief Implementation of `planar.engine.templates.builder` (plan 996,
/// task 6190). See builder.cppm for the anchor walk, the per-kind
/// fallbacks, and the two reproduced asymmetries.

module planar.engine.templates.builder;

import std;
import planar.db;
import planar.engine.templates.context;

namespace planar::engine::templates {

namespace {

/// @brief Load a `plans` row into a `plan_info`.
///
/// Every text column is `coalesce`d to `''` and `scope_id` to `0` IN SQL
/// rather than in C++, so a NULL column and an empty one are
/// indistinguishable here by construction — which is what the renderer
/// wants (`{{if .Plan.Slug}}` is false for both). `scope_id` is read with
/// `column_int64`, never as text-then-parse: the column is `integer` in the
/// schema and the oracle's own comment records that the text round-trip it
/// used to do could lose precision.
/// @param conn An open connection.
/// @param plan_id The plan's row id.
/// @return The plan, or the failure.
auto load_plan(db::connection& conn, std::int64_t plan_id) -> std::expected<plan_info, builder_error> {
  auto stmt = conn.prepare("select id, coalesce(slug,''), coalesce(title,''), coalesce(summary,''), "
                           "coalesce(status,''), coalesce(scope_kind,''), coalesce(scope_id,0) "
                           "from plans where id = ?");
  if (!stmt.has_value()) {
    return std::unexpected(builder_error::query_failed);
  }
  if (!stmt->bind_int64(1, plan_id).has_value()) {
    return std::unexpected(builder_error::query_failed);
  }
  auto const step = stmt->step();
  if (!step.has_value()) {
    return std::unexpected(builder_error::query_failed);
  }
  if (*step == db::step_result::done) {
    return std::unexpected(builder_error::not_found);
  }
  return plan_info{
      .id   = stmt->column_int64(0),
      .slug = stmt->column_text(1),
      // `summary`, not `body` — the column and the template-visible field
      // name genuinely disagree. See builder.cppm.
      .title      = stmt->column_text(2),
      .body       = stmt->column_text(3),
      .scope_kind = stmt->column_text(5),
      .scope_id   = stmt->column_int64(6),
      .status     = stmt->column_text(4),
  };
}

/// @brief Whether `plan_id` has no parent.
/// @param conn An open connection.
/// @param plan_id The plan's row id.
/// @return Whether it is a root plan, or the failure.
auto is_top_level_plan(db::connection& conn, std::int64_t plan_id) -> std::expected<bool, builder_error> {
  auto stmt = conn.prepare("select coalesce(parent_plan_id, 0) from plans where id = ?");
  if (!stmt.has_value() || !stmt->bind_int64(1, plan_id).has_value()) {
    return std::unexpected(builder_error::query_failed);
  }
  auto const step = stmt->step();
  if (!step.has_value()) {
    return std::unexpected(builder_error::query_failed);
  }
  if (*step == db::step_result::done) {
    return std::unexpected(builder_error::not_found);
  }
  return stmt->column_int64(0) == 0;
}

/// @brief Walk from an arbitrary entity to the anchor plan of its tree.
///
/// See builder.cppm for the two stages and the per-kind fallback.
/// @param conn An open connection.
/// @param from_kind The entity kind as spelled in `entity_links.from_kind`.
/// @param from_id The entity's row id.
/// @return The anchor plan, or the failure.
auto find_anchor_plan(db::connection& conn, std::string_view from_kind, std::int64_t from_id)
    -> std::expected<plan_info, builder_error> {
  std::int64_t first_plan_id = 0;
  {
    auto stmt = conn.prepare("select to_id from entity_links "
                             "where from_kind = ? and from_id = ? and to_kind = 'plan' "
                             "and relationship = 'derives-from' order by id limit 1");
    if (!stmt.has_value() || !stmt->bind_text(1, from_kind).has_value() || !stmt->bind_int64(2, from_id).has_value()) {
      return std::unexpected(builder_error::query_failed);
    }
    auto const step = stmt->step();
    if (!step.has_value()) {
      return std::unexpected(builder_error::query_failed);
    }
    if (*step == db::step_result::row) {
      first_plan_id = stmt->column_int64(0);
    } else if (from_kind == "plan") {
      // A plan needs no edge — it climbs its own parent chain.
      first_plan_id = from_id;
    } else if (from_kind == "task") {
      // Tasks hang off `tasks.plan_id`, a FOREIGN KEY rather than an
      // `entity_links` edge, so the absent edge is normal rather than an
      // error. A task with a NULL `plan_id` genuinely has no anchor.
      auto task_stmt = conn.prepare("select coalesce(plan_id, 0) from tasks where id = ?");
      if (!task_stmt.has_value() || !task_stmt->bind_int64(1, from_id).has_value()) {
        return std::unexpected(builder_error::query_failed);
      }
      auto const task_step = task_stmt->step();
      if (!task_step.has_value()) {
        return std::unexpected(builder_error::query_failed);
      }
      if (*task_step == db::step_result::done) {
        return std::unexpected(builder_error::anchor_plan_not_found);
      }
      auto const pid = task_stmt->column_int64(0);
      if (pid == 0) {
        return std::unexpected(builder_error::anchor_plan_not_found);
      }
      first_plan_id = pid;
    } else {
      // test_scenario and anything else: no fallback exists.
      return std::unexpected(builder_error::anchor_plan_not_found);
    }
  }

  // Climb `parent_plan_id` to the root.
  std::int64_t current = first_plan_id;
  while (true) {
    auto stmt = conn.prepare("select coalesce(parent_plan_id, 0) from plans where id = ?");
    if (!stmt.has_value() || !stmt->bind_int64(1, current).has_value()) {
      return std::unexpected(builder_error::query_failed);
    }
    auto const step = stmt->step();
    if (!step.has_value()) {
      return std::unexpected(builder_error::query_failed);
    }
    if (*step == db::step_result::done) {
      // A dangling parent pointer. The oracle reports it as "no anchor"
      // rather than as a missing row, and so does this.
      return std::unexpected(builder_error::anchor_plan_not_found);
    }
    auto const parent = stmt->column_int64(0);
    if (parent == 0) {
      break;
    }
    current = parent;
  }
  return load_plan(conn, current);
}

/// @brief Load an association's identity by row id.
///
/// A zero id, or an id matching no row, yields an EMPTY `assoc_info`
/// rather than an error — a plan in `global` scope has no association and
/// that is not a failure. Both spellings render `{{.Assoc.Slug}}` as the
/// empty string.
/// @param conn An open connection.
/// @param assoc_id The association's row id, or 0.
/// @return The association identity, or the failure.
auto load_assoc(db::connection& conn, std::int64_t assoc_id) -> std::expected<assoc_info, builder_error> {
  if (assoc_id == 0) {
    return assoc_info{};
  }
  auto stmt = conn.prepare("select coalesce(slug,''), coalesce(name,'') from associations where id = ?");
  if (!stmt.has_value() || !stmt->bind_int64(1, assoc_id).has_value()) {
    return std::unexpected(builder_error::query_failed);
  }
  auto const step = stmt->step();
  if (!step.has_value()) {
    return std::unexpected(builder_error::query_failed);
  }
  if (*step == db::step_result::done) {
    return assoc_info{};
  }
  return assoc_info{.slug = stmt->column_text(0), .name = stmt->column_text(1)};
}

/// @brief Load the repo slugs an entity touches, in edge-insertion order.
///
/// Ordered by `entity_links.id`, NOT by slug: the order repos were
/// attached is what `{{range .Touches}}` emits, and sorting here would
/// silently reorder a rendered issue body.
/// @param conn An open connection.
/// @param kind The entity kind.
/// @param id The entity's row id.
/// @return The slugs, or the failure.
auto load_touches(db::connection& conn, std::string_view kind, std::int64_t id)
    -> std::expected<std::vector<std::string>, builder_error> {
  auto stmt = conn.prepare("select coalesce(p.slug,'') from entity_links el "
                           "join projects p on p.id = el.to_id "
                           "where el.from_kind = ? and el.from_id = ? and el.to_kind = 'repo' "
                           "and el.relationship = 'touches' order by el.id");
  if (!stmt.has_value() || !stmt->bind_text(1, kind).has_value() || !stmt->bind_int64(2, id).has_value()) {
    return std::unexpected(builder_error::query_failed);
  }
  std::vector<std::string> slugs;
  while (true) {
    auto const step = stmt->step();
    if (!step.has_value()) {
      return std::unexpected(builder_error::query_failed);
    }
    if (*step == db::step_result::done) {
      return slugs;
    }
    slugs.push_back(stmt->column_text(0));
  }
}

/// @brief Load an entity's mirror external key.
///
/// Only `link_role = 'mirror'` counts, and only the lowest-id one. An
/// entity with no mirror renders `{{.ExternalKey}}` as empty, which is what
/// makes `{{if .ExternalKey}}` the "has this been propagated yet?" test the
/// shipped templates use it as.
/// @param conn An open connection.
/// @param kind The entity kind.
/// @param id The entity's row id.
/// @return The key, empty when absent, or the failure.
auto load_external_key(db::connection& conn, std::string_view kind, std::int64_t id)
    -> std::expected<std::string, builder_error> {
  auto stmt = conn.prepare("select coalesce(external_id, '') from external_links "
                           "where entity_kind = ? and entity_id = ? and link_role = 'mirror' order by id limit 1");
  if (!stmt.has_value() || !stmt->bind_text(1, kind).has_value() || !stmt->bind_int64(2, id).has_value()) {
    return std::unexpected(builder_error::query_failed);
  }
  auto const step = stmt->step();
  if (!step.has_value()) {
    return std::unexpected(builder_error::query_failed);
  }
  if (*step == db::step_result::done) {
    return std::string{};
  }
  return stmt->column_text(0);
}

} // namespace

auto build_task_context(db::connection& conn, std::int64_t task_id) -> std::expected<render_context, builder_error> {
  task_info task;
  {
    auto stmt = conn.prepare("select id, coalesce(title,''), coalesce(body,''), coalesce(status,''), "
                             "coalesce(priority,0), coalesce(scope_kind,''), coalesce(scope_id,0) "
                             "from tasks where id = ?");
    if (!stmt.has_value() || !stmt->bind_int64(1, task_id).has_value()) {
      return std::unexpected(builder_error::query_failed);
    }
    auto const step = stmt->step();
    if (!step.has_value()) {
      return std::unexpected(builder_error::query_failed);
    }
    if (*step == db::step_result::done) {
      return std::unexpected(builder_error::not_found);
    }
    task = task_info{
        .id         = stmt->column_int64(0),
        .title      = stmt->column_text(1),
        .body       = stmt->column_text(2),
        .status     = stmt->column_text(3),
        .priority   = stmt->column_int64(4),
        .scope_kind = stmt->column_text(5),
        .scope_id   = stmt->column_int64(6),
    };
  }

  auto anchor = find_anchor_plan(conn, "task", task_id);
  if (!anchor.has_value()) {
    return std::unexpected(anchor.error());
  }
  auto assoc = load_assoc(conn, anchor->scope_id);
  if (!assoc.has_value()) {
    return std::unexpected(assoc.error());
  }
  auto touches = load_touches(conn, "task", task_id);
  if (!touches.has_value()) {
    return std::unexpected(touches.error());
  }
  auto key = load_external_key(conn, "task", task_id);
  if (!key.has_value()) {
    return std::unexpected(key.error());
  }

  // `plan` and `feature` are the SAME row for a task. See builder.cppm.
  return render_context{
      .feature      = *anchor,
      .plan         = *anchor,
      .task         = std::move(task),
      .scenario     = {},
      .touches      = std::move(*touches),
      .assoc        = std::move(*assoc),
      .external_key = std::move(*key),
      .children     = {},
  };
}

auto build_plan_context(db::connection& conn, std::int64_t plan_id) -> std::expected<render_context, builder_error> {
  auto plan = load_plan(conn, plan_id);
  if (!plan.has_value()) {
    return std::unexpected(plan.error());
  }

  plan_info  anchor = *plan;
  auto const top    = is_top_level_plan(conn, plan_id);
  if (!top.has_value()) {
    return std::unexpected(top.error());
  }
  if (!*top) {
    auto found = find_anchor_plan(conn, "plan", plan_id);
    if (!found.has_value()) {
      return std::unexpected(found.error());
    }
    anchor = std::move(*found);
  }

  auto assoc = load_assoc(conn, anchor.scope_id);
  if (!assoc.has_value()) {
    return std::unexpected(assoc.error());
  }
  auto touches = load_touches(conn, "plan", plan_id);
  if (!touches.has_value()) {
    return std::unexpected(touches.error());
  }
  auto key = load_external_key(conn, "plan", plan_id);
  if (!key.has_value()) {
    return std::unexpected(key.error());
  }

  return render_context{
      .feature      = std::move(anchor),
      .plan         = std::move(*plan),
      .task         = {},
      .scenario     = {},
      .touches      = std::move(*touches),
      .assoc        = std::move(*assoc),
      .external_key = std::move(*key),
      .children     = {},
  };
}

auto build_scenario_context(db::connection& conn, std::int64_t scenario_id) -> std::expected<render_context, builder_error> {
  scenario_info scenario;
  {
    auto stmt = conn.prepare("select id, coalesce(title,''), coalesce(body,'') from test_scenarios where id = ?");
    if (!stmt.has_value() || !stmt->bind_int64(1, scenario_id).has_value()) {
      return std::unexpected(builder_error::query_failed);
    }
    auto const step = stmt->step();
    if (!step.has_value()) {
      return std::unexpected(builder_error::query_failed);
    }
    if (*step == db::step_result::done) {
      return std::unexpected(builder_error::not_found);
    }
    scenario = scenario_info{
        .id    = stmt->column_int64(0),
        .title = stmt->column_text(1),
        .body  = stmt->column_text(2),
    };
  }

  // NOTE the entity kind spelling: `test_scenario`, matching
  // `entity_links.from_kind` and `external_links.entity_kind`, even though
  // the CLI accepts the shorter `scenario:` alias for `--entity`.
  auto anchor = find_anchor_plan(conn, "test_scenario", scenario_id);
  if (!anchor.has_value()) {
    return std::unexpected(anchor.error());
  }
  auto assoc = load_assoc(conn, anchor->scope_id);
  if (!assoc.has_value()) {
    return std::unexpected(assoc.error());
  }
  auto key = load_external_key(conn, "test_scenario", scenario_id);
  if (!key.has_value()) {
    return std::unexpected(key.error());
  }

  // `touches` deliberately left empty — the oracle's scenario builder never
  // queries it. See builder.cppm.
  return render_context{
      .feature      = *anchor,
      .plan         = *anchor,
      .task         = {},
      .scenario     = std::move(scenario),
      .touches      = {},
      .assoc        = std::move(*assoc),
      .external_key = std::move(*key),
      .children     = {},
  };
}

} // namespace planar::engine::templates
