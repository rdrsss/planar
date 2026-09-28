/// @file regenerate.cpp
/// @brief Implementation of `planar.engine.workspace.regenerate` (see
/// regenerate.cppm) — the hand-rolled `{{...}}` template engine plus the
/// leaf's SQL and filesystem glue.
///
/// ## The template engine, and its ONE deliberate quirk
///
/// Behavior-preserving port of the `renderTemplate`/`renderRange`/
/// `execDirective`/`execRangeBlock`/`execIfBlock`/`evalTruthy`/
/// `resolveExpr`/`resolveCommaJoin`/`parseDirective` family in
/// zig/src/cmd/planar/handlers/workspace/regenerate.zig.
///
/// The one thing worth flagging before the mechanics: `parseDirective`
/// trims `" \t\r\n-"` off BOTH ends of a directive's inner text. That means
/// the Go-template-style `-}}` / `{{-` whitespace-trim markers PARSE but do
/// NOT TRIM anything beyond what `{{`/`}}` already delimit — a directive
/// spelled `{{if .Projects -}}` is read identically to `{{if .Projects}}`,
/// and the blank line the author intended to suppress with `-}}` survives
/// in the output. `default_agents_template.md` (embedded below) is written
/// with those markers throughout, and the oracle's own output carries the
/// resulting blank lines — verified against a live capture (this leaf's
/// header). This port reproduces that exactly: `parse_directive` strips the
/// SAME character set, so the same "quirk" (arguably a bug, D2 either way)
/// falls out for free rather than needing a special case.
module;

#include <cctype>

module planar.engine.workspace.regenerate;

import std;
import planar.docs_manifest;
import planar.engine.workspace.routing;
import planar.json_text;

namespace planar::engine::workspace::regenerate {

namespace {

namespace routing  = planar::engine::workspace::routing;
namespace identity = planar::engine::workspace::identity;

// ===========================================================================
// The default AGENTS.md template, embedded verbatim (see
// zig/src/engine/workspace/default_agents_template.md, the oracle's
// `@embedFile`d default).
// ===========================================================================

constexpr unsigned char k_default_template_bytes[] = {
#embed "default_agents_template.md"
};
const std::string_view k_default_template(reinterpret_cast<const char*>(k_default_template_bytes),
                                          sizeof(k_default_template_bytes));

// ===========================================================================
// Data queried for the render, beyond the routing table itself.
// ===========================================================================

struct plan_summary {
  std::int64_t id = 0;
  std::string  title;
};

/// @brief `select id, coalesce(title, '') from <table> where scope_kind =
/// 'association' and scope_id = ? and status = '<status>' order by id`.
///
/// Port of `regenerate.zig`'s comptime-templated `queryPlanSummaries`. The
/// two call sites below are its only instantiations (`plans`/`active` and
/// `questions`/`open`), so this is a plain runtime parameter rather than a
/// second comptime axis.
auto query_plan_summaries(db::connection& conn, std::int64_t org_id, std::string_view table_name, std::string_view status)
    -> std::optional<std::vector<plan_summary>> {
  auto stmt = conn.prepare(std::format("select id, coalesce(title, '') from {} "
                                       "where scope_kind = 'association' and scope_id = ? and status = ? "
                                       "order by id",
                                       table_name));
  if (!stmt.has_value() || !stmt->bind_int64(1, org_id).has_value() || !stmt->bind_text(2, status).has_value()) {
    return std::nullopt;
  }
  std::vector<plan_summary> out;
  while (true) {
    auto step = stmt->step();
    if (!step.has_value()) {
      return std::nullopt;
    }
    if (*step == db::step_result::done) {
      return out;
    }
    out.push_back(plan_summary{.id = stmt->column_int64(0), .title = stmt->column_text(1)});
  }
}

// ===========================================================================
// The render view and the "current loop item" the template engine tracks.
// ===========================================================================

struct agents_view {
  std::string_view                          workspace_name;
  std::string_view                          workspace_slug;
  std::int64_t                              workspace_id = 0;
  std::string_view                          generated_at;
  std::string_view                          source_path;
  std::span<const routing::project_route>   projects;
  std::span<const routing::dependency_edge> dependency_edges;
  std::span<const plan_summary>             active_plans;
  std::span<const plan_summary>             open_questions;
};

using current_item =
    std::variant<std::monostate, const routing::project_route*, const routing::dependency_edge*, const plan_summary*>;

// Which alternative of `current_item` is active is decided by the range
// path that produced it (`.Projects` -> project, `.DependencyEdges` ->
// edge, `.ActivePlans` / `.OpenQuestions` -> plan_summary), exactly as the
// oracle's `CurrentItem` union tag is decided by `iterateAndRender`'s
// branch. `.ActivePlans` and `.OpenQuestions` share the SAME alternative
// (`plan_summary`) because both loops bind `.ID` / `.Title`, matching the
// oracle's `PlanSummary` struct being reused for both.

/// @brief Extract the active `current_item` alternative as `const T*`, or
/// `nullptr` when a different alternative (or `monostate`) is active.
template <typename T> auto item_as(const current_item& item) -> const T* {
  auto slot = std::get_if<const T*>(&item);
  return slot != nullptr ? *slot : nullptr;
}

/// @brief `regenerate.zig`'s own `parseDirective`: trim `" \t\r\n-"` off
/// both ends. See this file's header for why the `-` inclusion is the
/// quirk that makes `-}}` inert.
auto parse_directive(std::string_view raw) -> std::string_view {
  constexpr std::string_view trim_set = " \t\r\n-";
  const auto                 begin    = raw.find_first_not_of(trim_set);
  if (begin == std::string_view::npos) {
    return {};
  }
  const auto end = raw.find_last_not_of(trim_set);
  return raw.substr(begin, end - begin + 1);
}

auto comma_join(std::span<const std::string> values) -> std::string {
  std::string out;
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i > 0) {
      out += ", ";
    }
    out += values[i];
  }
  return out;
}

auto eval_truthy(std::string_view path, const agents_view& view, const current_item& item) -> std::optional<bool> {
  if (path == ".Projects") {
    return !view.projects.empty();
  }
  if (path == ".DependencyEdges") {
    return !view.dependency_edges.empty();
  }
  if (path == ".ActivePlans") {
    return !view.active_plans.empty();
  }
  if (path == ".OpenQuestions") {
    return !view.open_questions.empty();
  }
  if (const auto* p = item_as<routing::project_route>(item)) {
    if (path == ".Slug") {
      return !p->slug.empty();
    }
    if (path == ".RootPath") {
      return !p->root_path.empty();
    }
    if (path == ".Summary") {
      return !p->summary.empty();
    }
  } else if (const auto* e = item_as<routing::dependency_edge>(item)) {
    if (path == ".From") {
      return !e->from.empty();
    }
    if (path == ".To") {
      return !e->to.empty();
    }
    if (path == ".Reason") {
      return !e->reason.empty();
    }
  } else if (const auto* q = item_as<plan_summary>(item)) {
    if (path == ".ID") {
      return q->id != 0;
    }
    if (path == ".Title") {
      return !q->title.empty();
    }
  }
  if (path == ".WorkspaceName") {
    return !view.workspace_name.empty();
  }
  if (path == ".WorkspaceSlug") {
    return !view.workspace_slug.empty();
  }
  if (path == ".WorkspaceID") {
    return view.workspace_id != 0;
  }
  if (path == ".GeneratedAt") {
    return !view.generated_at.empty();
  }
  if (path == ".SourcePath") {
    return !view.source_path.empty();
  }
  return false;
}

auto resolve_expr(std::string_view expr, const agents_view& view, const current_item& item) -> std::optional<std::string> {
  if (expr.starts_with("commaJoin ")) {
    auto arg = expr.substr(std::string_view{"commaJoin "}.size());
    while (!arg.empty() && (arg.front() == ' ' || arg.front() == '\t')) {
      arg.remove_prefix(1);
    }
    while (!arg.empty() && (arg.back() == ' ' || arg.back() == '\t')) {
      arg.remove_suffix(1);
    }
    if (arg != ".Capabilities") {
      return std::nullopt;
    }
    if (const auto* p = item_as<routing::project_route>(item)) {
      return comma_join(p->capabilities);
    }
    return std::nullopt;
  }
  if (expr.empty() || expr.front() != '.') {
    return std::nullopt;
  }

  if (const auto* p = item_as<routing::project_route>(item)) {
    if (expr == ".Slug") {
      return p->slug;
    }
    if (expr == ".RootPath") {
      return p->root_path;
    }
    if (expr == ".Summary") {
      return p->summary;
    }
  } else if (const auto* e = item_as<routing::dependency_edge>(item)) {
    if (expr == ".From") {
      return e->from;
    }
    if (expr == ".To") {
      return e->to;
    }
    if (expr == ".Reason") {
      return e->reason;
    }
  } else if (const auto* q = item_as<plan_summary>(item)) {
    if (expr == ".ID") {
      return std::format("{}", q->id);
    }
    if (expr == ".Title") {
      return q->title;
    }
  }

  if (expr == ".WorkspaceName") {
    return std::string{view.workspace_name};
  }
  if (expr == ".WorkspaceSlug") {
    return std::string{view.workspace_slug};
  }
  if (expr == ".WorkspaceID") {
    return std::format("{}", view.workspace_id);
  }
  if (expr == ".GeneratedAt") {
    return std::string{view.generated_at};
  }
  if (expr == ".SourcePath") {
    return std::string{view.source_path};
  }
  return std::nullopt;
}

/// @brief Find the matching `{{end}}` for a `{{range ...}}` / `{{if ...}}`
/// that just opened at `cursor`, tracking nesting depth exactly as the
/// oracle's `execRangeBlock` / `execIfBlock` do, and (for `if`) also note
/// a same-depth `{{else}}`.
struct block_span {
  std::size_t                final_end_open = 0; ///< Index of the `{{` that opens the MATCHING closing `{{end}}`.
  std::size_t                after_end      = 0; ///< Index just past that closing `{{end}}`.
  std::optional<std::size_t> else_start;         ///< `if` only: index of the `{{` opening a same-depth `{{else}}`.
  std::optional<std::size_t> else_body_start;    ///< `if` only: index just past that `{{else}}`.

  /// @brief The if-TRUE body's exclusive end: the else marker's own start
  /// when present, otherwise the closing `{{end}}`'s start — port of the
  /// oracle's `const if_body_end = else_start orelse open;`.
  [[nodiscard]] auto if_body_end() const -> std::size_t {
    return else_start.value_or(final_end_open);
  }
};

auto find_block_end(std::string_view src, std::size_t cursor, bool track_else) -> std::optional<block_span> {
  std::size_t probe = cursor;
  std::size_t depth = 1;
  block_span  span{};
  while (true) {
    const auto open = src.find("{{", probe);
    if (open == std::string_view::npos) {
      return std::nullopt;
    }
    const auto close = src.find("}}", open + 2);
    if (close == std::string_view::npos) {
      return std::nullopt;
    }
    const auto inner = parse_directive(src.substr(open + 2, close - open - 2));
    if (inner.starts_with("range ") || inner.starts_with("if ")) {
      ++depth;
    } else if (inner == "end") {
      --depth;
      if (depth == 0) {
        span.final_end_open = open;
        span.after_end      = close + 2;
        return span;
      }
    } else if (track_else && depth == 1 && inner == "else") {
      span.else_start      = open;
      span.else_body_start = close + 2;
    }
    probe = close + 2;
  }
}

auto render_range(std::string_view src, const agents_view& view, const current_item& item, std::string& out) -> bool;

auto iterate_and_render(std::string_view body, const agents_view& view, std::string_view path, std::string& out) -> bool {
  if (path == ".Projects") {
    for (const auto& p : view.projects) {
      if (!render_range(body, view, current_item{&p}, out)) {
        return false;
      }
    }
    return true;
  }
  if (path == ".DependencyEdges") {
    for (const auto& e : view.dependency_edges) {
      if (!render_range(body, view, current_item{&e}, out)) {
        return false;
      }
    }
    return true;
  }
  if (path == ".ActivePlans") {
    for (const auto& p : view.active_plans) {
      if (!render_range(body, view, current_item{&p}, out)) {
        return false;
      }
    }
    return true;
  }
  if (path == ".OpenQuestions") {
    for (const auto& q : view.open_questions) {
      if (!render_range(body, view, current_item{&q}, out)) {
        return false;
      }
    }
    return true;
  }
  return false;
}

auto exec_directive(std::string_view src, std::size_t& cursor, const agents_view& view, const current_item& item,
                    std::string& out, std::string_view directive) -> bool {
  if (directive.empty()) {
    return true;
  }
  if (directive == "end" || directive == "else") {
    return false; // Unexpected outside a range/if body — the oracle errors here too.
  }

  if (directive.starts_with("range ")) {
    auto path = directive.substr(std::string_view{"range "}.size());
    while (!path.empty() && (path.front() == ' ' || path.front() == '\t')) {
      path.remove_prefix(1);
    }
    while (!path.empty() && (path.back() == ' ' || path.back() == '\t')) {
      path.remove_suffix(1);
    }
    auto span = find_block_end(src, cursor, /*track_else=*/false);
    if (!span.has_value()) {
      return false;
    }
    if (!iterate_and_render(src.substr(cursor, span->final_end_open - cursor), view, path, out)) {
      return false;
    }
    cursor = span->after_end;
    return true;
  }

  if (directive.starts_with("if ")) {
    auto path = directive.substr(std::string_view{"if "}.size());
    while (!path.empty() && (path.front() == ' ' || path.front() == '\t')) {
      path.remove_prefix(1);
    }
    while (!path.empty() && (path.back() == ' ' || path.back() == '\t')) {
      path.remove_suffix(1);
    }
    auto span = find_block_end(src, cursor, /*track_else=*/true);
    if (!span.has_value()) {
      return false;
    }
    const auto truthy = eval_truthy(path, view, item);
    if (!truthy.has_value()) {
      return false;
    }
    if (*truthy) {
      if (!render_range(src.substr(cursor, span->if_body_end() - cursor), view, item, out)) {
        return false;
      }
    } else if (span->else_start.has_value()) {
      if (!render_range(src.substr(*span->else_body_start, span->final_end_open - *span->else_body_start), view, item, out)) {
        return false;
      }
    }
    cursor = span->after_end;
    return true;
  }

  auto value = resolve_expr(directive, view, item);
  if (!value.has_value()) {
    return false;
  }
  out += *value;
  return true;
}

auto render_range(std::string_view src, const agents_view& view, const current_item& item, std::string& out) -> bool {
  std::size_t cursor = 0;
  while (cursor < src.size()) {
    const auto open = src.find("{{", cursor);
    if (open == std::string_view::npos) {
      out += src.substr(cursor);
      return true;
    }
    out += src.substr(cursor, open - cursor);
    const auto close = src.find("}}", open + 2);
    if (close == std::string_view::npos) {
      return false;
    }
    const auto directive = parse_directive(src.substr(open + 2, close - open - 2));
    cursor               = close + 2;
    if (!exec_directive(src, cursor, view, item, out, directive)) {
      return false;
    }
  }
  return true;
}

auto render_template(std::string_view src, const agents_view& view) -> std::optional<std::string> {
  std::string out;
  if (!render_range(src, view, current_item{}, out)) {
    return std::nullopt;
  }
  return out;
}

auto agents_template_path(const identity::env_lookup& env) -> std::optional<std::filesystem::path> {
  auto home = identity::planar_home(env);
  if (!home.has_value()) {
    return std::nullopt;
  }
  return *home / "templates" / "doc-prompts" / "agents.md";
}

} // namespace

auto regenerate(db::connection& conn, const identity::env_lookup& env, std::int64_t org_id) -> std::expected<result, failure> {
  auto layout = identity::load_layout(env, org_id);
  if (!layout.has_value()) {
    // `loadLayout`'s only failure is `planarHome` returning `error.HomeNotSet`
    // (an unresolvable `$HOME`), which the oracle's handler does NOT special-
    // case — it falls into the generic `else` catch. See this file's header.
    return std::unexpected(failure{.kind = error_kind::generic_failure, .message = "regenerating AGENTS.md failed: HomeNotSet"});
  }

  std::ifstream table_input(layout->routing_table, std::ios::binary);
  if (!table_input) {
    return std::unexpected(
        failure{.kind = error_kind::not_found, .message = "routing table not found; run `planar workspace routing build` first"});
  }
  const std::string raw{std::istreambuf_iterator<char>(table_input), std::istreambuf_iterator<char>()};

  auto table = routing::decode(raw);
  if (!table.has_value()) {
    const auto kind = table.error() == routing::decode_error::invalid ? error_kind::invalid_input : error_kind::generic_failure;
    return std::unexpected(failure{
        .kind = kind, .message = std::format("regenerating AGENTS.md failed: {}", routing::decode_error_name(table.error()))});
  }

  auto plans = query_plan_summaries(conn, org_id, "plans", "active");
  if (!plans.has_value()) {
    return std::unexpected(failure{.kind = error_kind::generic_failure, .message = "regenerating AGENTS.md failed: QueryFailed"});
  }
  auto questions = query_plan_summaries(conn, org_id, "questions", "open");
  if (!questions.has_value()) {
    return std::unexpected(failure{.kind = error_kind::generic_failure, .message = "regenerating AGENTS.md failed: QueryFailed"});
  }

  std::string      custom_template;
  std::string_view body = k_default_template;
  if (auto template_path = agents_template_path(env); template_path.has_value()) {
    std::ifstream custom_input(*template_path, std::ios::binary);
    if (custom_input) {
      custom_template.assign(std::istreambuf_iterator<char>(custom_input), std::istreambuf_iterator<char>());
      body = custom_template;
    }
  }

  // Named so it outlives the `agents_view` below — `agents_view::source_path`
  // is a non-owning `string_view`, and `layout->dir.string()` returns a
  // temporary `std::string` that would otherwise dangle for the duration of
  // `render_template()`.
  const auto source_path = layout->dir.string();

  const agents_view view{
      .workspace_name   = table->workspace_name,
      .workspace_slug   = table->workspace_slug,
      .workspace_id     = table->workspace_id,
      .generated_at     = table->generated_at,
      .source_path      = source_path,
      .projects         = table->projects,
      .dependency_edges = table->cross.dependency_edges,
      .active_plans     = *plans,
      .open_questions   = *questions,
  };

  auto rendered = render_template(body, view);
  if (!rendered.has_value()) {
    return std::unexpected(
        failure{.kind = error_kind::generic_failure, .message = "regenerating AGENTS.md failed: TemplateError"});
  }

  std::error_code ec;
  std::filesystem::create_directories(layout->dir, ec);
  if (ec) {
    return std::unexpected(
        failure{.kind = error_kind::generic_failure, .message = "regenerating AGENTS.md failed: AccessDenied"});
  }

  {
    const auto tmp = layout->agents_md.string() + ".tmp";
    {
      std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
      if (!out) {
        return std::unexpected(
            failure{.kind = error_kind::generic_failure, .message = "regenerating AGENTS.md failed: AccessDenied"});
      }
      out.write(rendered->data(), static_cast<std::streamsize>(rendered->size()));
      if (!out) {
        return std::unexpected(
            failure{.kind = error_kind::generic_failure, .message = "regenerating AGENTS.md failed: AccessDenied"});
      }
    }
    std::filesystem::rename(tmp, layout->agents_md, ec);
    if (ec) {
      return std::unexpected(
          failure{.kind = error_kind::generic_failure, .message = "regenerating AGENTS.md failed: AccessDenied"});
    }
  }

  auto built_manifest = docs_manifest::build(layout->dir);
  if (!built_manifest.has_value()) {
    return std::unexpected(
        failure{.kind = error_kind::generic_failure, .message = "regenerating AGENTS.md failed: ManifestBuildFailed"});
  }
  const auto manifest_path = layout->dir / docs_manifest::file_name;
  if (!docs_manifest::write(manifest_path, *built_manifest)) {
    return std::unexpected(
        failure{.kind = error_kind::generic_failure, .message = "regenerating AGENTS.md failed: AccessDenied"});
  }

  return result{
      .agents_path   = layout->agents_md.string(),
      .manifest_path = manifest_path.string(),
      .project_count = static_cast<std::int64_t>(table->projects.size()),
      .bytes_written = static_cast<std::int64_t>(rendered->size()),
      .manifest_root = built_manifest->root,
  };
}

} // namespace planar::engine::workspace::regenerate
