/// @file strategy.cpp
/// @brief Implementation of `planar.engine.planning.strategy` (plan 996, task
/// 6310). See strategy.cppm for the six rules, the shared-substrate rationale,
/// and the full list of places the two verbs disagree.

module planar.engine.planning.strategy;

import std;
import planar.db;
import planar.json_text;

namespace planar::engine::planning::strategy {

using json_text::append_json_string;

namespace {

// =========================================================================
// Internal working representation (SHARED by both entry points)
// =========================================================================

/// A single touch. `path` is the repo-relative file path for a path-level
/// touch, the SYMBOL for a derived-closure touch, or `nullopt` for a coarse
/// whole-repo touch.
///
/// Repo identity is carried EXPLICITLY rather than folded into a flat token,
/// so a coarse whole-repo claim collides with a same-repo path touch. A bare
/// path string carries no repo identity and could not express that overlap.
struct touch {
  std::int64_t               repo_id = 0;
  std::optional<std::string> path;

  /// Two touches conflict iff they name the same repo AND (either is
  /// whole-repo OR they name the same path). Different repos never conflict;
  /// two distinct paths in one repo do not conflict, which is what enables
  /// intra-repo parallelism.
  [[nodiscard]] auto conflicts(const touch& other) const -> bool {
    if (repo_id != other.repo_id) {
      return false;
    }
    if (!path.has_value() || !other.path.has_value()) {
      return true; // one side claims the whole repo
    }
    return *path == *other.path;
  }
};

struct work_task {
  std::int64_t               id = 0;
  std::optional<std::string> slug;
  std::string                title;
  /// DECLARED touches. Rules 2-empty, 3 and 4 always read THIS set.
  std::vector<touch> touches;
  /// DERIVED closure touches. Empty under `declared`; rule 2's overlap branch
  /// reads it under `derived`.
  std::vector<touch>     closure_touches;
  std::vector<exclusion> exclusions;
  bool                   dropped = false;

  /// The set rule 2's OVERLAP pass reads for `source`.
  [[nodiscard]] auto overlap_touches(closure_source source) const -> const std::vector<touch>& {
    return source == closure_source::declared ? touches : closure_touches;
  }
};

// =========================================================================
// Touch-set helpers
// =========================================================================

/// Append `{repo_id, path}` unless an identical pair is already present.
///
/// The dedup deliberately compares against path-bearing entries only: a
/// whole-repo entry for the same repo does NOT suppress a path entry, matching
/// the oracle's `appendUniquePath`.
auto append_unique_path(std::vector<touch>& out, std::int64_t repo_id, std::string path) -> void {
  for (auto const& existing : out) {
    if (existing.repo_id == repo_id && existing.path.has_value() && *existing.path == path) {
      return;
    }
  }
  out.push_back(touch{.repo_id = repo_id, .path = std::move(path)});
}

/// Append a whole-repo touch unless one is already present for that repo.
auto append_unique_whole_repo(std::vector<touch>& out, std::int64_t repo_id) -> void {
  for (auto const& existing : out) {
    if (existing.repo_id == repo_id && !existing.path.has_value()) {
      return;
    }
  }
  out.push_back(touch{.repo_id = repo_id, .path = std::nullopt});
}

/// The first touch of `a` that conflicts with some touch of `b`, or `nullopt`
/// when the two sets are disjoint.
///
/// Returning the touch from the FIRST argument is load-bearing: the caller
/// formats ONE description from it and writes that same text into BOTH sides'
/// exclusion reasons.
auto shared_touch(const std::vector<touch>& a, const std::vector<touch>& b) -> std::optional<touch> {
  for (auto const& ta : a) {
    for (auto const& tb : b) {
      if (ta.conflicts(tb)) {
        return ta;
      }
    }
  }
  return std::nullopt;
}

/// Render a touch for an operator-facing exclusion reason.
auto describe_touch(const touch& t) -> std::string {
  if (t.path.has_value()) {
    return *t.path;
  }
  return std::format("repo:{} (whole repo)", t.repo_id);
}

/// Record an exclusion and mark the task dropped.
auto add_exclusion(work_task& t, std::uint8_t rule, std::string reason) -> void {
  t.exclusions.push_back(exclusion{.rule = rule, .reason = std::move(reason)});
  t.dropped = true;
}

// =========================================================================
// DB helpers (read-only) — THE SHARED LOADER SUBSTRATE
// =========================================================================

auto ensure_plan_exists(db::connection& conn, std::int64_t plan_id) -> std::expected<void, strategy_error> {
  auto stmt = conn.prepare("select count(*) from plans where id = ?");
  if (!stmt) {
    return std::unexpected(strategy_error::query_failed);
  }
  if (auto bound = stmt->bind_int64(1, plan_id); !bound) {
    return std::unexpected(strategy_error::query_failed);
  }
  auto stepped = stmt->step();
  if (!stepped) {
    return std::unexpected(strategy_error::query_failed);
  }
  if (*stepped == db::step_result::done || stmt->column_int64(0) == 0) {
    return std::unexpected(strategy_error::not_found);
  }
  return {};
}

/// The plan's not-done ids, for rule 1's closure intersection.
///
/// "Not done" is `status not in ('done','cancelled')` — WIDER than the `todo`
/// candidate set, so a `doing` or `blocked` task still blocks a dependant even
/// though it is never itself a candidate.
auto load_not_done_ids(db::connection& conn, std::int64_t plan_id) -> std::expected<std::set<std::int64_t>, strategy_error> {
  auto stmt = conn.prepare("select id from tasks where plan_id = ? and status not in ('done','cancelled')");
  if (!stmt) {
    return std::unexpected(strategy_error::query_failed);
  }
  if (auto bound = stmt->bind_int64(1, plan_id); !bound) {
    return std::unexpected(strategy_error::query_failed);
  }
  std::set<std::int64_t> out;
  while (true) {
    auto stepped = stmt->step();
    if (!stepped) {
      return std::unexpected(strategy_error::query_failed);
    }
    if (*stepped == db::step_result::done) {
      break;
    }
    out.insert(stmt->column_int64(0));
  }
  return out;
}

/// A task's DECLARED touch set.
///
/// Path-level detail REFINES the coarse repo signal: a repo carrying any
/// `task_touch_paths` row contributes its PATHS and NOT a whole-repo touch, so
/// two tasks editing different files in one repo are disjoint. A repo touched
/// only through the coarse `entity_links` edge contributes a whole-repo claim
/// that conflicts with any same-repo touch.
auto load_touches(db::connection& conn, std::int64_t task_id) -> std::expected<std::vector<touch>, strategy_error> {
  std::vector<touch>     out;
  std::set<std::int64_t> refined; // repos with path-level declarations

  {
    auto stmt = conn.prepare("select repo_id, path from task_touch_paths where task_id = ?");
    if (!stmt) {
      return std::unexpected(strategy_error::query_failed);
    }
    if (auto bound = stmt->bind_int64(1, task_id); !bound) {
      return std::unexpected(strategy_error::query_failed);
    }
    while (true) {
      auto stepped = stmt->step();
      if (!stepped) {
        return std::unexpected(strategy_error::query_failed);
      }
      if (*stepped == db::step_result::done) {
        break;
      }
      auto const repo_id = stmt->column_int64(0);
      refined.insert(repo_id);
      append_unique_path(out, repo_id, stmt->column_text(1));
    }
  }

  {
    auto stmt = conn.prepare("select el.to_id from entity_links el "
                             "where el.from_kind = 'task' and el.from_id = ? "
                             "and el.to_kind = 'repo' and el.relationship = 'touches'");
    if (!stmt) {
      return std::unexpected(strategy_error::query_failed);
    }
    if (auto bound = stmt->bind_int64(1, task_id); !bound) {
      return std::unexpected(strategy_error::query_failed);
    }
    while (true) {
      auto stepped = stmt->step();
      if (!stepped) {
        return std::unexpected(strategy_error::query_failed);
      }
      if (*stepped == db::step_result::done) {
        break;
      }
      auto const repo_id = stmt->column_int64(0);
      if (refined.contains(repo_id)) {
        continue; // path detail wins over the coarse signal
      }
      append_unique_whole_repo(out, repo_id);
    }
  }

  return out;
}

/// A task's DERIVED overlap set: one touch per effective-closure row, with the
/// SYMBOL carried in the `path` field so `touch::conflicts` compares symbols.
///
/// `role in ('modify','reference')` — `transitive` is excluded from the
/// effective closure (migration 00026).
auto load_closure_touches(db::connection& conn, std::int64_t task_id) -> std::expected<std::vector<touch>, strategy_error> {
  auto stmt = conn.prepare("select repo_id, symbol from closures "
                           "where task_id = ? and role in ('modify', 'reference')");
  if (!stmt) {
    return std::unexpected(strategy_error::query_failed);
  }
  if (auto bound = stmt->bind_int64(1, task_id); !bound) {
    return std::unexpected(strategy_error::query_failed);
  }
  std::vector<touch> out;
  while (true) {
    auto stepped = stmt->step();
    if (!stepped) {
      return std::unexpected(strategy_error::query_failed);
    }
    if (*stepped == db::step_result::done) {
      break;
    }
    append_unique_path(out, stmt->column_int64(0), stmt->column_text(1));
  }
  return out;
}

/// The plan's open (`todo`) candidate tasks, ordered `priority, id`.
///
/// `closure_touches` is loaded ONLY under `derived`, so the baseline path does
/// zero extra DB work — matching the oracle's byte-for-byte pre-D4 claim.
auto load_open_tasks(db::connection& conn, std::int64_t plan_id, closure_source source)
    -> std::expected<std::vector<work_task>, strategy_error> {
  auto stmt = conn.prepare("select id, slug, title from tasks "
                           "where plan_id = ? and status = 'todo' order by priority, id");
  if (!stmt) {
    return std::unexpected(strategy_error::query_failed);
  }
  if (auto bound = stmt->bind_int64(1, plan_id); !bound) {
    return std::unexpected(strategy_error::query_failed);
  }

  std::vector<work_task> out;
  while (true) {
    auto stepped = stmt->step();
    if (!stepped) {
      return std::unexpected(strategy_error::query_failed);
    }
    if (*stepped == db::step_result::done) {
      break;
    }
    work_task t;
    t.id    = stmt->column_int64(0);
    t.slug  = stmt->is_null(1) ? std::optional<std::string>{} : std::optional<std::string>{stmt->column_text(1)};
    t.title = stmt->column_text(2);

    auto touches = load_touches(conn, t.id);
    if (!touches) {
      return std::unexpected(touches.error());
    }
    t.touches = std::move(*touches);

    if (source == closure_source::derived) {
      auto closures = load_closure_touches(conn, t.id);
      if (!closures) {
        return std::unexpected(closures.error());
      }
      t.closure_touches = std::move(*closures);
    }
    out.push_back(std::move(t));
  }
  return out;
}

/// Rule 1: walk the transitive closure of outgoing `depends-on` edges and
/// return the first reached task that is in the plan's not-done set.
///
/// The relationship is `'depends-on'`, NOT `'blocks'` — migration 00033
/// renamed it and the oracle's surrounding comments were never updated.
auto blocked_by_not_done(db::connection& conn, std::int64_t task_id, const std::set<std::int64_t>& not_done)
    -> std::expected<std::optional<std::int64_t>, strategy_error> {
  std::set<std::int64_t>    seen{task_id};
  std::vector<std::int64_t> queue{task_id};

  while (!queue.empty()) {
    auto const cur = queue.back();
    queue.pop_back();

    auto stmt = conn.prepare("select to_id from entity_links "
                             "where from_kind = 'task' and from_id = ? "
                             "and to_kind = 'task' and relationship = 'depends-on'");
    if (!stmt) {
      return std::unexpected(strategy_error::query_failed);
    }
    if (auto bound = stmt->bind_int64(1, cur); !bound) {
      return std::unexpected(strategy_error::query_failed);
    }
    while (true) {
      auto stepped = stmt->step();
      if (!stepped) {
        return std::unexpected(strategy_error::query_failed);
      }
      if (*stepped == db::step_result::done) {
        break;
      }
      auto const blocker = stmt->column_int64(0);
      // The self-edge guard matters: a task depending on itself must not
      // report itself as its own blocker.
      if (blocker != task_id && not_done.contains(blocker)) {
        return std::optional<std::int64_t>{blocker};
      }
      if (!seen.contains(blocker)) {
        seen.insert(blocker);
        queue.push_back(blocker);
      }
    }
  }
  return std::optional<std::int64_t>{};
}

/// Rules 5 & 6: is the task linked, in EITHER direction, to a `kind` entity
/// whose status is `status_value`?
auto linked_to_unresolved(db::connection& conn, std::string_view kind, std::string_view status_value, std::int64_t task_id)
    -> std::expected<bool, strategy_error> {
  // `kind` is a module-internal literal (`question` / `decision`), never
  // operator input, so interpolating the table name is safe here.
  auto const sql  = std::format("select 1 from entity_links el join {0}s e on e.id = "
                                "(case when el.from_kind = 'task' then el.to_id else el.from_id end) "
                                "where ((el.from_kind = 'task' and el.from_id = ? and el.to_kind = '{0}') "
                                "or (el.to_kind = 'task' and el.to_id = ? and el.from_kind = '{0}')) "
                                "and e.status = ? limit 1",
                                kind);
  auto       stmt = conn.prepare(sql);
  if (!stmt) {
    return std::unexpected(strategy_error::query_failed);
  }
  if (auto b1 = stmt->bind_int64(1, task_id); !b1) {
    return std::unexpected(strategy_error::query_failed);
  }
  if (auto b2 = stmt->bind_int64(2, task_id); !b2) {
    return std::unexpected(strategy_error::query_failed);
  }
  if (auto b3 = stmt->bind_text(3, status_value); !b3) {
    return std::unexpected(strategy_error::query_failed);
  }
  auto stepped = stmt->step();
  if (!stepped) {
    return std::unexpected(strategy_error::query_failed);
  }
  return *stepped == db::step_result::row;
}

constexpr std::string_view k_singleton_files[] = {
    "CLAUDE.md",
    "AGENTS.md",
    "docs/cli-reference.md",
    "docs/architecture.md",
};

auto bool_text(bool value) -> std::string_view {
  return value ? std::string_view{"true"} : std::string_view{"false"};
}

} // namespace

// =========================================================================
// Classifiers
// =========================================================================

auto singleton_files() -> std::span<const std::string_view> {
  return k_singleton_files;
}

auto is_singleton_file(std::string_view path) -> bool {
  return std::ranges::find(k_singleton_files, path) != std::ranges::end(k_singleton_files);
}

auto is_migration_path(std::string_view path) -> bool {
  return path.starts_with("migrations/") && path.ends_with(".sql");
}

auto parse_closure_source(std::string_view text) -> std::optional<closure_source> {
  if (text == "declared") {
    return closure_source::declared;
  }
  if (text == "derived") {
    return closure_source::derived;
  }
  return std::nullopt;
}

auto closure_source_name(closure_source source) -> std::string_view {
  return source == closure_source::declared ? std::string_view{"declared"} : std::string_view{"derived"};
}

// =========================================================================
// recommend-strategy
// =========================================================================

auto recommend_with(db::connection& conn, std::int64_t plan_id, closure_source source)
    -> std::expected<recommendation, strategy_error> {
  if (auto exists = ensure_plan_exists(conn, plan_id); !exists) {
    return std::unexpected(exists.error());
  }

  auto not_done = load_not_done_ids(conn, plan_id);
  if (!not_done) {
    return std::unexpected(not_done.error());
  }
  auto loaded = load_open_tasks(conn, plan_id, source);
  if (!loaded) {
    return std::unexpected(loaded.error());
  }
  auto tasks = std::move(*loaded);

  // The rule order below IS the `excluded_by` array order. It is not sorted
  // by rule number, and a task tripping several rules emits them in exactly
  // this sequence.

  // ---- Rule 1: depends-on chain to a not-done task ----------------------
  for (auto& t : tasks) {
    auto blocker = blocked_by_not_done(conn, t.id, *not_done);
    if (!blocker) {
      return std::unexpected(blocker.error());
    }
    if (blocker->has_value()) {
      add_exclusion(t, 1, std::format("excluded by rule 1: blocked_by not-done task {}", **blocker));
    }
  }

  // ---- Rule 5: open question linked -------------------------------------
  for (auto& t : tasks) {
    auto linked = linked_to_unresolved(conn, "question", "open", t.id);
    if (!linked) {
      return std::unexpected(linked.error());
    }
    if (*linked) {
      add_exclusion(t, 5, "excluded by rule 5: linked to an open question");
    }
  }

  // ---- Rule 6: proposed decision linked ---------------------------------
  for (auto& t : tasks) {
    auto linked = linked_to_unresolved(conn, "decision", "proposed", t.id);
    if (!linked) {
      return std::unexpected(linked.error());
    }
    if (*linked) {
      add_exclusion(t, 6, "excluded by rule 6: linked to a proposed decision");
    }
  }

  // ---- Rule 2 (empty-touches branch) ------------------------------------
  // An empty DECLARED set means "touches everything" -> never eligible, so the
  // failure mode of omission is safe (serialize) rather than false-parallel.
  // NOTE this branch reads `touches` even under `derived`.
  for (auto& t : tasks) {
    if (t.touches.empty()) {
      add_exclusion(t, 2, "excluded by rule 2: no task_touches declared (treated as touches-everything)");
    }
  }

  // ---- Rule 3: migration touched (unilateral) ---------------------------
  for (auto& t : tasks) {
    for (auto const& tch : t.touches) {
      if (!tch.path.has_value()) {
        continue;
      }
      if (is_migration_path(*tch.path)) {
        add_exclusion(t, 3, std::format("excluded by rule 3: touches {}", *tch.path));
        break;
      }
    }
  }

  // ---- Rule 4: singleton authoritative file (unilateral) ----------------
  for (auto& t : tasks) {
    for (auto const& tch : t.touches) {
      if (!tch.path.has_value()) {
        continue;
      }
      if (is_singleton_file(*tch.path)) {
        add_exclusion(t, 4, std::format("excluded by rule 4: touches singleton authoritative file {}", *tch.path));
        break;
      }
    }
  }

  // ---- Rule 2 (overlap branch): drop-both-on-tie ------------------------
  // Runs ONLY over survivors of the unilateral rules, so an already-dropped
  // task cannot cascade a rule-2 exclusion onto a peer whose only conflict was
  // with it. The asymmetry is deliberate and oracle-verified: `j` is
  // re-checked for `dropped` every iteration, `i` only once, so `i` can
  // accumulate several rule-2 exclusions while a `j` dropped earlier in this
  // same pass is skipped later.
  auto const n = tasks.size();
  for (std::size_t i = 0; i < n; ++i) {
    if (tasks[i].dropped) {
      continue;
    }
    for (std::size_t j = i + 1; j < n; ++j) {
      if (tasks[j].dropped) {
        continue;
      }
      auto overlap = shared_touch(tasks[i].overlap_touches(source), tasks[j].overlap_touches(source));
      if (!overlap.has_value()) {
        continue;
      }
      // ONE description, written into BOTH sides.
      auto const desc = describe_touch(*overlap);
      add_exclusion(tasks[i], 2, std::format("excluded by rule 2: overlaps task {} on {}", tasks[j].id, desc));
      add_exclusion(tasks[j], 2, std::format("excluded by rule 2: overlaps task {} on {}", tasks[i].id, desc));
    }
  }

  recommendation rec;
  rec.plan_id    = plan_id;
  rec.open_tasks = tasks.size();
  for (auto& t : tasks) {
    task_ref ref{.id = t.id, .slug = std::move(t.slug), .title = std::move(t.title), .excluded_by = {}};
    if (t.dropped) {
      ref.excluded_by = std::move(t.exclusions);
      rec.serialized.push_back(std::move(ref));
    } else {
      rec.parallel_eligible.push_back(std::move(ref));
    }
  }
  rec.fan_out_available = rec.parallel_eligible.size() >= 2;
  return rec;
}

auto recommend(db::connection& conn, std::int64_t plan_id) -> std::expected<recommendation, strategy_error> {
  return recommend_with(conn, plan_id, closure_source::declared);
}

// =========================================================================
// divergence
// =========================================================================

auto compute_divergence(db::connection& conn, std::int64_t plan_id) -> std::expected<divergence_result, strategy_error> {
  if (auto exists = ensure_plan_exists(conn, plan_id); !exists) {
    return std::unexpected(exists.error());
  }

  // Load with `declared` (so `load_open_tasks` skips its closure query), then
  // fill the derived set explicitly — one task list carrying BOTH sources.
  auto loaded = load_open_tasks(conn, plan_id, closure_source::declared);
  if (!loaded) {
    return std::unexpected(loaded.error());
  }
  auto tasks = std::move(*loaded);
  for (auto& t : tasks) {
    auto closures = load_closure_touches(conn, t.id);
    if (!closures) {
      return std::unexpected(closures.error());
    }
    t.closure_touches = std::move(*closures);
  }

  // ONLY rule 2's pairwise overlap test. No unilateral rule runs here, and no
  // task is ever dropped -- every open task participates in every pair.
  divergence_result div;
  div.open_tasks         = tasks.size();
  std::size_t union_over = 0;

  auto const n = tasks.size();
  for (std::size_t i = 0; i < n; ++i) {
    for (std::size_t j = i + 1; j < n; ++j) {
      ++div.pairs;
      bool const decl = shared_touch(tasks[i].touches, tasks[j].touches).has_value();
      bool const der  = shared_touch(tasks[i].closure_touches, tasks[j].closure_touches).has_value();
      if (decl) {
        ++div.declared_overlaps;
      }
      if (der) {
        ++div.derived_overlaps;
      }
      if (decl || der) {
        ++union_over;
      }
      if (decl != der) {
        ++div.flips;
      }
    }
  }

  div.jaccard = union_over == 0 ? 0.0 : static_cast<double>(div.flips) / static_cast<double>(union_over);
  return div;
}

// =========================================================================
// Renderers
// =========================================================================

auto recommended_note(const recommendation& rec) -> std::string {
  if (rec.fan_out_available) {
    return std::format("parallel-fanout available: {} eligible tasks", rec.parallel_eligible.size());
  }
  if (rec.parallel_eligible.size() == 1) {
    return "no fan-out: only 1 eligible task; run sequentially";
  }
  return "no fan-out: no eligible tasks; run sequentially";
}

auto render_recommendation_json(const recommendation& rec, closure_source source) -> std::string {
  std::string out = std::format(R"({{"plan_id":{},"closure_source":)", rec.plan_id);
  append_json_string(out, closure_source_name(source));
  out += R"(,"parallel_eligible":[)";
  bool first = true;
  for (auto const& t : rec.parallel_eligible) {
    if (!first) {
      out += ",";
    }
    first = false;
    out += std::format(R"({{"id":{},"slug":)", t.id);
    if (t.slug.has_value()) {
      append_json_string(out, *t.slug);
    } else {
      out += "null";
    }
    out += R"(,"title":)";
    append_json_string(out, t.title);
    out += "}";
  }
  out += R"(],"serialized":[)";
  first = true;
  for (auto const& t : rec.serialized) {
    if (!first) {
      out += ",";
    }
    first = false;
    out += std::format(R"({{"id":{},"slug":)", t.id);
    if (t.slug.has_value()) {
      append_json_string(out, *t.slug);
    } else {
      out += "null";
    }
    out += R"(,"title":)";
    append_json_string(out, t.title);
    out += R"(,"excluded_by":[)";
    bool efirst = true;
    for (auto const& e : t.excluded_by) {
      if (!efirst) {
        out += ",";
      }
      efirst = false;
      out += std::format(R"({{"rule":{},"reason":)", e.rule);
      append_json_string(out, e.reason);
      out += "}";
    }
    out += "]}";
  }
  out +=
      std::format(R"(],"summary":{{"open_tasks":{},"eligible":{},"serialized":{},"fan_out_available":{}}},"recommended_note":)",
                  rec.open_tasks, rec.parallel_eligible.size(), rec.serialized.size(), bool_text(rec.fan_out_available));
  append_json_string(out, recommended_note(rec));
  out += "}\n";
  return out;
}

auto render_recommendation_text(const recommendation& rec, closure_source source) -> std::string {
  std::string out = std::format("plan:{}  source:{}  open:{}  eligible:{}  serialized:{}  fan_out_available:{}\n", rec.plan_id,
                                closure_source_name(source), rec.open_tasks, rec.parallel_eligible.size(), rec.serialized.size(),
                                rec.fan_out_available ? "yes" : "no");
  out += std::format("  {}\n", recommended_note(rec));

  out += "parallel-eligible:\n";
  if (rec.parallel_eligible.empty()) {
    out += "  (none)\n";
  } else {
    for (auto const& t : rec.parallel_eligible) {
      out += std::format("  task:{}  {}\n", t.id, t.title);
    }
  }

  out += "serialized:\n";
  if (rec.serialized.empty()) {
    out += "  (none)\n";
  } else {
    for (auto const& t : rec.serialized) {
      out += std::format("  task:{}  {}\n", t.id, t.title);
      for (auto const& e : t.excluded_by) {
        out += std::format("    - {}\n", e.reason);
      }
    }
  }
  return out;
}

auto render_divergence_json(std::int64_t plan_id, const divergence_result& div) -> std::string {
  // `jaccard` uses `{}` (shortest round-trip) here and `{:.4f}` in the text
  // arm. The two renderings genuinely differ: 0.5 emits `0.5` vs `0.5000`,
  // and 0.0 emits `0` vs `0.0000`.
  return std::format(R"({{"plan_id":{},"open_tasks":{},"pairs":{},"declared_overlaps":{},)"
                     R"("derived_overlaps":{},"flips":{},"jaccard":{}}})"
                     "\n",
                     plan_id, div.open_tasks, div.pairs, div.declared_overlaps, div.derived_overlaps, div.flips, div.jaccard);
}

auto render_divergence_text(std::int64_t plan_id, const divergence_result& div) -> std::string {
  return std::format("plan:{}  open:{}  pairs:{}  declared_overlaps:{}  derived_overlaps:{}  flips:{}  jaccard:{:.4f}\n", plan_id,
                     div.open_tasks, div.pairs, div.declared_overlaps, div.derived_overlaps, div.flips, div.jaccard);
}

} // namespace planar::engine::planning::strategy
