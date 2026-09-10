/// @file editflow.cpp
/// @brief Implementation of `planar.cmd.planar.editflow`.
///
/// The SQL lives here rather than in an engine bucket for the reason the
/// Zig original records under `D-no-engine-edits`: these queries serve the
/// cmd-layer drafting flow only, and pushing them into
/// `engine.workbench` would give the engine a second, thinner renderer
/// beside its canonical one. The two renderers ARE different — see this
/// module's interface header — and keeping the thin one here is what makes
/// that difference visible instead of hiding it inside the engine.
module planar.cmd.planar.editflow;

import std;
import planar.db;
import planar.engine.planning;
import planar.engine.workbench;
import planar.cmd.planar.context;
import planar.cmd.planar.editor;
import planar.cmd.planar.exit;

namespace planar::cmd {

namespace wb = engine::workbench;
namespace pl = engine::planning;

namespace {

/// @brief The failure surface, before it is turned into a `domain_error`.
///
/// Kept separate from `domain_error` because the MESSAGE depends on the
/// calling verb — `diff` and `review` name the family and the id in prose,
/// while `view` and `edit` emit the bare Zig error tag. One internal error
/// set, two renderings.
enum class flow_error : std::uint8_t {
  not_found,
  no_plan_link,
  query_failed,
  cyclic_plan_chain,
  io_failed,
  parse_failed,
  editor_failed,
  illegal_transition,
};

/// @brief The Zig `@errorName` tag for a failure.
///
/// `view` and `edit` write this verbatim after `error: `, because the Zig
/// original's `view`/`edit` handlers have no `catch` arms at all and the
/// runtime prints the tag. `NoPlanLink` was captured from the oracle; the
/// others are the Zig error-set spellings for arms that are not reachable
/// without a corrupt database, and are marked as such rather than being
/// presented as captured.
/// @param err The failure.
/// @return The tag.
auto zig_error_name(flow_error err) -> std::string_view {
  switch (err) {
  case flow_error::not_found:
    return "NotFound";
  case flow_error::no_plan_link:
    return "NoPlanLink"; // oracle-captured
  case flow_error::query_failed:
    return "QueryFailed";
  case flow_error::cyclic_plan_chain:
    return "CyclicPlanChain";
  case flow_error::io_failed:
    return "IoFailed";
  case flow_error::parse_failed:
    return "ParseFailed";
  case flow_error::editor_failed:
    return "EditorFailed";
  case flow_error::illegal_transition:
    return "IllegalTransition";
  }
  return "Unknown";
}

/// @brief The `view`/`edit` rendering: the bare tag, no prose, exit 1.
///
/// THE ONE DELIBERATE DIVERGENCE. The oracle follows this line with seven
/// stack frames naming absolute paths inside its own build tree. The line
/// and the exit code are reproduced; the frames are not. See the interface
/// header.
/// @param err The failure.
/// @return The domain error.
auto bare_error(flow_error err) -> domain_error {
  return error_from_body(domain_error_kind::generic_failure, std::string{zig_error_name(err)});
}

/// @brief The `diff`/`review` rendering: prose naming the family and id.
/// @param err The failure.
/// @param kind The entity kind, for the family noun.
/// @param id The entity id.
/// @param verb The verb name, for the fallback line.
/// @return The domain error.
auto prose_error(flow_error err, entity_kind kind, std::int64_t id, std::string_view verb) -> domain_error {
  auto const label = entity_kind_name(kind);
  switch (err) {
  case flow_error::not_found:
    // THE ARM THAT SPLITS THE SIX FAMILIES, and the reason `plan` and
    // `task` were held for their own oracle run (task 6208) rather than
    // wired alongside the other four at 6205.
    //
    // Unreachable for the four LINK-ANCHORED families: their anchor
    // resolver queries `entity_links` FIRST, before any existence check, so
    // a nonexistent id reports `no_plan_link` and lands on the arm below.
    //
    // For `plan` and `task` it is the only arm reachable. `plan` walks
    // `plans.parent_plan_id` and `task` reads `tasks.plan_id`, both of
    // which miss on a nonexistent id, so:
    //
    //   `plan diff 999`      exit 1, `no plan with id 999`
    //   `task diff 999`      exit 1, `no task with id 999`
    //   `question diff 999`  exit 1, `question 999 is not linked to a plan`
    //
    // Oracle-captured on all three. `not_found` maps to exit 1, same as
    // `generic_failure`, so the exit code does not distinguish them — only
    // the prose does, and `drafting_leaves.t.cpp` pins the prose.
    return error_from_body(domain_error_kind::not_found, std::format("no {} with id {}", label, id));
  case flow_error::no_plan_link:
    return error_from_body(domain_error_kind::generic_failure,
                           std::format("{} {} is not linked to a plan; cannot resolve anchor plan", label, id));
  default:
    return error_from_body(domain_error_kind::generic_failure, std::format("{} {} failed: {}", label, verb, zig_error_name(err)));
  }
}

/// @brief The SQL table backing an entity kind.
/// @param kind The entity kind.
/// @return The table name.
auto entity_table(entity_kind kind) -> std::string_view {
  switch (kind) {
  case entity_kind::plan:
    return "plans";
  case entity_kind::task:
    return "tasks";
  case entity_kind::question:
    return "questions";
  case entity_kind::scenario:
    return "test_scenarios";
  case entity_kind::decision:
    return "decisions";
  case entity_kind::artifact:
    return "artifacts";
  }
  return "";
}

/// @brief Walk `plans.parent_plan_id` upward to the root.
/// @param conn The database connection.
/// @param plan_id The starting plan.
/// @return The anchor id, or the failure.
auto walk_to_anchor(db::connection& conn, std::int64_t plan_id) -> std::expected<std::int64_t, flow_error> {
  auto current = plan_id;
  // The Zig original's bound, kept: a `parent_plan_id` cycle is a corrupt
  // database, and an unbounded loop would hang rather than refuse.
  for (int step = 0; step < 1024; ++step) {
    auto stmt = conn.prepare("select parent_plan_id from plans where id = ?");
    if (!stmt || !stmt->bind_int64(1, current)) {
      return std::unexpected(flow_error::query_failed);
    }
    auto stepped = stmt->step();
    if (!stepped) {
      return std::unexpected(flow_error::query_failed);
    }
    if (*stepped == db::step_result::done) {
      return std::unexpected(flow_error::not_found);
    }
    if (stmt->is_null(0)) {
      return current; // No parent: this IS the anchor.
    }
    current = stmt->column_int64(0);
  }
  return std::unexpected(flow_error::cyclic_plan_chain);
}

/// @brief The plan a task is pinned to.
/// @param conn The database connection.
/// @param task_id The task.
/// @return The plan id, or the failure.
auto task_plan_id(db::connection& conn, std::int64_t task_id) -> std::expected<std::int64_t, flow_error> {
  auto stmt = conn.prepare("select plan_id from tasks where id = ?");
  if (!stmt || !stmt->bind_int64(1, task_id)) {
    return std::unexpected(flow_error::query_failed);
  }
  auto stepped = stmt->step();
  if (!stepped) {
    return std::unexpected(flow_error::query_failed);
  }
  if (*stepped == db::step_result::done) {
    return std::unexpected(flow_error::not_found);
  }
  if (stmt->is_null(0)) {
    // An unpinned task cannot be reached through the editor flow.
    return std::unexpected(flow_error::no_plan_link);
  }
  return stmt->column_int64(0);
}

/// @brief The plan an entity derives from, through `entity_links`.
///
/// THE QUERY THAT DECIDES WHETHER THE WHOLE QUARTET WORKS. It runs BEFORE
/// any existence check, which is why a nonexistent id reports
/// `no_plan_link` rather than `not_found`.
/// @param conn The database connection.
/// @param from_kind The `entity_links.from_kind` spelling.
/// @param id The entity id.
/// @return The plan id, or the failure.
auto link_derived_plan_id(db::connection& conn, std::string_view from_kind, std::int64_t id)
    -> std::expected<std::int64_t, flow_error> {
  auto stmt = conn.prepare("select to_id from entity_links where from_kind = ? and from_id = ? "
                           "and to_kind = 'plan' and relationship = 'derives-from' order by id limit 1");
  if (!stmt || !stmt->bind_text(1, from_kind) || !stmt->bind_int64(2, id)) {
    return std::unexpected(flow_error::query_failed);
  }
  auto stepped = stmt->step();
  if (!stepped) {
    return std::unexpected(flow_error::query_failed);
  }
  if (*stepped == db::step_result::done) {
    return std::unexpected(flow_error::no_plan_link);
  }
  return stmt->column_int64(0);
}

auto resolve_anchor(db::connection& conn, entity_kind kind, std::int64_t id) -> std::expected<std::int64_t, flow_error> {
  switch (kind) {
  case entity_kind::plan:
    return walk_to_anchor(conn, id);
  case entity_kind::task: {
    auto const plan_id = task_plan_id(conn, id);
    if (!plan_id) {
      return std::unexpected(plan_id.error());
    }
    return walk_to_anchor(conn, *plan_id);
  }
  default: {
    auto const plan_id = link_derived_plan_id(conn, entity_link_kind(kind), id);
    if (!plan_id) {
      return std::unexpected(plan_id.error());
    }
    return walk_to_anchor(conn, *plan_id);
  }
  }
}

/// @brief One row's title and status, from any table carrying both.
struct title_status {
  std::string title;
  std::string status;
};

/// @brief Read title and status.
/// @param conn The database connection.
/// @param table The table name; never operator-supplied.
/// @param id The row id.
/// @return The pair, or the failure.
auto fetch_title_status(db::connection& conn, std::string_view table, std::int64_t id)
    -> std::expected<title_status, flow_error> {
  // `table` comes from `entity_table`, a closed six-arm switch over an
  // enum — never from operator input — so the interpolation cannot carry
  // an injection. The id is still bound.
  auto stmt = conn.prepare(std::format("select coalesce(title,''), coalesce(status,'') from {} where id = ?", table));
  if (!stmt || !stmt->bind_int64(1, id)) {
    return std::unexpected(flow_error::query_failed);
  }
  auto stepped = stmt->step();
  if (!stepped) {
    return std::unexpected(flow_error::query_failed);
  }
  if (*stepped == db::step_result::done) {
    return std::unexpected(flow_error::not_found);
  }
  return title_status{.title = stmt->column_text(0), .status = stmt->column_text(1)};
}

/// @brief A plan's slug, falling back to `p<id>` when it has none.
/// @param conn The database connection.
/// @param plan_id The plan.
/// @return The slug, or the failure.
auto fetch_plan_slug(db::connection& conn, std::int64_t plan_id) -> std::expected<std::string, flow_error> {
  auto stmt = conn.prepare("select coalesce(slug,'') from plans where id = ?");
  if (!stmt || !stmt->bind_int64(1, plan_id)) {
    return std::unexpected(flow_error::query_failed);
  }
  auto stepped = stmt->step();
  if (!stepped) {
    return std::unexpected(flow_error::query_failed);
  }
  if (*stepped == db::step_result::done) {
    return std::unexpected(flow_error::not_found);
  }
  auto slug = stmt->column_text(0);
  if (slug.empty()) {
    return std::format("p{}", plan_id);
  }
  return slug;
}

/// @brief A task's workbench sub-directory: its repo slug, else `cross`.
/// @param conn The database connection.
/// @param task_id The task.
/// @return The directory name, or the failure.
auto task_workbench_dir(db::connection& conn, std::int64_t task_id) -> std::expected<std::string, flow_error> {
  auto const fs_form = [](std::string_view slug) {
    std::string out{slug};
    std::ranges::replace(out, '/', '_');
    return out;
  };

  auto scope = conn.prepare("select coalesce(scope_kind,''), scope_id from tasks where id = ?");
  if (!scope || !scope->bind_int64(1, task_id)) {
    return std::unexpected(flow_error::query_failed);
  }
  auto scope_stepped = scope->step();
  if (!scope_stepped) {
    return std::unexpected(flow_error::query_failed);
  }
  if (*scope_stepped == db::step_result::done) {
    return std::unexpected(flow_error::not_found);
  }
  if (scope->column_text(0) == "repo" && !scope->is_null(1)) {
    auto const repo_id = scope->column_int64(1);
    auto       project = conn.prepare("select slug from projects where id = ?");
    if (!project || !project->bind_int64(1, repo_id)) {
      return std::unexpected(flow_error::query_failed);
    }
    auto project_stepped = project->step();
    if (!project_stepped) {
      return std::unexpected(flow_error::query_failed);
    }
    if (*project_stepped == db::step_result::row) {
      return fs_form(project->column_text(0));
    }
    return std::unexpected(flow_error::not_found);
  }

  auto touched = conn.prepare("select p.slug from entity_links el join projects p on p.id = el.to_id "
                              "where el.from_kind = 'task' and el.from_id = ? and el.to_kind = 'repo' "
                              "and el.relationship = 'touches' order by el.id limit 1");
  if (!touched || !touched->bind_int64(1, task_id)) {
    return std::unexpected(flow_error::query_failed);
  }
  auto touched_stepped = touched->step();
  if (!touched_stepped) {
    return std::unexpected(flow_error::query_failed);
  }
  if (*touched_stepped == db::step_result::row) {
    return fs_form(touched->column_text(0));
  }
  return std::string{"cross"};
}

/// @brief The feature-relative path `view` and `edit` render into.
///
/// NOT the same table `engine.workbench.sync::render_entity` uses. The two
/// agree for `question`, `decision` and `scenario` and DISAGREE for
/// `artifact`, which lands under `artifacts/` here and at the feature root
/// there. See the interface header.
/// @param conn The database connection.
/// @param kind The entity kind.
/// @param id The entity id.
/// @param anchor_id The anchor plan.
/// @return The relative path, or the failure.
auto entity_rel_path(db::connection& conn, entity_kind kind, std::int64_t id, std::int64_t anchor_id)
    -> std::expected<std::string, flow_error> {
  switch (kind) {
  case entity_kind::plan: {
    if (id == anchor_id) {
      return std::string{"README.md"};
    }
    auto const slug = fetch_plan_slug(conn, id);
    if (!slug) {
      return std::unexpected(slug.error());
    }
    return std::format("plans/{}.md", *slug);
  }
  case entity_kind::task: {
    auto const row = fetch_title_status(conn, "tasks", id);
    if (!row) {
      return std::unexpected(row.error());
    }
    auto const dir = task_workbench_dir(conn, id);
    if (!dir) {
      return std::unexpected(dir.error());
    }
    return std::format("tasks/{}/{}-{}.md", *dir, id, wb::feature::slugify(row->title));
  }
  case entity_kind::scenario: {
    auto const row = fetch_title_status(conn, "test_scenarios", id);
    if (!row) {
      return std::unexpected(row.error());
    }
    return std::format("scenarios/{}-{}.md", id, wb::feature::slugify(row->title));
  }
  case entity_kind::decision: {
    auto const row = fetch_title_status(conn, "decisions", id);
    if (!row) {
      return std::unexpected(row.error());
    }
    return std::format("decisions/{}-{}.md", id, wb::feature::slugify(row->title));
  }
  case entity_kind::question: {
    auto const row = fetch_title_status(conn, "questions", id);
    if (!row) {
      return std::unexpected(row.error());
    }
    return std::format("questions/{}-{}.md", id, wb::feature::slugify(row->title));
  }
  case entity_kind::artifact: {
    auto stmt = conn.prepare("select coalesce(title,''), coalesce(kind,'') from artifacts where id = ?");
    if (!stmt || !stmt->bind_int64(1, id)) {
      return std::unexpected(flow_error::query_failed);
    }
    auto stepped = stmt->step();
    if (!stepped) {
      return std::unexpected(flow_error::query_failed);
    }
    if (*stepped == db::step_result::done) {
      return std::unexpected(flow_error::not_found);
    }
    // Under `artifacts/`. `sync::render_entity` puts it at the feature
    // ROOT. Both ship; see the interface header.
    return std::format("artifacts/{}", wb::feature::artifact_filename(id, stmt->column_text(0), stmt->column_text(1)));
  }
  }
  return std::unexpected(flow_error::not_found);
}

/// @brief THE THIN RENDERER — title and status only, no timestamps.
///
/// Deliberately not `engine.workbench.sync::render_entity`. The difference
/// between the two is operator-visible on every `view` followed by a
/// `diff`, and is documented in the interface header.
/// @param conn The database connection.
/// @param kind The entity kind.
/// @param id The entity id.
/// @param anchor_id The anchor plan, written into front matter.
/// @return The complete file bytes, or the failure.
auto render_thin(db::connection& conn, entity_kind kind, std::int64_t id, std::int64_t anchor_id)
    -> std::expected<std::string, flow_error> {
  auto const make = [&](std::string_view sql) -> std::expected<db::statement, flow_error> {
    auto stmt = conn.prepare(sql);
    if (!stmt || !stmt->bind_int64(1, id)) {
      return std::unexpected(flow_error::query_failed);
    }
    auto stepped = stmt->step();
    if (!stepped) {
      return std::unexpected(flow_error::query_failed);
    }
    if (*stepped == db::step_result::done) {
      return std::unexpected(flow_error::not_found);
    }
    return std::move(*stmt);
  };

  switch (kind) {
  case entity_kind::plan: {
    auto stmt = make("select coalesce(title,''), coalesce(status,'') from plans where id = ?");
    if (!stmt) {
      return std::unexpected(stmt.error());
    }
    auto const              title = stmt->column_text(0);
    auto const              state = stmt->column_text(1);
    wb::parse::front_matter fm{
        .entity_kind = "plan", .entity_id = id, .anchor_plan_id = anchor_id, .title = title, .status = state};
    return wb::render::render(fm, std::format("# Plan {}: {}\n\n**Status:** {}\n", id, title, state));
  }
  case entity_kind::task: {
    auto stmt = make("select coalesce(title,''), coalesce(status,''), priority from tasks where id = ?");
    if (!stmt) {
      return std::unexpected(stmt.error());
    }
    auto const              title    = stmt->column_text(0);
    auto const              state    = stmt->column_text(1);
    auto const              priority = stmt->column_int64(2);
    wb::parse::front_matter fm{.entity_kind    = "task",
                               .entity_id      = id,
                               .anchor_plan_id = anchor_id,
                               .title          = title,
                               .status         = state,
                               .priority       = priority};
    return wb::render::render(fm,
                              std::format("# Task {}: {}\n\n**Status:** {}  \n**Priority:** {}\n", id, title, state, priority));
  }
  case entity_kind::scenario: {
    auto stmt = make("select coalesce(title,''), coalesce(status,'') from test_scenarios where id = ?");
    if (!stmt) {
      return std::unexpected(stmt.error());
    }
    auto const              title = stmt->column_text(0);
    auto const              state = stmt->column_text(1);
    wb::parse::front_matter fm{
        .entity_kind = "scenario", .entity_id = id, .anchor_plan_id = anchor_id, .title = title, .status = state};
    return wb::render::render(fm, std::format("# Scenario {}: {}\n\n**Status:** {}\n", id, title, state));
  }
  case entity_kind::decision: {
    auto stmt = make("select coalesce(title,''), coalesce(status,''), coalesce(body,''), coalesce(rationale,'') "
                     "from decisions where id = ?");
    if (!stmt) {
      return std::unexpected(stmt.error());
    }
    auto const              title     = stmt->column_text(0);
    auto const              state     = stmt->column_text(1);
    auto const              body      = stmt->column_text(2);
    auto const              rationale = stmt->column_text(3);
    wb::parse::front_matter fm{
        .entity_kind = "decision", .entity_id = id, .anchor_plan_id = anchor_id, .title = title, .status = state};
    return wb::render::render(fm, std::format("# Decision {}: {}\n\n**Status:** {}\n\n## Body\n\n{}\n\n## Rationale\n\n{}\n", id,
                                              title, state, body, rationale));
  }
  case entity_kind::question: {
    auto stmt = make("select coalesce(title,''), coalesce(status,''), coalesce(body,'') from questions where id = ?");
    if (!stmt) {
      return std::unexpected(stmt.error());
    }
    auto const              title = stmt->column_text(0);
    auto const              state = stmt->column_text(1);
    auto const              body  = stmt->column_text(2);
    wb::parse::front_matter fm{
        .entity_kind = "question", .entity_id = id, .anchor_plan_id = anchor_id, .title = title, .status = state};
    return wb::render::render(fm, std::format("# Question {}: {}\n\n**Status:** {}\n\n{}\n", id, title, state, body));
  }
  case entity_kind::artifact: {
    auto stmt = make("select coalesce(title,''), coalesce(status,''), coalesce(kind,''), coalesce(body,'') "
                     "from artifacts where id = ?");
    if (!stmt) {
      return std::unexpected(stmt.error());
    }
    auto const              title     = stmt->column_text(0);
    auto const              state     = stmt->column_text(1);
    auto const              kind_text = stmt->column_text(2);
    auto const              body      = stmt->column_text(3);
    wb::parse::front_matter fm{.entity_kind    = "artifact",
                               .entity_id      = id,
                               .anchor_plan_id = anchor_id,
                               .title          = title,
                               .status         = state,
                               .artifact_kind  = kind_text};
    return wb::render::render(fm, std::format("# Artifact {}: {}\n\n**Kind:** {}  \n**Status:** {}\n\n## Content\n\n{}\n", id,
                                              title, kind_text, state, body));
  }
  }
  return std::unexpected(flow_error::not_found);
}

/// @brief The absolute feature directory for an anchor plan.
/// @param ctx The invocation context, for the workbench root.
/// @param conn The database connection.
/// @param anchor_id The anchor plan.
/// @return The directory, or the failure.
auto feature_dir_for(const context& ctx, db::connection& conn, std::int64_t anchor_id) -> std::expected<std::string, flow_error> {
  auto const anchor = wb::sync::fetch_anchor(conn, anchor_id);
  if (!anchor) {
    return std::unexpected(anchor.error() == wb::sync::sync_error::not_found ? flow_error::not_found : flow_error::query_failed);
  }
  auto const root = wb::root::resolve_root(ctx.env());
  if (!root) {
    return std::unexpected(flow_error::io_failed);
  }
  return wb::sync::feature_dir_for(*root, *anchor);
}

/// @brief Write `content` to `path`, creating parent directories.
/// @param path The absolute file path.
/// @param content The bytes.
/// @return Nothing, or `io_failed`.
auto write_file(const std::filesystem::path& path, std::string_view content) -> std::expected<void, flow_error> {
  std::error_code ec;
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path(), ec);
  }
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) {
    return std::unexpected(flow_error::io_failed);
  }
  file.write(content.data(), static_cast<std::streamsize>(content.size()));
  if (!file) {
    return std::unexpected(flow_error::io_failed);
  }
  return {};
}

/// @brief Read a whole file, or the empty string when it does not exist.
/// @param path The file.
/// @return The bytes.
auto read_file_or_empty(const std::filesystem::path& path) -> std::string {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return {};
  }
  std::ostringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

// ---------------------------------------------------------------------------
// Unified diff
// ---------------------------------------------------------------------------

/// @brief One diff operation.
enum class diff_op_tag : std::uint8_t { equal, remove, insert };

/// @brief One line, tagged.
struct diff_op {
  diff_op_tag      tag = diff_op_tag::equal;
  std::string_view line;
};

/// @brief A run of operations to print together, with context.
struct hunk {
  std::size_t start = 0;
  std::size_t end   = 0;
};

/// @brief Split into lines, reproducing the Zig original's trailing-newline
/// handling exactly.
///
/// A trailing `\n` yields a final EMPTY element. That element is what makes
/// the oracle's hunk line counts come out as they do, so it is not an
/// artifact to clean up — dropping it shifts every `@@` header.
/// @param content The bytes.
/// @return The lines, borrowed from `content`.
auto split_lines(std::string_view content) -> std::vector<std::string_view> {
  std::vector<std::string_view> out;
  std::size_t                   start = 0;
  for (std::size_t i = 0; i < content.size(); ++i) {
    if (content[i] != '\n') {
      continue;
    }
    out.push_back(content.substr(start, i - start));
    start = i + 1;
  }
  if (start < content.size()) {
    out.push_back(content.substr(start));
  } else if (!content.empty() && content.back() == '\n') {
    out.emplace_back(content.data() + content.size(), std::size_t{0});
  }
  return out;
}

/// @brief Build the operation list via a longest-common-subsequence table.
///
/// The same quadratic fill and the same tie-break (`down >= right` prefers
/// a DELETE) as the Zig original. The tie-break is observable: reversing it
/// swaps the order of `-` and `+` lines inside a hunk.
/// @param left The database side.
/// @param right The filesystem side.
/// @return The operations.
auto build_diff_ops(std::span<const std::string_view> left, std::span<const std::string_view> right) -> std::vector<diff_op> {
  auto const n     = left.size();
  auto const m     = right.size();
  auto const width = m + 1;

  std::vector<std::size_t> lcs((n + 1) * (m + 1), 0);
  for (std::size_t i = n; i-- > 0;) {
    for (std::size_t j = m; j-- > 0;) {
      auto const index = (i * width) + j;
      if (left[i] == right[j]) {
        lcs[index] = lcs[((i + 1) * width) + (j + 1)] + 1;
      } else {
        auto const down   = lcs[((i + 1) * width) + j];
        auto const rightv = lcs[(i * width) + (j + 1)];
        lcs[index]        = down >= rightv ? down : rightv;
      }
    }
  }

  std::vector<diff_op> ops;
  std::size_t          i = 0;
  std::size_t          j = 0;
  while (i < n && j < m) {
    if (left[i] == right[j]) {
      ops.push_back({.tag = diff_op_tag::equal, .line = left[i]});
      ++i;
      ++j;
      continue;
    }
    auto const down   = lcs[((i + 1) * width) + j];
    auto const rightv = lcs[(i * width) + (j + 1)];
    if (down >= rightv) {
      ops.push_back({.tag = diff_op_tag::remove, .line = left[i]});
      ++i;
    } else {
      ops.push_back({.tag = diff_op_tag::insert, .line = right[j]});
      ++j;
    }
  }
  for (; i < n; ++i) {
    ops.push_back({.tag = diff_op_tag::remove, .line = left[i]});
  }
  for (; j < m; ++j) {
    ops.push_back({.tag = diff_op_tag::insert, .line = right[j]});
  }
  return ops;
}

/// @brief Group changed operations into hunks with `context` lines around.
/// @param ops The operations.
/// @param context The context radius, always 3 here.
/// @return The hunks.
auto build_hunks(std::span<const diff_op> ops, std::size_t context) -> std::vector<hunk> {
  std::vector<std::size_t> changes;
  for (std::size_t index = 0; index < ops.size(); ++index) {
    if (ops[index].tag != diff_op_tag::equal) {
      changes.push_back(index);
    }
  }
  if (changes.empty()) {
    return {};
  }

  std::vector<hunk>          hunks;
  std::optional<std::size_t> open_start;
  std::size_t                open_end = 0;
  for (auto const change : changes) {
    auto const candidate_start = change > context ? change - context : 0;
    auto const candidate_end   = std::min(ops.size(), change + context + 1);
    if (!open_start.has_value()) {
      open_start = candidate_start;
      open_end   = candidate_end;
      continue;
    }
    if (candidate_start <= open_end) {
      open_end = std::max(open_end, candidate_end);
      continue;
    }
    hunks.push_back({.start = *open_start, .end = open_end});
    open_start = candidate_start;
    open_end   = candidate_end;
  }
  hunks.push_back({.start = *open_start, .end = open_end});
  return hunks;
}

/// @brief Running (old, new) line counts before each operation index.
/// @param ops The operations.
/// @return `ops.size() + 1` pairs.
auto build_pre_counts(std::span<const diff_op> ops) -> std::vector<std::pair<std::size_t, std::size_t>> {
  std::vector<std::pair<std::size_t, std::size_t>> pre(ops.size() + 1, {0, 0});
  std::size_t                                      old_count = 0;
  std::size_t                                      new_count = 0;
  for (std::size_t index = 0; index < ops.size(); ++index) {
    switch (ops[index].tag) {
    case diff_op_tag::equal:
      ++old_count;
      ++new_count;
      break;
    case diff_op_tag::remove:
      ++old_count;
      break;
    case diff_op_tag::insert:
      ++new_count;
      break;
    }
    pre[index + 1] = {old_count, new_count};
  }
  return pre;
}

/// @brief Format one side of an `@@` header.
///
/// A zero count renders as `<start-1>,0`, which is the unified-diff
/// convention for "before this line" and is what the oracle emits.
/// @param start The 1-based start line.
/// @param count The line count.
/// @return The range text.
auto format_hunk_range(std::size_t start, std::size_t count) -> std::string {
  if (count == 0) {
    return std::format("{},0", start == 0 ? 0 : start - 1);
  }
  if (count == 1) {
    return std::format("{}", start);
  }
  return std::format("{},{}", start, count);
}

/// @brief Write the whole unified diff to stdout.
/// @param out The output stream.
/// @param kind The entity kind, for the `---` header.
/// @param id The entity id.
/// @param path The workbench path, for the `+++` header.
/// @param db_content The database side.
/// @param fs_content The filesystem side.
auto write_unified_diff(std::ostream& out, entity_kind kind, std::int64_t id, std::string_view path, std::string_view db_content,
                        std::string_view fs_content) -> void {
  out << std::format("--- db:{}:{}\n", entity_kind_name(kind), id);
  out << std::format("+++ fs:{}\n", path);

  auto const db_lines = split_lines(db_content);
  auto const fs_lines = split_lines(fs_content);
  auto const ops      = build_diff_ops(db_lines, fs_lines);
  auto const hunks    = build_hunks(ops, 3);
  auto const pre      = build_pre_counts(ops);

  for (auto const& piece : hunks) {
    std::size_t old_count = 0;
    std::size_t new_count = 0;
    for (std::size_t index = piece.start; index < piece.end; ++index) {
      switch (ops[index].tag) {
      case diff_op_tag::equal:
        ++old_count;
        ++new_count;
        break;
      case diff_op_tag::remove:
        ++old_count;
        break;
      case diff_op_tag::insert:
        ++new_count;
        break;
      }
    }
    out << std::format("@@ -{} +{} @@\n", format_hunk_range(pre[piece.start].first + 1, old_count),
                       format_hunk_range(pre[piece.start].second + 1, new_count));
    for (std::size_t index = piece.start; index < piece.end; ++index) {
      char const prefix = ops[index].tag == diff_op_tag::equal ? ' ' : (ops[index].tag == diff_op_tag::remove ? '-' : '+');
      out << prefix << ops[index].line << '\n';
    }
  }
}

// ---------------------------------------------------------------------------
// The diff/review snapshot
// ---------------------------------------------------------------------------

/// @brief The database side, the filesystem side, and where they live.
struct snapshot {
  std::string  path;
  std::string  db_content;
  std::string  fs_content;
  std::int64_t anchor_plan_id = 0;
  bool         has_changes    = false;
};

/// @brief Build the snapshot `diff` and `review` both work from.
///
/// USES THE CANONICAL RENDERER AND ITS PATH, not `render_thin` and
/// `entity_rel_path`. That is the whole reason `diff` disagrees with the
/// file `view` just wrote.
/// @param ctx The invocation context.
/// @param conn The database connection.
/// @param kind The entity kind.
/// @param id The entity id.
/// @return The snapshot, or the failure.
auto load_snapshot(const context& ctx, db::connection& conn, entity_kind kind, std::int64_t id)
    -> std::expected<snapshot, flow_error> {
  auto const anchor_id = resolve_anchor(conn, kind, id);
  if (!anchor_id) {
    return std::unexpected(anchor_id.error());
  }
  auto const rendered = wb::sync::render_entity(conn, *anchor_id, entity_kind_name(kind), id);
  if (!rendered) {
    return std::unexpected(rendered.error() == wb::sync::sync_error::not_found ? flow_error::not_found
                                                                               : flow_error::query_failed);
  }
  auto const dir = feature_dir_for(ctx, conn, *anchor_id);
  if (!dir) {
    return std::unexpected(dir.error());
  }
  auto const absolute = (std::filesystem::path{*dir} / rendered->rel_path).string();
  auto       fs       = read_file_or_empty(absolute);
  return snapshot{.path           = absolute,
                  .db_content     = rendered->content,
                  .fs_content     = fs,
                  .anchor_plan_id = *anchor_id,
                  .has_changes    = rendered->content != fs};
}

/// @brief JSON-escape one string for the review envelope.
/// @param value The raw value.
/// @return The escaped value, without surrounding quotes.
auto json_escape(std::string_view value) -> std::string {
  std::string out;
  out.reserve(value.size());
  for (char const c : value) {
    switch (c) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      out += c;
    }
  }
  return out;
}

/// @brief The `review --json` envelope.
///
/// Hand-composed rather than reflected because the oracle emits `verdict`
/// as an explicit `null` in preview mode — a Glaze `std::optional` would
/// omit the key — and because the KEY ORDER is part of the byte contract.
/// @param kind The entity kind.
/// @param id The entity id.
/// @param snap The snapshot.
/// @param verdict The verdict text, or unset for preview mode.
/// @return The complete JSON line, terminator included.
auto review_json(entity_kind kind, std::int64_t id, const snapshot& snap, std::optional<std::string_view> verdict)
    -> std::string {
  return std::format(R"({{"entity":"{}","id":{},"anchor_plan_id":{},"workbench_path":"{}","verdict":{},)"
                     R"("has_changes":{},"persisted":false,"persistence":"none"}})"
                     "\n",
                     entity_kind_name(kind), id, snap.anchor_plan_id, json_escape(snap.path),
                     verdict.has_value() ? std::format("\"{}\"", *verdict) : "null", snap.has_changes ? "true" : "false");
}

/// @brief Whether any front-matter field beyond title and status changed.
///
/// Drives the `[M4 limitation: ...]` warning. Identity fields
/// (`entity_kind`, `entity_id`, `anchor_plan_id`) are deliberately NOT
/// compared: the Zig original treats a change to them as a no-op rather
/// than a warning.
/// @param before The rendered front matter.
/// @param after The saved front matter.
/// @return Whether to warn.
auto other_fields_changed(const wb::parse::front_matter& before, const wb::parse::front_matter& after) -> bool {
  return before.priority != after.priority || before.scope != after.scope || before.artifact_kind != after.artifact_kind ||
         before.touches != after.touches || before.verifies != after.verifies || before.cites != after.cites ||
         before.derives_from != after.derives_from;
}

/// @brief The transition matrix arm for a kind, for the scenario guard.
/// @param kind The entity kind.
/// @return The matching `transition_kind`.
auto transition_kind_for(entity_kind kind) -> pl::transition_kind {
  switch (kind) {
  case entity_kind::plan:
    return pl::transition_kind::plan;
  case entity_kind::task:
    return pl::transition_kind::task;
  case entity_kind::question:
    return pl::transition_kind::question;
  case entity_kind::scenario:
    return pl::transition_kind::scenario;
  case entity_kind::decision:
    return pl::transition_kind::decision;
  case entity_kind::artifact:
    return pl::transition_kind::artifact;
  }
  return pl::transition_kind::plan;
}

/// @brief Apply the title and/or status the operator saved.
///
/// SCENARIO ALONE IS GUARDED by the status matrix, and that asymmetry is
/// the Zig original's, not an oversight here: `engine.planning.scenario`
/// owns the dedicated `verify`/`retire` verbs, and the editflow path is the
/// escape hatch when an operator edits front matter directly — so the guard
/// is applied there and nowhere else. `decision` will move `proposed ->
/// superseded` through here, an edge its own `decision supersede` verb
/// refuses.
///
/// That does NOT make the other three unguarded, and reading it that way
/// costs a round. A status edit passes THREE gates and only the second is
/// scenario-only:
///
///   1. `engine.workbench.parse` validates `status:` against the PER-KIND
///      set before `edit` ever calls this. An unknown status never arrives.
///   2. This matrix check — `scenario` only.
///   3. The TABLE's own CHECK constraints, which no code here consults.
///
/// Gate 3 is the sharp one. `questions` carries
/// `check((status = 'answered' and answer_body is not null and answered_at
/// is not null) or status != 'answered')`, and this function writes title
/// and status ONLY — so a front-matter edit to `answered` is refused by
/// SQLite and surfaces as a bare `QueryFailed` naming neither the
/// constraint nor `question answer`, the verb that works. Oracle-confirmed
/// and reproduced verbatim; improving the message is a behaviour change and
/// wants its own task.
/// @param conn The database connection.
/// @param kind The entity kind.
/// @param id The entity id.
/// @param title The new title, if it changed.
/// @param status The new status, if it changed.
/// @return Nothing, or the failure.
auto apply_mutations(db::connection& conn, entity_kind kind, std::int64_t id, std::optional<std::string> title,
                     std::optional<std::string> status) -> std::expected<void, flow_error> {
  if (!title.has_value() && !status.has_value()) {
    return {};
  }

  if (kind == entity_kind::scenario && status.has_value()) {
    auto const current = fetch_title_status(conn, "test_scenarios", id);
    if (!current) {
      return std::unexpected(current.error());
    }
    if (!pl::check_transition(transition_kind_for(kind), current->status, *status, false)) {
      return std::unexpected(flow_error::illegal_transition);
    }
  }

  std::string sql = std::format("update {} set updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now')", entity_table(kind));
  if (title.has_value()) {
    sql += ", title = ?";
  }
  if (status.has_value()) {
    sql += ", status = ?";
  }
  sql += " where id = ?";

  auto stmt = conn.prepare(sql);
  if (!stmt) {
    return std::unexpected(flow_error::query_failed);
  }
  int index = 1;
  if (title.has_value() && !stmt->bind_text(index++, *title)) {
    return std::unexpected(flow_error::query_failed);
  }
  if (status.has_value() && !stmt->bind_text(index++, *status)) {
    return std::unexpected(flow_error::query_failed);
  }
  if (!stmt->bind_int64(index, id)) {
    return std::unexpected(flow_error::query_failed);
  }
  if (!stmt->step()) {
    return std::unexpected(flow_error::query_failed);
  }
  return {};
}

/// @brief Open `path` in `$PAGER`, then `less`, then `cat`.
///
/// The child INHERITS stdout, so its output is not routed through
/// `ctx.out()`. That is the oracle's behaviour and it is why the tests
/// assert the FILE this flow wrote plus a witness that a pager was spawned,
/// rather than captured stdout.
///
/// Every arm is best-effort: the Zig original swallows a pager failure
/// entirely, including `cat`'s, so a host with none of the three still
/// completes the verb at exit 0 having written the file.
/// @param ctx The invocation context.
/// @param path The file to show.
auto exec_pager(const context& ctx, std::string_view path) -> void {
  auto const try_one = [&](std::string_view program) {
    std::vector<std::string> const argv{std::string{program}, std::string{path}};
    return spawn_inherit(ctx.env(), argv).has_value();
  };
  auto const configured = ctx.env()("PAGER");
  if (configured.has_value() && !configured->empty() && try_one(*configured)) {
    return;
  }
  if (try_one("less")) {
    return;
  }
  static_cast<void>(try_one("cat"));
}

} // namespace

auto entity_kind_name(entity_kind kind) -> std::string_view {
  switch (kind) {
  case entity_kind::plan:
    return "plan";
  case entity_kind::task:
    return "task";
  case entity_kind::question:
    return "question";
  case entity_kind::scenario:
    return "scenario";
  case entity_kind::decision:
    return "decision";
  case entity_kind::artifact:
    return "artifact";
  }
  return "";
}

auto entity_link_kind(entity_kind kind) -> std::string_view {
  return kind == entity_kind::scenario ? std::string_view{"test_scenario"} : entity_kind_name(kind);
}

auto resolve_anchor_plan(db::connection& conn, entity_kind kind, std::int64_t id) -> std::expected<std::int64_t, domain_error> {
  auto const anchor = resolve_anchor(conn, kind, id);
  if (!anchor) {
    return std::unexpected(bare_error(anchor.error()));
  }
  return *anchor;
}

auto view_path(const context& ctx, db::connection& conn, entity_kind kind, std::int64_t id, std::int64_t anchor_id)
    -> std::expected<std::string, domain_error> {
  auto const dir = feature_dir_for(ctx, conn, anchor_id);
  if (!dir) {
    return std::unexpected(bare_error(dir.error()));
  }
  auto const rel = entity_rel_path(conn, kind, id, anchor_id);
  if (!rel) {
    return std::unexpected(bare_error(rel.error()));
  }
  return (std::filesystem::path{*dir} / *rel).string();
}

auto view(context& ctx, db::connection& conn, entity_kind kind, std::int64_t id) -> std::expected<void, domain_error> {
  auto const anchor_id = resolve_anchor(conn, kind, id);
  if (!anchor_id) {
    // The oracle ALSO writes a line naming the entity here, then dies with
    // the raw tag from the unhandled error. Both go to stderr; the prose
    // line first.
    ctx.err() << std::format("error: cannot resolve anchor plan for {} {}: {}\n", entity_kind_name(kind), id,
                             zig_error_name(anchor_id.error()));
    return std::unexpected(bare_error(anchor_id.error()));
  }

  auto const path = view_path(ctx, conn, kind, id, *anchor_id);
  if (!path) {
    return std::unexpected(path.error());
  }
  auto const content = render_thin(conn, kind, id, *anchor_id);
  if (!content) {
    return std::unexpected(bare_error(content.error()));
  }
  if (auto const written = write_file(*path, *content); !written) {
    return std::unexpected(bare_error(written.error()));
  }
  exec_pager(ctx, *path);
  return {};
}

auto edit(context& ctx, db::connection& conn, entity_kind kind, std::int64_t id, const edit_opts& opts)
    -> std::expected<void, domain_error> {
  auto const anchor_id = resolve_anchor(conn, kind, id);
  if (!anchor_id) {
    ctx.err() << std::format("error: cannot resolve anchor plan for {} {}: {}\n", entity_kind_name(kind), id,
                             zig_error_name(anchor_id.error()));
    return std::unexpected(bare_error(anchor_id.error()));
  }

  auto const path = view_path(ctx, conn, kind, id, *anchor_id);
  if (!path) {
    return std::unexpected(path.error());
  }
  auto const content = render_thin(conn, kind, id, *anchor_id);
  if (!content) {
    return std::unexpected(bare_error(content.error()));
  }

  // The database is authoritative: a workbench file that has drifted is
  // OVERWRITTEN before the editor opens, and the operator is told so.
  if (auto const existing = read_file_or_empty(*path); !existing.empty() && existing != *content) {
    ctx.err() << std::format("warning: overwriting local workbench file with DB-rendered content before edit: {}\n", *path);
  }
  if (auto const written = write_file(*path, *content); !written) {
    return std::unexpected(bare_error(written.error()));
  }

  auto const edited = invoke(ctx.env(), *content, invoke_opts{.editor_override = opts.editor_override});
  if (!edited) {
    return std::unexpected(bare_error(flow_error::editor_failed));
  }
  if (edited->exit_code != 0) {
    ctx.err() << std::format("aborted: editor exited with code {}\n", edited->exit_code);
    return {};
  }

  auto const after = wb::parse::parse(edited->content);
  if (!after) {
    ctx.err() << std::format("error: failed to parse editor output for {} {}: {}\n", entity_kind_name(kind), id,
                             wb::parse::error_name(after.error()));
    return std::unexpected(bare_error(flow_error::parse_failed));
  }
  auto const before = wb::parse::parse(*content);
  if (!before) {
    ctx.err() << std::format("error: failed to parse original content: {}\n", wb::parse::error_name(before.error()));
    return std::unexpected(bare_error(flow_error::parse_failed));
  }

  if (other_fields_changed(before->frontmatter, after->frontmatter)) {
    ctx.err() << "[M4 limitation: only title and status mutations are applied; other fields are ignored]\n";
  }

  bool const title_changed  = before->frontmatter.title != after->frontmatter.title;
  bool const status_changed = before->frontmatter.status != after->frontmatter.status;
  if (!title_changed && !status_changed) {
    ctx.err() << "aborted: no changes made\n";
    return {};
  }

  auto const applied = apply_mutations(conn, kind, id, title_changed ? std::optional{after->frontmatter.title} : std::nullopt,
                                       status_changed ? std::optional{after->frontmatter.status} : std::nullopt);
  if (!applied) {
    return std::unexpected(bare_error(applied.error()));
  }

  // The on-disk file is left holding what was SAVED, not what was applied,
  // so a follow-up `diff` shows the dropped fields as pending drift. The
  // Zig original ignores a failure here; so does this.
  static_cast<void>(write_file(*path, edited->content));
  return {};
}

auto diff(context& ctx, db::connection& conn, entity_kind kind, std::int64_t id) -> std::expected<void, domain_error> {
  auto const snap = load_snapshot(ctx, conn, kind, id);
  if (!snap) {
    return std::unexpected(prose_error(snap.error(), kind, id, "diff"));
  }
  if (!snap->has_changes) {
    return {};
  }
  write_unified_diff(ctx.out(), kind, id, snap->path, snap->db_content, snap->fs_content);
  return {};
}

auto review(context& ctx, db::connection& conn, entity_kind kind, std::int64_t id, std::optional<review_verdict> verdict,
            bool json) -> std::expected<void, domain_error> {
  auto const snap = load_snapshot(ctx, conn, kind, id);
  if (!snap) {
    return std::unexpected(prose_error(snap.error(), kind, id, "review"));
  }

  auto const verdict_text = [&]() -> std::optional<std::string_view> {
    if (!verdict.has_value()) {
      return std::nullopt;
    }
    return *verdict == review_verdict::approve ? std::string_view{"approve"} : std::string_view{"request-changes"};
  }();

  if (json) {
    ctx.out() << review_json(kind, id, *snap, verdict_text);
    return {};
  }

  if (!verdict.has_value()) {
    // Preview mode in text form IS `diff`, byte for byte.
    if (snap->has_changes) {
      write_unified_diff(ctx.out(), kind, id, snap->path, snap->db_content, snap->fs_content);
    }
    return {};
  }

  ctx.out() << std::format("{} {} review: {} (persisted: no; reason: no per-entity review table)\n", entity_kind_name(kind), id,
                           *verdict_text);
  ctx.out() << std::format("workbench: {}\nanchor_plan: {}\nchanges_pending: {}\n", snap->path, snap->anchor_plan_id,
                           snap->has_changes ? "yes" : "no");
  return {};
}

} // namespace planar::cmd
