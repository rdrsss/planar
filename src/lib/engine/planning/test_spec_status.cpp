/// @file test_spec_status.cpp
/// @brief Implementation of `planar.engine.planning.test_spec_status` (plan
/// 996, task 6095). See test_spec_status.cppm for scope and the
/// oracle-derived semantics.

module planar.engine.planning.test_spec_status;

import std;
import planar.json_text;
import planar.db;

namespace planar::engine::planning::test_spec_status {

// The one shared escape table, layer 1. See json_text.cppm.
using json_text::append_json_string;

namespace {

/// @brief Trim exactly the two bytes the Zig original trims (`" \t"`) — NOT
/// newlines, and no locale involvement.
auto trim_spaces_tabs(std::string_view s) -> std::string_view {
  constexpr std::string_view k_ws = " \t";
  const auto                 b    = s.find_first_not_of(k_ws);
  if (b == std::string_view::npos) {
    return {};
  }
  const auto e = s.find_last_not_of(k_ws);
  return s.substr(b, e - b + 1);
}

auto ascii_lower(char c) -> char {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

auto starts_with_ci(std::string_view s, std::string_view prefix) -> bool {
  if (s.size() < prefix.size()) {
    return false;
  }
  for (std::size_t i = 0; i < prefix.size(); ++i) {
    if (ascii_lower(s[i]) != ascii_lower(prefix[i])) {
      return false;
    }
  }
  return true;
}

/// @brief Left-align in `width`, never truncating — zig's `{s:<N}`.
auto pad_right(std::string_view s, std::size_t width) -> std::string {
  std::string out{s};
  if (out.size() < width) {
    out.append(width - out.size(), ' ');
  }
  return out;
}

/// @brief Right-align in `width`, never truncating — zig's `{s:>N}`.
auto pad_left(std::string_view s, std::size_t width) -> std::string {
  if (s.size() >= width) {
    return std::string{s};
  }
  std::string out(width - s.size(), ' ');
  out.append(s);
  return out;
}

/// @brief Render a count the way the oracle's `{d:>5}` does: an EXPLICIT sign
/// character, then the digits, right-aligned in five and overflowing rather
/// than truncating.
///
/// This is the surprising part of the text renderer and it is not a guess --
/// the captured table reads `+1`, `+0`, and `+12345`, the last of which is
/// six characters wide in a five-wide column and simply pushes the rest of
/// the row right. A plain right-aligned integer is wrong on every single row.
auto signed_count(std::int64_t value) -> std::string {
  return pad_left(std::format("{:+d}", value), 5);
}

/// @brief Truncate to 32 characters as `29 + "..."`, matching the Zig helper.
auto truncate32(std::string_view s) -> std::string {
  if (s.size() <= 32) {
    return std::string{s};
  }
  return std::string{s.substr(0, 29)} + "...";
}

struct task_buckets {
  std::int64_t happy       = 0;
  std::int64_t empty       = 0;
  std::int64_t error_count = 0;
  std::int64_t edge        = 0;
  std::int64_t other       = 0;

  auto add(bucket b) -> void {
    switch (b) {
    case bucket::happy:
      ++happy;
      break;
    case bucket::empty:
      ++empty;
      break;
    case bucket::error_case:
      ++error_count;
      break;
    case bucket::edge:
      ++edge;
      break;
    case bucket::other:
      ++other;
      break;
    }
  }
};

struct plan_row {
  std::int64_t id = 0;
  std::string  title;
};

struct scenario_row {
  std::int64_t id = 0;
  std::string  title;
};

} // namespace

auto classify_bucket(std::string_view title) -> bucket {
  const auto trimmed = trim_spaces_tabs(title);
  if (starts_with_ci(trimmed, "happy path")) {
    return bucket::happy;
  }
  // Three prefixes reach the one bucket, and the compound form is tested
  // first so `empty / null ...` cannot be short-circuited by the bare
  // `empty` arm into a different answer. (Both arms return `empty` here, so
  // the ordering is behavioural only if the arms ever diverge -- preserved
  // from the Zig original deliberately.)
  if (starts_with_ci(trimmed, "empty / null") || starts_with_ci(trimmed, "empty") || starts_with_ci(trimmed, "null")) {
    return bucket::empty;
  }
  if (starts_with_ci(trimmed, "error")) {
    return bucket::error_case;
  }
  if (starts_with_ci(trimmed, "edge")) {
    return bucket::edge;
  }
  return bucket::other;
}

auto fetch_anchor(db::connection& conn, std::string_view argument) -> std::expected<anchor, test_spec_error> {
  // Numeric branch first. A fully-numeric argument that resolves to nothing
  // does NOT fall through to the slug branch -- matching the Zig original,
  // where the id lookup returns NotFound directly.
  std::int64_t parsed    = 0;
  const auto*  begin     = argument.data();
  const auto*  end       = begin + argument.size();
  const bool   is_number = !argument.empty() && std::from_chars(begin, end, parsed).ec == std::errc{} &&
                           std::from_chars(begin, end, parsed).ptr == end;

  if (is_number) {
    auto stmt = conn.prepare("select p.id, p.slug from plans p where p.id = ? and p.parent_plan_id is null");
    if (!stmt) {
      return std::unexpected(test_spec_error::query_failed);
    }
    if (auto bound = stmt->bind_int64(1, parsed); !bound) {
      return std::unexpected(test_spec_error::query_failed);
    }
    auto stepped = stmt->step();
    if (!stepped) {
      return std::unexpected(test_spec_error::query_failed);
    }
    if (*stepped != db::step_result::row) {
      return std::unexpected(test_spec_error::not_found);
    }
    return anchor{.id = stmt->column_int64(0), .slug = stmt->column_text(1)};
  }

  auto stmt = conn.prepare("select p.id, p.slug from plans p "
                           "where p.parent_plan_id is null and p.slug = ? order by p.id limit 1");
  if (!stmt) {
    return std::unexpected(test_spec_error::query_failed);
  }
  if (auto bound = stmt->bind_text(1, argument); !bound) {
    return std::unexpected(test_spec_error::query_failed);
  }
  auto stepped = stmt->step();
  if (!stepped) {
    return std::unexpected(test_spec_error::query_failed);
  }
  if (*stepped != db::step_result::row) {
    return std::unexpected(test_spec_error::not_found);
  }
  return anchor{.id = stmt->column_int64(0), .slug = stmt->column_text(1)};
}

auto compute(db::connection& conn, std::int64_t anchor_plan_id) -> std::expected<status, test_spec_error> {
  // --- step 1: the anchor plus every plan with a derives-from edge to it ---
  std::vector<plan_row> plans;
  {
    auto stmt = conn.prepare("select p.id, p.title from plans p "
                             "where p.id = ? or p.id in ("
                             "  select from_id from entity_links "
                             "   where from_kind = 'plan' and to_kind = 'plan' and to_id = ? "
                             "     and relationship = 'derives-from') "
                             "order by p.id");
    if (!stmt) {
      return std::unexpected(test_spec_error::query_failed);
    }
    if (!stmt->bind_int64(1, anchor_plan_id).has_value() || !stmt->bind_int64(2, anchor_plan_id).has_value()) {
      return std::unexpected(test_spec_error::query_failed);
    }
    while (true) {
      auto stepped = stmt->step();
      if (!stepped) {
        return std::unexpected(test_spec_error::query_failed);
      }
      if (*stepped == db::step_result::done) {
        break;
      }
      plans.push_back(plan_row{.id = stmt->column_int64(0), .title = stmt->column_text(1)});
    }
  }

  // --- step 2: scenarios attached to the ANCHOR (not to the milestones) ---
  std::vector<scenario_row> scenarios;
  {
    auto stmt = conn.prepare("select s.id, s.title from test_scenarios s where s.id in ("
                             "  select from_id from entity_links "
                             "   where from_kind = 'test_scenario' and to_kind = 'plan' and to_id = ? "
                             "     and relationship = 'derives-from')");
    if (!stmt) {
      return std::unexpected(test_spec_error::query_failed);
    }
    if (auto bound = stmt->bind_int64(1, anchor_plan_id); !bound) {
      return std::unexpected(test_spec_error::query_failed);
    }
    while (true) {
      auto stepped = stmt->step();
      if (!stepped) {
        return std::unexpected(test_spec_error::query_failed);
      }
      if (*stepped == db::step_result::done) {
        break;
      }
      scenarios.push_back(scenario_row{.id = stmt->column_int64(0), .title = stmt->column_text(1)});
    }
  }

  // --- step 3: which tasks are covered, and by scenarios of which buckets --
  //
  // The bucket tallies are per-VERIFYING-SCENARIO. A task verified by five
  // scenarios contributes five bucket hits and exactly one to tasks_covered.
  std::unordered_map<std::int64_t, task_buckets> per_task;
  {
    auto stmt = conn.prepare("select to_id from entity_links "
                             "where from_kind = 'test_scenario' and from_id = ? "
                             "  and to_kind = 'task' and relationship = 'verifies'");
    if (!stmt) {
      return std::unexpected(test_spec_error::query_failed);
    }
    for (const auto& sc : scenarios) {
      const auto b = classify_bucket(sc.title);
      if (auto reset = stmt->reset(); !reset) {
        return std::unexpected(test_spec_error::query_failed);
      }
      if (auto bound = stmt->bind_int64(1, sc.id); !bound) {
        return std::unexpected(test_spec_error::query_failed);
      }
      while (true) {
        auto stepped = stmt->step();
        if (!stepped) {
          return std::unexpected(test_spec_error::query_failed);
        }
        if (*stepped == db::step_result::done) {
          break;
        }
        per_task[stmt->column_int64(0)].add(b);
      }
    }
  }

  // --- step 4: per-milestone task aggregates plus the roll-up -------------
  status report;
  report.summary.anchor_plan_id  = anchor_plan_id;
  report.summary.total_scenarios = static_cast<std::int64_t>(scenarios.size());

  auto stmt = conn.prepare("select id, slug from tasks where plan_id = ?");
  if (!stmt) {
    return std::unexpected(test_spec_error::query_failed);
  }
  for (const auto& pr : plans) {
    milestone_status ms{.plan_id = pr.id, .title = pr.title};
    if (auto reset = stmt->reset(); !reset) {
      return std::unexpected(test_spec_error::query_failed);
    }
    if (auto bound = stmt->bind_int64(1, pr.id); !bound) {
      return std::unexpected(test_spec_error::query_failed);
    }
    while (true) {
      auto stepped = stmt->step();
      if (!stepped) {
        return std::unexpected(test_spec_error::query_failed);
      }
      if (*stepped == db::step_result::done) {
        break;
      }
      const auto task_id = stmt->column_int64(0);
      ++ms.total_tasks;
      // A NULL slug and an EMPTY slug both fail to count -- the Zig original
      // checks `s.len > 0` inside the non-null branch.
      if (!stmt->is_null(1) && !stmt->column_text(1).empty()) {
        ++ms.tasks_with_slug;
      }
      if (const auto it = per_task.find(task_id); it != per_task.end()) {
        ++ms.tasks_covered;
        ms.happy += it->second.happy;
        ms.empty += it->second.empty;
        ms.error_count += it->second.error_count;
        ms.edge += it->second.edge;
        ms.other += it->second.other;
      }
    }
    report.summary.total_tasks += ms.total_tasks;
    report.summary.tasks_with_slug += ms.tasks_with_slug;
    report.summary.tasks_covered += ms.tasks_covered;
    report.milestones.push_back(std::move(ms));
  }

  // Stable ordering: plan id ascending, then title. The SQL already returns
  // `order by p.id`, so this is a belt-and-suspenders re-sort that also pins
  // the title tie-break for the (schema-impossible) duplicate-id case. Uses a
  // STABLE sort, matching zig's `std.sort.block`.
  std::ranges::stable_sort(report.milestones, [](const milestone_status& a, const milestone_status& b) {
    if (a.plan_id != b.plan_id) {
      return a.plan_id < b.plan_id;
    }
    return a.title < b.title;
  });

  return report;
}

auto render_json(const status& report) -> std::string {
  std::string out;
  for (const auto& m : report.milestones) {
    out.append(std::format("{{\"plan_id\":{},\"title\":", m.plan_id));
    append_json_string(out, m.title);
    out.append(std::format(",\"total_tasks\":{},\"tasks_with_slug\":{},\"tasks_covered\":{}", m.total_tasks, m.tasks_with_slug,
                           m.tasks_covered));
    // Serialized as "error", not "error_count" -- the wire name is fixed.
    out.append(std::format(",\"happy\":{},\"empty\":{},\"error\":{},\"edge\":{},\"other\":{}}}\n", m.happy, m.empty,
                           m.error_count, m.edge, m.other));
  }
  out.append(std::format("{{\"anchor_plan_id\":{},\"total_tasks\":{},\"tasks_with_slug\":{},"
                         "\"tasks_covered\":{},\"total_scenarios\":{}}}\n",
                         report.summary.anchor_plan_id, report.summary.total_tasks, report.summary.tasks_with_slug,
                         report.summary.tasks_covered, report.summary.total_scenarios));
  return out;
}

auto render_text(std::string_view anchor_slug, const status& report) -> std::string {
  std::string out;
  out.append(std::format("test-spec status for plan {} ({})\n", report.summary.anchor_plan_id, anchor_slug));
  out.append(std::format("  {} {} {} {}   {} {} {} {} {}\n", pad_right("milestone", 32), pad_left("tasks", 5),
                         pad_left("slug", 5), pad_left("cov", 5), pad_left("happy", 5), pad_left("empty", 5),
                         pad_left("error", 5), pad_left("edge", 5), pad_left("other", 5)));
  for (const auto& m : report.milestones) {
    out.append(std::format("  {} {} {} {}   {} {} {} {} {}\n", pad_right(truncate32(m.title), 32), signed_count(m.total_tasks),
                           signed_count(m.tasks_with_slug), signed_count(m.tasks_covered), signed_count(m.happy),
                           signed_count(m.empty), signed_count(m.error_count), signed_count(m.edge), signed_count(m.other)));
  }
  out.push_back('\n');
  out.append(std::format("  {} scenarios total; {} of {} slug-bearing tasks covered ({} total tasks).\n",
                         report.summary.total_scenarios, report.summary.tasks_covered, report.summary.tasks_with_slug,
                         report.summary.total_tasks));
  return out;
}

auto render_not_found(std::string_view argument) -> std::string {
  return std::format("plan '{}' not found", argument);
}

} // namespace planar::engine::planning::test_spec_status
