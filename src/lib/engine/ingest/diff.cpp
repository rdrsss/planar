/// @file diff.cpp
/// @brief Implementation of `planar.engine.ingest.diff` (see diff.cppm).
///
/// Behavior-preserving port (D2) of `zig/src/engine/ingestor/diff.zig`. Every
/// SQL statement below is carried over verbatim so the set of rows considered
/// "existing" is identical between the two implementations — a widened or
/// narrowed WHERE clause here would silently change which entities are
/// proposed for removal.

module planar.engine.ingest.diff;

import std;
import planar.db;
import planar.engine.ingest.parse;

namespace planar::engine::ingest::diff {
namespace {

/// @brief One stored child plan considered for reconciliation.
struct db_child_plan {
  std::int64_t id_ = 0;
  std::string  title_;
};

/// @brief One stored task considered for reconciliation.
struct db_task {
  std::int64_t id_ = 0;
  std::string  title_;
  std::string  body_;
  std::string  slug_;
  std::string  next_action_; ///< Needed to tell a generated next action from a refined one.
};

/// @brief One stored decision considered for reconciliation.
struct db_decision {
  std::int64_t id_ = 0;
  std::string  title_;
  std::string  body_;
};

/// @brief One stored question considered for reconciliation.
struct db_question {
  std::int64_t id_ = 0;
  std::string  title_;
  std::string  status_;
};

/// @brief One stored test scenario considered for reconciliation.
struct db_scenario {
  std::int64_t id_ = 0;
  std::string  title_;
  std::string  body_;
};

/// @brief The anchor plan's identity fields.
struct anchor_row {
  std::string slug_;
  std::string assoc_slug_;
  std::string status_;
};

/// @brief Case-insensitive, trim-aware title comparison.
///
/// This is THE reconciliation key for every entity kind, so a change here
/// silently re-partitions a whole spec into adds and removals.
constexpr std::string_view title_whitespace = " \t\n\r";

[[nodiscard]] auto trim_title(std::string_view s) -> std::string_view {
  const auto first = s.find_first_not_of(title_whitespace);
  if (first == std::string_view::npos) {
    return {};
  }
  return s.substr(first, s.find_last_not_of(title_whitespace) - first + 1);
}

[[nodiscard]] auto titles_equal_ignore_case(std::string_view a, std::string_view b) -> bool {
  const auto ta = trim_title(a);
  const auto tb = trim_title(b);
  if (ta.size() != tb.size()) {
    return false;
  }
  return std::ranges::equal(ta, tb, [](char ca, char cb) {
    const auto lower = [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c; };
    return lower(ca) == lower(cb);
  });
}

/// @brief Trims inline whitespace from both ends.
[[nodiscard]] auto trim_inline(std::string_view s) -> std::string_view {
  constexpr std::string_view ws    = " \t";
  const auto                 first = s.find_first_not_of(ws);
  if (first == std::string_view::npos) {
    return {};
  }
  return s.substr(first, s.find_last_not_of(ws) - first + 1);
}

/// @brief Looks up an entry in `items` whose title matches `title`.
template <typename T> [[nodiscard]] auto by_title(std::span<const T> items, std::string_view title) -> const T* {
  for (const auto& item : items) {
    if (titles_equal_ignore_case(item.title_, title)) {
      return &item;
    }
  }
  return nullptr;
}

/// @brief Loads the anchor plan's slug, association slug and status.
///
/// The `parent_plan_id is null` clause is load-bearing: ingest anchors are
/// root plans, and pointing it at a child plan must be a clean `not_found`
/// rather than a diff computed against the wrong subtree.
[[nodiscard]] auto load_anchor_plan(db::connection& conn, std::int64_t id) -> std::expected<anchor_row, diff_error> {
  auto stmt = conn.prepare(R"(select p.slug, coalesce(a.slug, ''), p.status
from plans p
left join associations a on (p.scope_kind = 'association' and a.id = p.scope_id)
where p.id = ? and p.parent_plan_id is null)");
  if (!stmt.has_value()) {
    return std::unexpected(diff_error::query_failed);
  }
  if (!stmt->bind_int64(1, id).has_value()) {
    return std::unexpected(diff_error::query_failed);
  }
  const auto stepped = stmt->step();
  if (!stepped.has_value()) {
    return std::unexpected(diff_error::query_failed);
  }
  if (*stepped == db::step_result::done) {
    return std::unexpected(diff_error::not_found);
  }
  return anchor_row{.slug_ = stmt->column_text(0), .assoc_slug_ = stmt->column_text(1), .status_ = stmt->column_text(2)};
}

/// @brief Loads the anchor's non-abandoned child plans, ordered by id.
[[nodiscard]] auto load_child_plans(db::connection& conn, std::int64_t parent_id)
    -> std::expected<std::vector<db_child_plan>, diff_error> {
  auto stmt = conn.prepare(R"(select id, title from plans
where parent_plan_id = ?
  and status != 'abandoned'
order by id)");
  if (!stmt.has_value() || !stmt->bind_int64(1, parent_id).has_value()) {
    return std::unexpected(diff_error::query_failed);
  }
  std::vector<db_child_plan> out;
  while (true) {
    const auto stepped = stmt->step();
    if (!stepped.has_value()) {
      return std::unexpected(diff_error::query_failed);
    }
    if (*stepped == db::step_result::done) {
      break;
    }
    out.push_back({.id_ = stmt->column_int64(0), .title_ = stmt->column_text(1)});
  }
  return out;
}

/// @brief Loads the non-cancelled tasks linked to `plan_id` by `derives-from`.
[[nodiscard]] auto load_tasks_for_plan(db::connection& conn, std::int64_t plan_id)
    -> std::expected<std::vector<db_task>, diff_error> {
  auto stmt = conn.prepare(R"(select t.id, t.title, coalesce(t.body, ''), coalesce(t.slug, ''),
       coalesce(t.next_action, '')
from tasks t
join entity_links el on (el.from_kind = 'task' and el.from_id = t.id
                         and el.to_kind = 'plan' and el.to_id = ?
                         and el.relationship = 'derives-from')
where t.status != 'cancelled'
order by t.id)");
  if (!stmt.has_value() || !stmt->bind_int64(1, plan_id).has_value()) {
    return std::unexpected(diff_error::query_failed);
  }
  std::vector<db_task> out;
  while (true) {
    const auto stepped = stmt->step();
    if (!stepped.has_value()) {
      return std::unexpected(diff_error::query_failed);
    }
    if (*stepped == db::step_result::done) {
      break;
    }
    out.push_back({.id_          = stmt->column_int64(0),
                   .title_       = stmt->column_text(1),
                   .body_        = stmt->column_text(2),
                   .slug_        = stmt->column_text(3),
                   .next_action_ = stmt->column_text(4)});
  }
  return out;
}

/// @brief Loads the live decisions linked to the anchor by `derives-from`.
[[nodiscard]] auto load_decisions_for_anchor(db::connection& conn, std::int64_t anchor_id)
    -> std::expected<std::vector<db_decision>, diff_error> {
  auto stmt = conn.prepare(R"(select d.id, d.title, coalesce(d.body, '')
from decisions d
join entity_links el on (el.from_kind = 'decision' and el.from_id = d.id
                         and el.to_kind = 'plan' and el.to_id = ?
                         and el.relationship = 'derives-from')
where d.status not in ('superseded', 'withdrawn')
order by d.id)");
  if (!stmt.has_value() || !stmt->bind_int64(1, anchor_id).has_value()) {
    return std::unexpected(diff_error::query_failed);
  }
  std::vector<db_decision> out;
  while (true) {
    const auto stepped = stmt->step();
    if (!stepped.has_value()) {
      return std::unexpected(diff_error::query_failed);
    }
    if (*stepped == db::step_result::done) {
      break;
    }
    out.push_back({.id_ = stmt->column_int64(0), .title_ = stmt->column_text(1), .body_ = stmt->column_text(2)});
  }
  return out;
}

/// @brief Loads the questions in the anchor plan's own scope.
///
/// Questions are matched by scope rather than by a link edge, so the anchor's
/// `scope_kind`/`scope_id` is read first; a NULL `scope_id` (global scope)
/// needs a different WHERE clause than a bound one.
[[nodiscard]] auto load_questions_for_anchor(db::connection& conn, std::int64_t anchor_id)
    -> std::expected<std::vector<db_question>, diff_error> {
  auto scope_stmt = conn.prepare("select scope_kind, scope_id from plans where id = ?");
  if (!scope_stmt.has_value() || !scope_stmt->bind_int64(1, anchor_id).has_value()) {
    return std::unexpected(diff_error::query_failed);
  }
  const auto scope_stepped = scope_stmt->step();
  if (!scope_stepped.has_value()) {
    return std::unexpected(diff_error::query_failed);
  }
  if (*scope_stepped == db::step_result::done) {
    return std::unexpected(diff_error::not_found);
  }
  const auto                  scope_kind = scope_stmt->column_text(0);
  std::optional<std::int64_t> scope_id;
  if (!scope_stmt->is_null(1)) {
    scope_id = scope_stmt->column_int64(1);
  }

  auto stmt = scope_id.has_value() ? conn.prepare(R"(select id, title, status from questions
where scope_kind = ? and scope_id = ?
order by id)")
                                   : conn.prepare(R"(select id, title, status from questions
where scope_kind = ? and scope_id is null
order by id)");
  if (!stmt.has_value() || !stmt->bind_text(1, scope_kind).has_value()) {
    return std::unexpected(diff_error::query_failed);
  }
  if (scope_id.has_value() && !stmt->bind_int64(2, *scope_id).has_value()) {
    return std::unexpected(diff_error::query_failed);
  }

  std::vector<db_question> out;
  while (true) {
    const auto stepped = stmt->step();
    if (!stepped.has_value()) {
      return std::unexpected(diff_error::query_failed);
    }
    if (*stepped == db::step_result::done) {
      break;
    }
    out.push_back({.id_ = stmt->column_int64(0), .title_ = stmt->column_text(1), .status_ = stmt->column_text(2)});
  }
  return out;
}

/// @brief Loads the live test scenarios linked to the anchor by `derives-from`.
[[nodiscard]] auto load_scenarios_for_anchor(db::connection& conn, std::int64_t anchor_id)
    -> std::expected<std::vector<db_scenario>, diff_error> {
  auto stmt = conn.prepare(R"(select ts.id, ts.title, coalesce(ts.body, '')
from test_scenarios ts
join entity_links el on el.from_kind = 'test_scenario' and el.from_id = ts.id
   and el.to_kind = 'plan' and el.to_id = ?
   and el.relationship = 'derives-from'
where ts.status != 'retired'
order by ts.id)");
  if (!stmt.has_value() || !stmt->bind_int64(1, anchor_id).has_value()) {
    return std::unexpected(diff_error::query_failed);
  }
  std::vector<db_scenario> out;
  while (true) {
    const auto stepped = stmt->step();
    if (!stepped.has_value()) {
      return std::unexpected(diff_error::query_failed);
    }
    if (*stepped == db::step_result::done) {
      break;
    }
    out.push_back({.id_ = stmt->column_int64(0), .title_ = stmt->column_text(1), .body_ = stmt->column_text(2)});
  }
  return out;
}

/// @brief Finds a task outside `excluded_task_ids` already holding `slug`.
///
/// No status filter, deliberately: the unique index
/// (`ux_tasks_slug on tasks(slug) where slug is not null`, migration 00011) is
/// status-agnostic, and `planar task cancel` does not null the slug. A
/// cancelled-but-slugged task therefore still holds the index slot — and is
/// exactly the holder an operator's obvious search misses, because the usual
/// listing verbs hide non-open tasks.
///
/// `existing_plan_id_` is read from the `tasks.plan_id` COLUMN rather than a
/// link edge, because a task created by `task add --plan` sets only the column.
[[nodiscard]] auto find_global_slug_collision(db::connection& conn, std::string_view slug,
                                              const std::set<std::int64_t>& excluded_task_ids)
    -> std::expected<std::optional<slug_collision>, diff_error> {
  auto stmt = conn.prepare(R"(select id, coalesce(plan_id, 0)
from tasks
where slug = ?
order by id limit 1)");
  if (!stmt.has_value() || !stmt->bind_text(1, slug).has_value()) {
    return std::unexpected(diff_error::query_failed);
  }
  const auto stepped = stmt->step();
  if (!stepped.has_value()) {
    return std::unexpected(diff_error::query_failed);
  }
  if (*stepped == db::step_result::done) {
    return std::optional<slug_collision>{};
  }
  const auto task_id = stmt->column_int64(0);
  const auto plan_id = stmt->column_int64(1);
  if (excluded_task_ids.contains(task_id)) {
    return std::optional<slug_collision>{};
  }
  return std::optional<slug_collision>{
      slug_collision{.slug_ = std::string{slug}, .existing_task_id_ = task_id, .existing_plan_id_ = plan_id}};
}

/// @brief Whether a decision with `title` is already queued in this pass.
[[nodiscard]] auto decision_already_queued(std::span<const decision_entry> items, std::string_view title) -> bool {
  return std::ranges::any_of(items, [title](const decision_entry& e) { return titles_equal_ignore_case(e.title_, title); });
}

} // namespace

auto to_string(op value) -> std::string_view {
  switch (value) {
  case op::add:
    return "add";
  case op::update:
    return "update";
  case op::remove:
    return "remove";
  }
  return "add";
}

auto diff::total_additions() const -> std::size_t {
  std::size_t n = 0;
  for (const auto& plan : child_plans_) {
    if (plan.op_ == op::add) {
      ++n;
    }
    n += static_cast<std::size_t>(std::ranges::count_if(plan.tasks_, [](const task_entry& t) { return t.op_ == op::add; }));
  }
  n += static_cast<std::size_t>(std::ranges::count_if(decisions_, [](const decision_entry& d) { return d.op_ == op::add; }));
  n += new_questions_.size();
  return n;
}

auto diff::total_updates() const -> std::size_t {
  std::size_t n = 0;
  for (const auto& plan : child_plans_) {
    n += static_cast<std::size_t>(std::ranges::count_if(plan.tasks_, [](const task_entry& t) { return t.op_ == op::update; }));
  }
  n += static_cast<std::size_t>(std::ranges::count_if(decisions_, [](const decision_entry& d) { return d.op_ == op::update; }));
  n += updated_question_status_.size();
  return n;
}

auto diff::total_removals() const -> std::size_t {
  return orphan_tasks_.size() + orphan_plans_.size();
}

auto diff::is_empty() const -> bool {
  return total_additions() == 0 && total_updates() == 0 && total_removals() == 0;
}

auto is_non_trivial(std::string_view body) -> bool {
  std::size_t count = 0;
  std::size_t start = 0;
  while (start <= body.size()) {
    const auto nl   = body.find('\n', start);
    const auto line = body.substr(start, nl == std::string_view::npos ? std::string_view::npos : nl - start);
    if (const auto t = trim_inline(line); t.starts_with("- ") || t.starts_with("* ")) {
      if (++count >= 2) {
        return true;
      }
    }
    if (nl == std::string_view::npos) {
      break;
    }
    start = nl + 1;
  }
  return false;
}

auto build_task_body(const parse::work_item& item) -> std::string {
  std::string out = "## Acceptance Criteria\n\n- ";
  out.append(item.title_);
  out.push_back('\n');
  if (!item.touches_.empty()) {
    out.append("\n## Repository Scope\n\n");
    for (const auto& slug : item.touches_) {
      out.append("- touches: ");
      out.append(slug);
      out.push_back('\n');
    }
  }
  return out;
}

auto build_legacy_task_body(const parse::work_item& item) -> std::string {
  std::string out = "## Acceptance Criteria\n\n- ";
  out.append(item.title_);
  out.append(legacy_acceptance_suffix);
  out.push_back('\n');
  if (!item.touches_.empty()) {
    out.append("\n## Repository Scope\n\n");
    for (const auto& slug : item.touches_) {
      out.append("- touches: ");
      out.append(slug);
      out.push_back('\n');
    }
  }
  return out;
}

auto build_task_next_action(const parse::work_item& item, std::size_t milestone_index, std::size_t item_index) -> std::string {
  return std::format("Deliver roadmap milestone {} item {}: {}", milestone_index, item_index, item.title_);
}

auto is_generated_task_body(std::string_view stored, const parse::work_item& item) -> bool {
  return stored == build_task_body(item) || stored == build_legacy_task_body(item);
}

auto build_scenario_body(const parse::scenario& s) -> std::string {
  std::string out;
  if (!s.verifies_.empty()) {
    out.append("**Verifies:** ");
    for (std::size_t i = 0; i < s.verifies_.size(); ++i) {
      if (i > 0) {
        out.append(", ");
      }
      out.append(s.verifies_[i].kind_);
      out.push_back(':');
      if (!s.verifies_[i].slug_.empty()) {
        out.append(s.verifies_[i].slug_);
      } else {
        out.append(std::format("{}", s.verifies_[i].id_));
      }
    }
    out.push_back('\n');
  }
  if (!s.kind_.empty()) {
    out.append("**Kind:** ");
    out.append(s.kind_);
    out.push_back('\n');
  }
  if (!s.acceptance_.empty()) {
    out.append("**Acceptance:** ");
    out.append(s.acceptance_);
    out.push_back('\n');
  }
  if (!s.body_.empty()) {
    if (!out.empty()) {
      out.push_back('\n');
    }
    out.append(s.body_);
  }
  constexpr std::string_view ws    = " \t\n\r";
  const auto                 first = out.find_first_not_of(ws);
  if (first == std::string::npos) {
    return {};
  }
  return out.substr(first, out.find_last_not_of(ws) - first + 1);
}

auto compute(db::connection& conn, std::int64_t anchor_plan_id, std::span<const parse::milestone> milestones,
             std::span<const parse::decision> decisions, std::span<const parse::question> questions,
             std::span<const parse::scenario> scenarios) -> std::expected<diff, diff_error> {
  diff result{};
  result.anchor_plan_id_ = anchor_plan_id;

  // ---- anchor plan ------------------------------------------------------
  const auto anchor = load_anchor_plan(conn, anchor_plan_id);
  if (!anchor.has_value()) {
    return std::unexpected(anchor.error());
  }
  result.anchor_slug_    = anchor->slug_;
  result.assoc_slug_     = anchor->assoc_slug_;
  result.current_status_ = anchor->status_;

  // ---- child plans + tasks ---------------------------------------------
  const auto existing_plans = load_child_plans(conn, anchor_plan_id);
  if (!existing_plans.has_value()) {
    return std::unexpected(existing_plans.error());
  }
  std::set<std::int64_t> used_plan_ids;

  for (std::size_t milestone_index = 0; milestone_index < milestones.size(); ++milestone_index) {
    const auto& ms       = milestones[milestone_index];
    const auto  ms_title = std::string{trim_inline(ms.name_)};
    const auto* existing = by_title(std::span<const db_child_plan>{*existing_plans}, ms_title);

    plan_entry entry{.op_          = existing != nullptr ? op::update : op::add,
                     .title_       = ms_title,
                     .existing_id_ = existing != nullptr ? existing->id_ : 0,
                     .tasks_       = {}};
    if (existing != nullptr) {
      used_plan_ids.insert(existing->id_);
    }

    std::vector<db_task> existing_tasks;
    if (existing != nullptr) {
      auto loaded = load_tasks_for_plan(conn, existing->id_);
      if (!loaded.has_value()) {
        return std::unexpected(loaded.error());
      }
      existing_tasks = std::move(*loaded);
    }
    std::set<std::int64_t> used_task_ids;

    for (std::size_t item_index = 0; item_index < ms.work_items_.size(); ++item_index) {
      const auto& item          = ms.work_items_[item_index];
      const auto  wi_title      = std::string{trim_inline(item.title_)};
      const auto  task_body     = build_task_body(item);
      const auto* existing_task = by_title(std::span<const db_task>{existing_tasks}, wi_title);

      // Provenance is recorded for EVERY bullet, including one whose task is
      // unchanged and therefore never reaches the mutation lists below.
      result.roadmap_citations_.push_back(
          {.existing_task_id_ = existing_task != nullptr ? existing_task->id_ : 0,
           .child_plan_title_ = ms_title,
           .task_title_       = wi_title,
           .source_locator_   = std::format("roadmap#milestone:{}/item:{}", milestone_index + 1, item_index + 1),
           .source_text_      = item.source_text_.empty() ? item.title_ : item.source_text_});

      auto next_action = build_task_next_action(item, milestone_index + 1, item_index + 1);

      if (existing_task != nullptr) {
        used_task_ids.insert(existing_task->id_);

        // "Generated" is decided by byte-comparison against every shipped
        // projection, so an operator's enriched body is never overwritten and
        // a body written before the legacy suffix was dropped still upgrades.
        const bool generated           = is_generated_task_body(existing_task->body_, item);
        const bool body_changed        = generated && existing_task->body_ != task_body;
        const bool slug_needs_backfill = !item.slug_.empty() && existing_task->slug_.empty();
        if (!body_changed && !slug_needs_backfill) {
          continue;
        }
        entry.tasks_.push_back(
            {.op_    = op::update,
             .title_ = wi_title,
             // Empty body means "the operator enriched this; leave it alone".
             .body_    = generated ? task_body : std::string{},
             .touches_ = item.touches_,
             .depends_ = item.depends_,
             .slug_    = item.slug_,
             // Only a still-legacy next action is replaced; a refined one is
             // operator work.
             .next_action_      = existing_task->next_action_ == legacy_next_action ? std::move(next_action) : std::string{},
             .existing_id_      = existing_task->id_,
             .child_plan_title_ = ms_title});
      } else {
        entry.tasks_.push_back({.op_               = op::add,
                                .title_            = wi_title,
                                .body_             = task_body,
                                .touches_          = item.touches_,
                                .depends_          = item.depends_,
                                .slug_             = item.slug_,
                                .next_action_      = std::move(next_action),
                                .existing_id_      = 0,
                                .child_plan_title_ = ms_title});
      }
    }

    // Orphan tasks: stored under this milestone but absent from the roadmap.
    for (const auto& stored : existing_tasks) {
      if (used_task_ids.contains(stored.id_)) {
        continue;
      }
      result.orphan_tasks_.push_back({.op_               = op::remove,
                                      .title_            = stored.title_,
                                      .body_             = stored.body_,
                                      .touches_          = {},
                                      .depends_          = {},
                                      .slug_             = {},
                                      .next_action_      = {},
                                      .existing_id_      = stored.id_,
                                      .child_plan_title_ = ms_title});
    }

    result.child_plans_.push_back(std::move(entry));
  }

  // ---- orphan child plans ----------------------------------------------
  for (const auto& stored_plan : *existing_plans) {
    if (used_plan_ids.contains(stored_plan.id_)) {
      continue;
    }
    plan_entry orphan{.op_ = op::remove, .title_ = stored_plan.title_, .existing_id_ = stored_plan.id_, .tasks_ = {}};
    const auto kid_tasks = load_tasks_for_plan(conn, stored_plan.id_);
    if (!kid_tasks.has_value()) {
      return std::unexpected(kid_tasks.error());
    }
    for (const auto& kid : *kid_tasks) {
      orphan.tasks_.push_back({.op_               = op::remove,
                               .title_            = kid.title_,
                               .body_             = {},
                               .touches_          = {},
                               .depends_          = {},
                               .slug_             = {},
                               .next_action_      = {},
                               .existing_id_      = kid.id_,
                               .child_plan_title_ = stored_plan.title_});
    }
    result.orphan_plans_.push_back(std::move(orphan));
  }

  // ---- decisions --------------------------------------------------------
  const auto existing_decisions = load_decisions_for_anchor(conn, anchor_plan_id);
  if (!existing_decisions.has_value()) {
    return std::unexpected(existing_decisions.error());
  }
  for (const auto& parsed : decisions) {
    const auto  dec_title = std::string{trim_inline(parsed.title_)};
    const auto* existing  = by_title(std::span<const db_decision>{*existing_decisions}, dec_title);
    if (existing != nullptr) {
      if (existing->body_ == parsed.body_) {
        continue;
      }
      result.decisions_.push_back({.op_ = op::update, .title_ = dec_title, .body_ = parsed.body_, .existing_id_ = existing->id_});
    } else {
      result.decisions_.push_back({.op_ = op::add, .title_ = dec_title, .body_ = parsed.body_, .existing_id_ = 0});
    }
  }

  // ---- questions --------------------------------------------------------
  const auto existing_questions = load_questions_for_anchor(conn, anchor_plan_id);
  if (!existing_questions.has_value()) {
    return std::unexpected(existing_questions.error());
  }
  for (const auto& parsed : questions) {
    const auto  q_title  = std::string{trim_inline(parsed.title_)};
    const auto* existing = by_title(std::span<const db_question>{*existing_questions}, q_title);
    if (existing != nullptr) {
      if (!parsed.resolution_.empty() && existing->status_ == "open") {
        result.updated_question_status_.push_back({.question_id_    = existing->id_,
                                                   .question_title_ = existing->title_,
                                                   .old_status_     = "open",
                                                   .new_status_     = "answered",
                                                   .answer_         = parsed.resolution_});
        // A resolved question also proposes the derived decision, unless one
        // with that title already exists or is already queued this pass.
        if (by_title(std::span<const db_decision>{*existing_decisions}, q_title) == nullptr &&
            !decision_already_queued(result.decisions_, q_title)) {
          result.decisions_.push_back({.op_ = op::add, .title_ = q_title, .body_ = parsed.resolution_, .existing_id_ = 0});
        }
      }
      continue;
    }
    result.new_questions_.push_back(parsed);
    if (!parsed.resolution_.empty() && by_title(std::span<const db_decision>{*existing_decisions}, q_title) == nullptr &&
        !decision_already_queued(result.decisions_, q_title)) {
      result.decisions_.push_back({.op_ = op::add, .title_ = q_title, .body_ = parsed.resolution_, .existing_id_ = 0});
    }
  }

  // ---- test-spec scenarios ---------------------------------------------
  if (!scenarios.empty()) {
    const auto existing_scenarios = load_scenarios_for_anchor(conn, anchor_plan_id);
    if (!existing_scenarios.has_value()) {
      return std::unexpected(existing_scenarios.error());
    }
    for (const auto& parsed : scenarios) {
      const auto title = std::string{trim_inline(parsed.title_)};
      if (title.empty()) {
        continue;
      }
      auto        body_text = build_scenario_body(parsed);
      const auto* existing  = by_title(std::span<const db_scenario>{*existing_scenarios}, title);
      if (existing != nullptr) {
        if (existing->body_ == body_text) {
          continue;
        }
        result.scenarios_.push_back({.op_          = op::update,
                                     .title_       = title,
                                     .body_        = std::move(body_text),
                                     .kind_        = parsed.kind_,
                                     .acceptance_  = parsed.acceptance_,
                                     .verifies_    = parsed.verifies_,
                                     .existing_id_ = existing->id_});
      } else {
        result.scenarios_.push_back({.op_          = op::add,
                                     .title_       = title,
                                     .body_        = std::move(body_text),
                                     .kind_        = parsed.kind_,
                                     .acceptance_  = parsed.acceptance_,
                                     .verifies_    = parsed.verifies_,
                                     .existing_id_ = 0});
      }
    }
  }

  // ---- global slug-collision check -------------------------------------
  //
  // A slug held by a task this very ingest is UPDATING is not a collision: the
  // update path back-fills only when the stored slug is NULL, so it never
  // touches an already-set one.
  std::set<std::int64_t> ingest_task_ids;
  for (const auto& plan : result.child_plans_) {
    for (const auto& task : plan.tasks_) {
      if (task.op_ == op::update && task.existing_id_ > 0) {
        ingest_task_ids.insert(task.existing_id_);
      }
    }
  }
  for (const auto& plan : result.child_plans_) {
    for (const auto& task : plan.tasks_) {
      if (task.op_ != op::add || task.slug_.empty()) {
        continue;
      }
      auto collision = find_global_slug_collision(conn, task.slug_, ingest_task_ids);
      if (!collision.has_value()) {
        return std::unexpected(collision.error());
      }
      if (collision->has_value()) {
        result.slug_collisions_.push_back(std::move(**collision));
      }
    }
  }

  return result;
}

} // namespace planar::engine::ingest::diff
