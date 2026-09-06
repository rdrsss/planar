/// @file load.cpp
/// @brief Implementation of `planar.engine.grouping.load` (plan 996, task
/// 6095). See load.cppm for scope, the edge-orientation contract, and the cut
/// list.

module planar.engine.grouping.load;

import std;
import planar.json_text;
import planar.db;
import planar.engine.grouping.greedy;
import planar.engine.grouping.mtkahypar;

namespace planar::engine::grouping::load {

// The one shared escape table, layer 1. See json_text.cppm.
using json_text::append_json_string;

namespace {

auto bool_text(bool value) -> std::string_view {
  return value ? std::string_view{"true"} : std::string_view{"false"};
}

auto plan_exists(db::connection& conn, std::int64_t plan_id) -> std::expected<bool, grouping_error> {
  auto stmt = conn.prepare("select count(*) from plans where id = ?");
  if (!stmt) {
    return std::unexpected(grouping_error::query_failed);
  }
  if (auto bound = stmt->bind_int64(1, plan_id); !bound) {
    return std::unexpected(grouping_error::query_failed);
  }
  auto stepped = stmt->step();
  if (!stepped) {
    return std::unexpected(grouping_error::query_failed);
  }
  if (*stepped != db::step_result::row) {
    return false;
  }
  return stmt->column_int64(0) != 0;
}

} // namespace

auto solver_from_text(std::string_view text) -> std::optional<solver> {
  if (text == "greedy") {
    return solver::greedy;
  }
  if (text == "mtkahypar") {
    return solver::mtkahypar;
  }
  return std::nullopt;
}

auto solver_to_text(solver s) -> std::string_view {
  return s == solver::greedy ? std::string_view{"greedy"} : std::string_view{"mtkahypar"};
}

auto load_open_task_ids(db::connection& conn, std::int64_t plan_id) -> std::expected<std::vector<std::int64_t>, grouping_error> {
  // `status = 'todo'` only -- NOT "not done". A `doing` or `blocked` task is
  // excluded exactly like a `done` one, matching recommend-strategy's
  // candidate set so the two arms agree.
  auto stmt = conn.prepare("select id from tasks where plan_id = ? and status = 'todo' order by priority, id");
  if (!stmt) {
    return std::unexpected(grouping_error::query_failed);
  }
  if (auto bound = stmt->bind_int64(1, plan_id); !bound) {
    return std::unexpected(grouping_error::query_failed);
  }

  std::vector<std::int64_t> out;
  while (true) {
    auto stepped = stmt->step();
    if (!stepped) {
      return std::unexpected(grouping_error::query_failed);
    }
    if (*stepped == db::step_result::done) {
      break;
    }
    out.push_back(stmt->column_int64(0));
  }
  return out;
}

auto load_units(db::connection& conn, std::int64_t task_id) -> std::expected<std::vector<greedy::unit>, grouping_error> {
  auto stmt = conn.prepare("select symbol, role, token_weight from closures "
                           "where task_id = ? and role in ('modify', 'reference') order by symbol");
  if (!stmt) {
    return std::unexpected(grouping_error::query_failed);
  }
  if (auto bound = stmt->bind_int64(1, task_id); !bound) {
    return std::unexpected(grouping_error::query_failed);
  }

  std::vector<greedy::unit>                    out;
  std::unordered_map<std::string, std::size_t> seen; // symbol -> index into out
  while (true) {
    auto stepped = stmt->step();
    if (!stepped) {
      return std::unexpected(grouping_error::query_failed);
    }
    if (*stepped == db::step_result::done) {
      break;
    }
    auto       symbol   = stmt->column_text(0);
    const auto role_txt = stmt->column_text(1);
    // A negative token_weight is clamped to zero rather than wrapping -- the
    // column is a plain integer with no CHECK, and the weight type is
    // unsigned.
    const auto raw_weight = stmt->column_int64(2);
    const auto weight     = static_cast<std::uint32_t>(std::max<std::int64_t>(0, raw_weight));
    // Anything that is not literally "modify" reads as `reference`; the SQL
    // already restricted the set to the two effective roles.
    const auto r = (role_txt == "modify") ? greedy::role::modify : greedy::role::reference;

    if (const auto it = seen.find(symbol); it != seen.end()) {
      // Fold: modify dominates, and the larger weight wins defensively.
      //
      // The modify-dominance arm is DEFENSIVE, and a break-probe found no
      // mutant that reaches it: `explain query plan` shows the IN-list
      // driving two seeks through `ix_closures_task_role` in sorted order,
      // so for a given task the 'modify' rows arrive before the 'reference'
      // ones and the unit is always CREATED as modify.
      //
      // KEEP IT -- and note the reason is stronger than "unreachable
      // defensive code we tolerate" (task 6104). This arm is not unreachable
      // BY CONSTRUCTION; it is unreachable under a plan nothing pins. The
      // query above orders by SYMBOL, not by role, so the relative order of
      // the two rows sharing a symbol is not specified by the SQL at all --
      // it falls out of the index seeks and of SQLite's sorter, which is not
      // documented as stable. A SQLite bump or a change in the statistics
      // the planner consults can reorder them with no edit to this file.
      //
      // Deleting the arm would therefore convert an unspecified ordering
      // into a correctness dependency, and the failure is SILENT: a
      // dual-role symbol would settle as `reference`, the pair would read
      // as read-write instead of write-write, and a merge that must be
      // blocked would go through. See load.t.cpp's "write-write non-merge"
      // test for the observable consequence this protects.
      if (r == greedy::role::modify) {
        out[it->second].role_ = greedy::role::modify;
      }
      if (weight > out[it->second].weight) {
        out[it->second].weight = weight;
      }
    } else {
      seen.emplace(symbol, out.size());
      out.push_back(greedy::unit{.qualified = std::move(symbol), .role_ = r, .weight = weight});
    }
  }
  return out;
}

auto load_deps(db::connection& conn, std::span<const std::int64_t> open_ids)
    -> std::expected<std::vector<greedy::dep>, grouping_error> {
  const std::unordered_set<std::int64_t> open(open_ids.begin(), open_ids.end());

  auto stmt = conn.prepare("select from_id, to_id from entity_links "
                           "where from_kind = 'task' and to_kind = 'task' and relationship = 'depends-on'");
  if (!stmt) {
    return std::unexpected(grouping_error::query_failed);
  }

  std::vector<greedy::dep> out;
  while (true) {
    auto stepped = stmt->step();
    if (!stepped) {
      return std::unexpected(grouping_error::query_failed);
    }
    if (*stepped == db::step_result::done) {
      break;
    }
    const auto from_id = stmt->column_int64(0); // the BLOCKED task
    const auto to_id   = stmt->column_int64(1); // the BLOCKER task
    if (!open.contains(from_id) || !open.contains(to_id)) {
      continue;
    }
    if (from_id == to_id) {
      continue;
    }
    // The orientation flip. from_id -> blocked, to_id -> blocker.
    out.push_back(greedy::dep{.blocked = from_id, .blocker = to_id});
  }
  return out;
}

namespace {

/// @brief Shared loader: the plan-existence check, open tasks, their
/// effective closures, and the dependency DAG. Both `recommend` and
/// `recommend_with` need identical inputs so the two solver arms are scored
/// on the exact same data.
struct loaded_inputs {
  std::vector<greedy::task> tasks;
  std::vector<greedy::dep>  deps;
};

auto load_inputs(db::connection& conn, std::int64_t plan_id) -> std::expected<loaded_inputs, grouping_error> {
  auto exists = plan_exists(conn, plan_id);
  if (!exists) {
    return std::unexpected(exists.error());
  }
  if (!*exists) {
    return std::unexpected(grouping_error::not_found);
  }

  auto task_ids = load_open_task_ids(conn, plan_id);
  if (!task_ids) {
    return std::unexpected(task_ids.error());
  }

  std::vector<greedy::task> tasks;
  tasks.reserve(task_ids->size());
  for (const auto id : *task_ids) {
    auto units = load_units(conn, id);
    if (!units) {
      return std::unexpected(units.error());
    }
    tasks.push_back(greedy::task{.id = id, .units = std::move(*units)});
  }

  auto deps = load_deps(conn, *task_ids);
  if (!deps) {
    return std::unexpected(deps.error());
  }

  return loaded_inputs{.tasks = std::move(tasks), .deps = std::move(*deps)};
}

} // namespace

auto recommend(db::connection& conn, std::int64_t plan_id, std::uint32_t budget)
    -> std::expected<recommendation, grouping_error> {
  auto loaded = load_inputs(conn, plan_id);
  if (!loaded) {
    return std::unexpected(loaded.error());
  }

  return recommendation{
      .plan_id           = plan_id,
      .budget            = budget,
      .open_tasks        = loaded->tasks.size(),
      .grouping_         = greedy::group(loaded->tasks, loaded->deps, budget),
      .solver_           = solver::greedy,
      .optimal_available = false,
      .selected_greedy   = false,
  };
}

auto recommend_with(db::connection& conn, std::int64_t plan_id, std::uint32_t budget, solver requested)
    -> std::expected<recommendation, grouping_error> {
  if (requested == solver::greedy) {
    return recommend(conn, plan_id, budget);
  }

  auto loaded = load_inputs(conn, plan_id);
  if (!loaded) {
    return std::unexpected(loaded.error());
  }
  const auto open_tasks = loaded->tasks.size();

  if (mtkahypar::available()) {
    if (auto solver_grouping = mtkahypar::try_partition(loaded->tasks, loaded->deps, budget); solver_grouping.has_value()) {
      // Both arms run on IDENTICAL inputs, scored with the IDENTICAL cost
      // function (greedy::grouping::total_cost()) -- the task-4247 contract:
      // the shipped result's cost is never higher than greedy's own.
      auto greedy_grouping = greedy::group(loaded->tasks, loaded->deps, budget);
      if (greedy_grouping.total_cost() < solver_grouping->total_cost()) {
        // Greedy strictly better -> ship greedy, but report the optimal arm
        // as having run (it did) and record that its result was discarded.
        return recommendation{
            .plan_id           = plan_id,
            .budget            = budget,
            .open_tasks        = open_tasks,
            .grouping_         = std::move(greedy_grouping),
            .solver_           = solver::mtkahypar,
            .optimal_available = true,
            .selected_greedy   = true,
        };
      }
      // mtkahypar tied or beat greedy -> ship the solver result.
      return recommendation{
          .plan_id           = plan_id,
          .budget            = budget,
          .open_tasks        = open_tasks,
          .grouping_         = std::move(*solver_grouping),
          .solver_           = solver::mtkahypar,
          .optimal_available = true,
          .selected_greedy   = false,
      };
    }
    // Invocation failed mid-run -> degrade to greedy, same as unavailable.
  }

  return recommendation{
      .plan_id           = plan_id,
      .budget            = budget,
      .open_tasks        = open_tasks,
      .grouping_         = greedy::group(loaded->tasks, loaded->deps, budget),
      .solver_           = solver::greedy,
      .optimal_available = false,
      .selected_greedy   = false,
  };
}

auto render_json(const recommendation& rec) -> std::string {
  std::string out = std::format("{{\"plan_id\":{},\"budget\":{},\"open_tasks\":{},\"solver\":\"{}\","
                                "\"optimal_available\":{},\"selected_greedy\":{},\"slices\":[",
                                rec.plan_id, rec.budget, rec.open_tasks, solver_to_text(rec.solver_),
                                bool_text(rec.optimal_available), bool_text(rec.selected_greedy));
  for (std::size_t i = 0; i < rec.grouping_.slices.size(); ++i) {
    if (i != 0) {
      out.push_back(',');
    }
    const auto& s = rec.grouping_.slices[i];
    out.append("{\"task_ids\":[");
    for (std::size_t j = 0; j < s.task_ids.size(); ++j) {
      if (j != 0) {
        out.push_back(',');
      }
      out.append(std::format("{}", s.task_ids[j]));
    }
    out.append("],\"union_symbols\":[");
    for (std::size_t j = 0; j < s.union_symbols.size(); ++j) {
      if (j != 0) {
        out.push_back(',');
      }
      append_json_string(out, s.union_symbols[j]);
    }
    out.append(std::format("],\"cost\":{}}}", s.cost));
  }
  out.append(std::format("],\"summary\":{{\"slices\":{},\"total_cost\":{}}}}}\n", rec.grouping_.slices.size(),
                         rec.grouping_.total_cost()));
  return out;
}

auto render_text(const recommendation& rec) -> std::string {
  // TWO spaces between every key:value pair on the header line.
  std::string out =
      std::format("plan:{}  budget:{}  open:{}  solver:{}  optimal_available:{}  "
                  "selected_greedy:{}  slices:{}  total_cost:{}\n",
                  rec.plan_id, rec.budget, rec.open_tasks, solver_to_text(rec.solver_), bool_text(rec.optimal_available),
                  bool_text(rec.selected_greedy), rec.grouping_.slices.size(), rec.grouping_.total_cost());
  if (rec.grouping_.slices.empty()) {
    out.append("  (no open tasks to group)\n");
    return out;
  }
  for (std::size_t i = 0; i < rec.grouping_.slices.size(); ++i) {
    const auto& s = rec.grouping_.slices[i];
    // Slice numbering is 1-based and is a display ordinal, not an id.
    out.append(std::format("slice {}  cost:{}  tasks:[", i + 1, s.cost));
    for (std::size_t j = 0; j < s.task_ids.size(); ++j) {
      if (j != 0) {
        // Comma-SPACE here, unlike the JSON form's bare comma.
        out.append(", ");
      }
      out.append(std::format("{}", s.task_ids[j]));
    }
    out.append("]\n");
    for (const auto& sym : s.union_symbols) {
      out.append(std::format("    - {}\n", sym));
    }
  }
  return out;
}

auto render_plan_not_found(std::int64_t plan_id) -> std::string {
  return std::format("plan {} not found", plan_id);
}

auto render_invalid_plan_id(std::string_view argument) -> std::string {
  return std::format("plan id must be an integer, got '{}'", argument);
}

auto render_invalid_budget(std::string_view argument) -> std::string {
  return std::format("--budget must be a non-negative integer, got '{}'", argument);
}

auto render_invalid_solver(std::string_view argument) -> std::string {
  return std::format("--solver must be 'greedy' or 'mtkahypar', got '{}'", argument);
}

} // namespace planar::engine::grouping::load
