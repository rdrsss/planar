/// @file descendants.cpp
/// @brief Implementation of `planar.engine.planning.descendants`. See
/// descendants.cppm for why the walk lives in this bucket rather than in
/// `engine_extsync`, and for the two oracle-verified surprises (`plan_id` is
/// ignored; a multiply-linked task repeats).

module planar.engine.planning.descendants;

import std;
import planar.db;

namespace planar::engine::planning::descendants {

namespace {

/// @brief `coalesce(title,'')` for one plan.
/// @param conn An open, migrated database connection.
/// @param plan_id The plan.
/// @return The title, or the failure.
auto load_plan_title(db::connection& conn, std::int64_t plan_id) -> std::expected<std::string, descendants_error> {
  auto stmt = conn.prepare("select coalesce(title,'') from plans where id = ?");
  if (!stmt) {
    return std::unexpected(descendants_error::query_failed);
  }
  if (auto bound = stmt->bind_int64(1, plan_id); !bound) {
    return std::unexpected(descendants_error::query_failed);
  }
  auto stepped = stmt->step();
  if (!stepped) {
    return std::unexpected(descendants_error::query_failed);
  }
  if (*stepped == db::step_result::done) {
    return std::unexpected(descendants_error::not_found);
  }
  return stmt->column_text(0);
}

} // namespace

auto walk_tree(db::connection& conn, std::int64_t anchor_plan_id) -> std::expected<std::vector<tree_entry>, descendants_error> {
  std::vector<tree_entry> entries;

  auto anchor_title = load_plan_title(conn, anchor_plan_id);
  if (!anchor_title) {
    return std::unexpected(anchor_title.error());
  }
  entries.push_back(tree_entry{.kind = entry_kind::plan_anchor, .id = anchor_plan_id, .title = std::move(*anchor_title)});

  // Breadth-first over `parent_plan_id`: the frontier is a QUEUE and each
  // level's query is `order by id`, so the emitted order is level by level,
  // ascending within a level -- not a depth-first pre-order. On the probe
  // fixture (plan 4's parent is plan 2) the oracle emitted 2, 3, 4: both
  // children of the anchor before the grandchild. Depth-first would have
  // produced 2, 4, 3, and nothing else in the output distinguishes them.
  std::deque<std::int64_t> frontier;
  frontier.push_back(anchor_plan_id);
  while (!frontier.empty()) {
    auto const current = frontier.front();
    frontier.pop_front();

    auto stmt = conn.prepare("select id, coalesce(title,'') from plans where parent_plan_id = ? order by id");
    if (!stmt) {
      return std::unexpected(descendants_error::query_failed);
    }
    if (auto bound = stmt->bind_int64(1, current); !bound) {
      return std::unexpected(descendants_error::query_failed);
    }
    while (true) {
      auto stepped = stmt->step();
      if (!stepped) {
        return std::unexpected(descendants_error::query_failed);
      }
      if (*stepped == db::step_result::done) {
        break;
      }
      auto const id = stmt->column_int64(0);
      entries.push_back(tree_entry{.kind = entry_kind::plan_child, .id = id, .title = stmt->column_text(1)});
      frontier.push_back(id);
    }
  }

  // The plan id set is snapshotted AFTER the BFS completes and BEFORE the
  // task pass appends anything, so the tasks below are never themselves
  // walked for children. Reading the ids back out of `entries` (rather than
  // tracking them during the BFS) is the oracle's shape and is correct only
  // because of that ordering.
  std::vector<std::int64_t> plan_ids;
  plan_ids.reserve(entries.size());
  for (auto const& entry : entries) {
    plan_ids.push_back(entry.id);
  }

  // One query PER PLAN, so `distinct` de-duplicates WITHIN a plan and not
  // across them: a task linked to two plans in the tree is emitted twice.
  // Oracle-verified -- see descendants.cppm.
  for (auto const plan_id : plan_ids) {
    auto stmt = conn.prepare("select distinct t.id, coalesce(t.title,'') from tasks t "
                             "join entity_links el on el.from_kind='task' and el.from_id=t.id "
                             "where el.to_kind='plan' and el.to_id=? and el.relationship='derives-from' "
                             "order by t.id");
    if (!stmt) {
      return std::unexpected(descendants_error::query_failed);
    }
    if (auto bound = stmt->bind_int64(1, plan_id); !bound) {
      return std::unexpected(descendants_error::query_failed);
    }
    while (true) {
      auto stepped = stmt->step();
      if (!stepped) {
        return std::unexpected(descendants_error::query_failed);
      }
      if (*stepped == db::step_result::done) {
        break;
      }
      entries.push_back(tree_entry{.kind = entry_kind::task, .id = stmt->column_int64(0), .title = stmt->column_text(1)});
    }
  }

  return entries;
}

} // namespace planar::engine::planning::descendants
