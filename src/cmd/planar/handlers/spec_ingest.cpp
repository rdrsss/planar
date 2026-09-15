/// @file spec_ingest.cpp
/// @brief Implementation of `planar.cmd.planar.handlers.spec_ingest`. See
/// spec_ingest.cppm for the port's scope and oracle-faithful all-or-nothing
/// `--apply` transaction contract.

module planar.cmd.planar.handlers.spec_ingest;

import std;
import planar.cliapp.args;
import planar.db;
import planar.engine.ingest;
import planar.engine.planning;
import planar.engine.entitylink;
import planar.engine.runtime.session;
import planar.engine.workbench;
import planar.cmd.planar.context;
import planar.cmd.planar.exit;
import planar.cmd.planar.handler;
import planar.cmd.planar.scope;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers {

namespace ingest_ns  = engine::ingest;
namespace diff_ns    = engine::ingest::diff;
namespace parse_ns   = engine::ingest::parse;
namespace cov_ns     = engine::ingest::coverage;
namespace render_ns  = engine::ingest::render;
namespace mat_ns     = engine::ingest::materialize;
namespace pl         = engine::planning;
namespace el         = engine::entitylink;
namespace sess       = engine::runtime::session;
namespace wb_feature = engine::workbench::feature;
namespace wb_parse   = engine::workbench::parse;
namespace wb_root    = engine::workbench::root;

// ===========================================================================
// Error-name helpers (Zig `@errorName` spellings, for oracle-matching text)
// ===========================================================================

/// @brief Returns the Zig-parity spelling for a plan engine error.
/// @param err Engine error to render.
/// @return Oracle-compatible error-name spelling.
auto name_of(pl::plan_error err) -> std::string_view {
  switch (err) {
  case pl::plan_error::not_found:
    return "NotFound";
  case pl::plan_error::slug_conflict:
    return "SlugConflict";
  case pl::plan_error::slug_not_found:
    return "SlugNotFound";
  case pl::plan_error::invalid_parent_cycle:
    return "InvalidParentCycle";
  case pl::plan_error::illegal_transition:
    return "IllegalTransition";
  case pl::plan_error::unknown_status:
    return "UnknownStatus";
  case pl::plan_error::query_failed:
    return "QueryFailed";
  case pl::plan_error::audit_write_failed:
    return "WriteFailed";
  }
  return "Unknown";
}

/// @brief Returns the Zig-parity spelling for a task engine error.
/// @param err Engine error to render.
/// @return Oracle-compatible error-name spelling.
auto name_of(pl::task_error err) -> std::string_view {
  switch (err) {
  case pl::task_error::not_found:
    return "NotFound";
  case pl::task_error::slug_not_found:
    return "SlugNotFound";
  case pl::task_error::slug_conflict:
    return "SlugConflict";
  case pl::task_error::illegal_transition:
    return "IllegalTransition";
  case pl::task_error::unknown_status:
    return "UnknownStatus";
  case pl::task_error::invalid_due_at:
    return "InvalidDueAt";
  case pl::task_error::query_failed:
    return "QueryFailed";
  case pl::task_error::audit_write_failed:
    return "WriteFailed";
  }
  return "Unknown";
}

/// @brief Returns the Zig-parity spelling for a decision engine error.
/// @param err Engine error to render.
/// @return Oracle-compatible error-name spelling.
auto name_of(pl::decision_error err) -> std::string_view {
  switch (err) {
  case pl::decision_error::not_found:
    return "NotFound";
  case pl::decision_error::unsupported_scope:
    return "UnsupportedScope";
  case pl::decision_error::slug_not_found:
    return "SlugNotFound";
  case pl::decision_error::terminal_status:
    return "TerminalStatus";
  case pl::decision_error::invalid_status:
    return "InvalidStatus";
  case pl::decision_error::query_failed:
    return "QueryFailed";
  case pl::decision_error::link_exists:
    return "LinkExists";
  case pl::decision_error::audit_write_failed:
    return "WriteFailed";
  }
  return "Unknown";
}

/// @brief Returns the Zig-parity spelling for a question engine error.
/// @param err Engine error to render.
/// @return Oracle-compatible error-name spelling.
auto name_of(pl::question_error err) -> std::string_view {
  switch (err) {
  case pl::question_error::not_found:
    return "NotFound";
  case pl::question_error::unsupported_scope:
    return "UnsupportedScope";
  case pl::question_error::slug_not_found:
    return "SlugNotFound";
  case pl::question_error::query_failed:
    return "QueryFailed";
  case pl::question_error::answer_required:
    return "AnswerRequired";
  case pl::question_error::illegal_transition:
    return "IllegalTransition";
  case pl::question_error::audit_write_failed:
    return "WriteFailed";
  }
  return "Unknown";
}

/// @brief Returns the Zig-parity spelling for a scenario engine error.
/// @param err Engine error to render.
/// @return Oracle-compatible error-name spelling.
auto name_of(pl::scenario_error err) -> std::string_view {
  switch (err) {
  case pl::scenario_error::not_found:
    return "NotFound";
  case pl::scenario_error::unsupported_scope:
    return "UnsupportedScope";
  case pl::scenario_error::slug_not_found:
    return "SlugNotFound";
  case pl::scenario_error::illegal_transition:
    return "IllegalTransition";
  case pl::scenario_error::unknown_status:
    return "UnknownStatus";
  case pl::scenario_error::query_failed:
    return "QueryFailed";
  case pl::scenario_error::audit_write_failed:
    return "WriteFailed";
  }
  return "Unknown";
}

/// @brief Returns the Zig-parity spelling for an entity-link engine error.
/// @param err Engine error to render.
/// @return Oracle-compatible error-name spelling.
auto name_of(el::entity_link_error err) -> std::string_view {
  switch (err) {
  case el::entity_link_error::not_found:
    return "NotFound";
  case el::entity_link_error::link_exists:
    return "LinkExists";
  case el::entity_link_error::unsupported_scope:
    return "UnsupportedScope";
  case el::entity_link_error::invalid_ref:
    return "InvalidRef";
  case el::entity_link_error::endpoint_not_found:
    return "EndpointNotFound";
  case el::entity_link_error::query_failed:
    return "QueryFailed";
  case el::entity_link_error::audit_write_failed:
    return "WriteFailed";
  }
  return "Unknown";
}

// ===========================================================================
// entity_links helper: idempotent add
// ===========================================================================

/// @brief Adds an `entity_links` row; an existing row is a tolerated no-op.
/// Mirrors zig's `ensureLink`.
/// @param conn Database connection.
/// @param from_kind Source entity kind.
/// @param from_id Source entity id.
/// @param to_kind Target entity kind.
/// @param to_id Target entity id.
/// @param rel Relationship to record.
/// @return Success, or the oracle-compatible persistence error.
auto ensure_link(db::connection& conn, el::entity_kind from_kind, std::int64_t from_id, el::entity_kind to_kind,
                 std::int64_t to_id, el::relationship rel) -> std::expected<void, std::string> {
  auto added =
      el::add(conn, {.from_kind = from_kind, .from_id = from_id, .to_kind = to_kind, .to_id = to_id, .relationship_ = rel});
  if (!added) {
    if (added.error() == el::entity_link_error::link_exists) {
      return {};
    }
    return std::unexpected(std::string{name_of(added.error())});
  }
  return {};
}

// ===========================================================================
// Anchor lookup + workbench artifact helpers
// ===========================================================================

/// @brief Stored identity and scope information for one ingest anchor plan.
struct anchor {
  /// @brief Numeric primary key of the anchor plan.
  std::int64_t id = 0;
  /// @brief Stable, user-facing slug of the anchor plan.
  std::string slug;
  /// @brief Associated project slug used to locate the workbench feature.
  std::string assoc_slug;
  /// @brief Stored entity scope in canonical `project:` or `repo:` form.
  std::string scope_slug;
  /// @brief Current lifecycle status of the anchor plan.
  std::string status;
};

/// @brief Shared projection for resolving an anchor's identity and scope.
constexpr std::string_view k_anchor_sql = "select p.id, p.slug, coalesce(a.slug, ''), p.status, "
                                          " case p.scope_kind "
                                          "   when 'association' then coalesce(a.slug, '') "
                                          "   when 'repo' then 'repo:' || coalesce(pr.slug, '') "
                                          "   else '' "
                                          " end "
                                          "from plans p "
                                          "left join associations a on (p.scope_kind = 'association' and a.id = p.scope_id) "
                                          "left join projects pr on (p.scope_kind = 'repo' and pr.id = p.scope_id) ";

/// @brief Maps the anchor projection's current row into an `anchor` record.
/// @param stmt Stepped query holding the anchor projection.
/// @return Anchor values decoded from the current row.
auto row_to_anchor(db::statement& stmt) -> anchor {
  return anchor{
      .id         = stmt.column_int64(0),
      .slug       = stmt.column_text(1),
      .assoc_slug = stmt.column_text(2),
      .scope_slug = stmt.column_text(4),
      .status     = stmt.column_text(3),
  };
}

/// @brief Resolves an anchor plan from a numeric id or a top-level plan
/// slug. Mirrors zig's `fetchAnchor`/`fetchAnchorById`.
/// @param conn Database connection.
/// @param arg Numeric id or anchor slug.
/// @return Anchor record, or lookup failure text.
auto fetch_anchor(db::connection& conn, std::string_view arg) -> std::expected<anchor, std::string> {
  std::int64_t parsed = 0;
  auto const*  begin  = arg.data();
  auto const*  end    = begin + arg.size();
  bool const   is_number =
      !arg.empty() && std::from_chars(begin, end, parsed).ec == std::errc{} && std::from_chars(begin, end, parsed).ptr == end;

  if (is_number) {
    auto stmt = conn.prepare(std::string(k_anchor_sql) + "where p.id = ? and p.parent_plan_id is null");
    if (!stmt) {
      return std::unexpected(std::string{"QueryFailed"});
    }
    if (!stmt->bind_int64(1, parsed)) {
      return std::unexpected(std::string{"QueryFailed"});
    }
    auto stepped = stmt->step();
    if (!stepped) {
      return std::unexpected(std::string{"QueryFailed"});
    }
    if (*stepped != db::step_result::row) {
      return std::unexpected(std::string{"NotFound"});
    }
    return row_to_anchor(*stmt);
  }

  auto stmt = conn.prepare(std::string(k_anchor_sql) + "where p.parent_plan_id is null and p.slug = ? order by p.id limit 1");
  if (!stmt) {
    return std::unexpected(std::string{"QueryFailed"});
  }
  if (!stmt->bind_text(1, arg)) {
    return std::unexpected(std::string{"QueryFailed"});
  }
  auto stepped = stmt->step();
  if (!stepped) {
    return std::unexpected(std::string{"QueryFailed"});
  }
  if (*stepped != db::step_result::row) {
    return std::unexpected(std::string{"NotFound"});
  }
  return row_to_anchor(*stmt);
}

/// @brief `<external-id>` when a `plan` external_link exists, else `p<id>`.
/// Mirrors zig's `planKey`.
/// @param conn Database connection.
/// @param anchor_id Anchor plan id.
/// @return Workbench feature key for the anchor.
auto plan_key(db::connection& conn, std::int64_t anchor_id) -> std::string {
  auto stmt = conn.prepare("select external_id from external_links where entity_kind = 'plan' and entity_id = ? limit 1");
  if (!stmt || !stmt->bind_int64(1, anchor_id)) {
    return std::format("p{}", anchor_id);
  }
  auto stepped = stmt->step();
  if (!stepped || *stepped != db::step_result::row) {
    return std::format("p{}", anchor_id);
  }
  auto const ext = stmt->column_text(0);
  if (ext.empty()) {
    return std::format("p{}", anchor_id);
  }
  return ext;
}

/// @brief `\n## Content\n` marker in `body`; returns everything after it
/// (leading `\n` trimmed once), or `body` unchanged when the marker is
/// absent. Mirrors zig's `extractContentSection`.
/// @param body Workbench-rendered artifact body.
/// @return Original artifact content without the renderer wrapper.
auto extract_content_section(std::string_view body) -> std::string {
  constexpr std::string_view marker = "\n## Content\n";
  auto const                 pos    = body.find(marker);
  if (pos == std::string_view::npos) {
    return std::string{body};
  }
  auto section = body.substr(pos + marker.size());
  if (!section.empty() && section.front() == '\n') {
    section.remove_prefix(1);
  }
  return std::string{section};
}

/// @brief Reads the lowest-id artifact of `kind` linked to `anchor_id`,
/// strips its front matter, and extracts the `## Content` section. Mirrors
/// zig's `readArtifactBody`.
/// @param conn Database connection.
/// @param feature_dir Workbench feature directory.
/// @param anchor_id Anchor plan id.
/// @param kind Artifact kind to read.
/// @return Extracted artifact body, or lookup/read failure text.
auto read_artifact_body(db::connection& conn, std::string_view feature_dir, std::int64_t anchor_id, std::string_view kind)
    -> std::expected<std::string, std::string> {
  auto stmt = conn.prepare("select a.id, a.title from artifacts a "
                           "join entity_links el on el.from_kind = 'artifact' and el.from_id = a.id "
                           " and el.to_kind = 'plan' and el.to_id = ? and el.relationship = 'derives-from' "
                           "where a.kind = ? order by a.id limit 1");
  if (!stmt || !stmt->bind_int64(1, anchor_id) || !stmt->bind_text(2, kind)) {
    return std::unexpected(std::string{"QueryFailed"});
  }
  auto stepped = stmt->step();
  if (!stepped || *stepped != db::step_result::row) {
    return std::unexpected(std::string{"ArtifactNotFound"});
  }
  auto const id    = stmt->column_int64(0);
  auto const title = stmt->column_text(1);

  auto const filename = wb_feature::artifact_filename(id, title, kind);
  auto const path     = std::filesystem::path(feature_dir) / filename;

  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return std::unexpected(std::string{"ArtifactReadFailed"});
  }
  std::ostringstream buf;
  buf << in.rdbuf();
  auto const raw = buf.str();

  auto parsed = wb_parse::parse(raw);
  if (!parsed) {
    return raw; // Fall back to raw on parse failure, mirroring zig.
  }
  return extract_content_section(parsed->body);
}

// ===========================================================================
// The layer-3 `apply` composition (D20 shape; see spec_ingest.cppm header
// for why this is not in `engine_ingest` and its outer transaction boundary)
// ===========================================================================

/// @brief Counts and state transitions produced by one successful apply.
struct apply_result {
  /// @brief Number of created child plans.
  std::size_t plans_created = 0;
  /// @brief Number of updated child plans.
  std::size_t plans_updated = 0;
  /// @brief Number of created tasks.
  std::size_t tasks_created = 0;
  /// @brief Number of updated tasks.
  std::size_t tasks_updated = 0;
  /// @brief Number of cancelled tasks.
  std::size_t tasks_cancelled = 0;
  /// @brief Number of added decisions.
  std::size_t decisions_added = 0;
  /// @brief Number of added test scenarios.
  std::size_t scenarios_added = 0;
  /// @brief Number of generated placeholders retired after authored coverage landed.
  std::size_t scenarios_retired = 0;
  /// @brief Number of added questions.
  std::size_t questions_added = 0;
  /// @brief Number of answered questions.
  std::size_t questions_answered = 0;
  /// @brief Whether the draft anchor transitioned to active.
  bool anchor_activated = false;
  /// @brief Number of written touch-path facts.
  std::size_t touch_paths_written = 0;
  /// @brief Number of unresolved touch entries.
  std::size_t touches_unresolved = 0;
  /// @brief Number of written dependency edges.
  std::size_t depends_written = 0;
  /// @brief Number of unresolved dependency references.
  std::size_t depends_unresolved = 0;
};

/// @brief What went wrong applying a diff. `message` is either a bare Zig
/// `@errorName` spelling (`"SlugConflict"`, `"QueryFailed"`, ...) or, for
/// `resolve_slug_failed`, already carries the fuller oracle text below.
struct apply_error {
  /// @brief Oracle-compatible failure spelling or detailed resolution text.
  std::string message;
  /// @brief Populated only when a `materialize::reconcile` citation failure
  /// is the cause — the handler renders one line per entry.
  std::vector<mat_ns::citation_diagnostic> citations;
};

/// @brief Looks up a repository's numeric id by its canonical slug.
/// @param conn Database connection.
/// @param slug Repository slug.
/// @return Repository id when found.
auto repo_id_by_slug(db::connection& conn, std::string_view slug) -> std::optional<std::int64_t> {
  auto stmt = conn.prepare("select id from projects where slug = ?");
  if (!stmt || !stmt->bind_text(1, slug)) {
    return std::nullopt;
  }
  auto stepped = stmt->step();
  if (!stepped || *stepped != db::step_result::row) {
    return std::nullopt;
  }
  return stmt->column_int64(0);
}

/// @brief The repo a bare `[touches: <path>]` path should resolve against.
/// Mirrors zig's `soleMemberRepoId`.
/// @param conn Database connection.
/// @param anchor_plan_id Anchor plan id.
/// @return Sole member repository id when unambiguous.
auto sole_member_repo_id(db::connection& conn, std::int64_t anchor_plan_id) -> std::optional<std::int64_t> {
  auto scope_stmt = conn.prepare("select scope_kind, scope_id from plans where id = ?");
  if (!scope_stmt || !scope_stmt->bind_int64(1, anchor_plan_id)) {
    return std::nullopt;
  }
  auto stepped = scope_stmt->step();
  if (!stepped || *stepped != db::step_result::row) {
    return std::nullopt;
  }
  auto const kind = scope_stmt->column_text(0);
  if (scope_stmt->is_null(1)) {
    return std::nullopt;
  }
  auto const scope_id = scope_stmt->column_int64(1);
  if (kind == "repo") {
    return scope_id;
  }
  if (kind != "association") {
    return std::nullopt;
  }

  auto stmt = conn.prepare("select project_id from project_associations where association_id = ? limit 2");
  if (!stmt || !stmt->bind_int64(1, scope_id)) {
    return std::nullopt;
  }
  auto first_step = stmt->step();
  if (!first_step || *first_step != db::step_result::row) {
    return std::nullopt;
  }
  auto const first       = stmt->column_int64(0);
  auto       second_step = stmt->step();
  if (second_step && *second_step == db::step_result::row) {
    return std::nullopt; // Polyrepo association: ambiguous.
  }
  return first;
}

/// @brief Persists one resolved touch-path fact and updates apply counts.
/// @param conn Database connection.
/// @param task_id Task receiving the path fact.
/// @param repo_id Repository owning the path.
/// @param path Repository-relative path.
/// @param res Apply counters to update.
/// @return Success, or persistence failure text.
auto write_touch_path(db::connection& conn, std::int64_t task_id, std::int64_t repo_id, std::string_view path, apply_result& res)
    -> std::expected<void, std::string> {
  auto linked = ensure_link(conn, el::entity_kind::task, task_id, el::entity_kind::repo, repo_id, el::relationship::touches);
  if (!linked) {
    return linked;
  }
  auto written = el::add_touch_path(conn, task_id, repo_id, path);
  if (!written) {
    return std::unexpected(std::string{name_of(written.error())});
  }
  res.touch_paths_written += 1;
  return {};
}

/// @brief Resolves each `[touches: …]` entry and records it. Mirrors zig's
/// `applyTouchesLinks`.
/// @param conn Database connection.
/// @param task_id Task receiving touch links.
/// @param entries Parsed touch entries.
/// @param default_repo_id Repository used for bare paths.
/// @param res Apply counters to update.
/// @return Success, or resolution/persistence failure text.
auto apply_touches_links(db::connection& conn, std::int64_t task_id, std::span<const std::string> entries,
                         std::optional<std::int64_t> default_repo_id, apply_result& res) -> std::expected<void, std::string> {
  for (auto const& entry : entries) {
    if (auto rid = repo_id_by_slug(conn, entry)) {
      auto linked = ensure_link(conn, el::entity_kind::task, task_id, el::entity_kind::repo, *rid, el::relationship::touches);
      if (!linked) {
        return linked;
      }
      continue;
    }

    if (auto const colon = entry.find(':'); colon != std::string::npos) {
      auto const slug = std::string_view(entry).substr(0, colon);
      auto const path = std::string_view(entry).substr(colon + 1);
      if (!slug.empty() && !path.empty()) {
        if (auto rid = repo_id_by_slug(conn, slug)) {
          auto written = write_touch_path(conn, task_id, *rid, path, res);
          if (!written) {
            return written;
          }
          continue;
        }
      }
      res.touches_unresolved += 1;
      continue;
    }

    if (default_repo_id) {
      auto written = write_touch_path(conn, task_id, *default_repo_id, entry, res);
      if (!written) {
        return written;
      }
      continue;
    }

    res.touches_unresolved += 1;
  }
  return {};
}

/// @brief Finds a task slug only within the anchor plan's derived tree.
/// @param conn Database connection.
/// @param slug Task slug to resolve.
/// @param anchor_plan_id Anchor plan id.
/// @return Task id when the slug belongs to the tree.
auto task_id_by_slug_in_tree(db::connection& conn, std::string_view slug, std::int64_t anchor_plan_id)
    -> std::optional<std::int64_t> {
  if (slug.empty()) {
    return std::nullopt;
  }
  auto stmt = conn.prepare("select t.id from tasks t "
                           "where t.slug = ? and ("
                           "  t.plan_id = ? or t.plan_id in ("
                           "    select from_id from entity_links "
                           "     where from_kind = 'plan' and to_kind = 'plan' and to_id = ? "
                           "       and relationship = 'derives-from')) limit 1");
  if (!stmt || !stmt->bind_text(1, slug) || !stmt->bind_int64(2, anchor_plan_id) || !stmt->bind_int64(3, anchor_plan_id)) {
    return std::nullopt;
  }
  auto stepped = stmt->step();
  if (!stepped || *stepped != db::step_result::row) {
    return std::nullopt;
  }
  return stmt->column_int64(0);
}

/// @brief One citation reference resolved to its persisted entity identity.
struct resolved_ref {
  /// @brief Referenced entity kind.
  std::string kind;
  /// @brief Resolved numeric entity id.
  std::int64_t id = 0;
};

/// @brief Maps every `task:<slug>` ref to a numeric task id, scoped to the
/// anchor's plan tree. An unresolvable slug aborts with `ResolveSlugFailed`.
/// Mirrors zig's `resolveSlugRefs`.
/// @param conn Database connection.
/// @param refs Parsed references.
/// @param scenario_title Scenario containing the references.
/// @param anchor_plan_id Anchor plan id.
/// @return Resolved reference list, or resolution failure text.
auto resolve_slug_refs(db::connection& conn, std::span<const parse_ns::task_ref> refs, std::string_view scenario_title,
                       std::int64_t anchor_plan_id) -> std::expected<std::vector<resolved_ref>, std::string> {
  std::vector<resolved_ref> out;
  out.reserve(refs.size());
  for (auto const& ref : refs) {
    if (ref.slug_.empty()) {
      out.push_back({.kind = ref.kind_, .id = ref.id_});
      continue;
    }
    if (auto id = task_id_by_slug_in_tree(conn, ref.slug_, anchor_plan_id)) {
      out.push_back({.kind = ref.kind_, .id = *id});
      continue;
    }
    return std::unexpected(std::string{"ResolveSlugFailed"});
  }
  return out;
}

/// @brief Resolves a roadmap task through its child-plan and task titles.
/// @param conn Database connection.
/// @param anchor_plan_id Anchor plan id.
/// @param child_plan_title Milestone title.
/// @param task_title Roadmap task title.
/// @return Matching active task id when present.
auto resolve_task_from_roadmap_mapping(db::connection& conn, std::int64_t anchor_plan_id, std::string_view child_plan_title,
                                       std::string_view task_title) -> std::optional<std::int64_t> {
  auto stmt = conn.prepare("select t.id from tasks t join plans p on p.id = t.plan_id "
                           "where p.parent_plan_id = ? and lower(trim(p.title)) = lower(trim(?)) "
                           "  and lower(trim(t.title)) = lower(trim(?)) and t.status != 'cancelled' "
                           "order by t.id limit 1");
  if (!stmt || !stmt->bind_int64(1, anchor_plan_id) || !stmt->bind_text(2, child_plan_title) || !stmt->bind_text(3, task_title)) {
    return std::nullopt;
  }
  auto stepped = stmt->step();
  if (!stepped || *stepped != db::step_result::row) {
    return std::nullopt;
  }
  return stmt->column_int64(0);
}

/// @brief Resolves every roadmap citation's task id, records the `cites`
/// edges, and removes stale ones no longer produced. Mirrors zig's
/// `reconcileRoadmapCitations`.
/// @param conn Database connection.
/// @param diff Parsed specification diff.
/// @return Reconciled citations, or apply failure.
auto reconcile_roadmap_citations(db::connection& conn, const diff_ns::diff& diff)
    -> std::expected<std::vector<mat_ns::roadmap_citation>, apply_error> {
  auto artifact_stmt = conn.prepare("select a.id from artifacts a "
                                    "join entity_links el on el.from_kind = 'artifact' and el.from_id = a.id "
                                    " and el.to_kind = 'plan' and el.to_id = ? and el.relationship = 'derives-from' "
                                    "where a.kind = 'roadmap' order by a.id limit 1");
  if (!artifact_stmt || !artifact_stmt->bind_int64(1, diff.anchor_plan_id_)) {
    return std::unexpected(apply_error{.message = "QueryFailed"});
  }
  auto stepped = artifact_stmt->step();
  if (!stepped) {
    return std::unexpected(apply_error{.message = "QueryFailed"});
  }
  if (*stepped != db::step_result::row) {
    return std::vector<mat_ns::roadmap_citation>{};
  }
  auto const artifact_id = artifact_stmt->column_int64(0);

  if (!conn.execute("create temp table if not exists spec_ingest_roadmap_citations (task_id integer primary key)") ||
      !conn.execute("delete from temp.spec_ingest_roadmap_citations")) {
    return std::unexpected(apply_error{.message = "QueryFailed"});
  }

  std::vector<mat_ns::roadmap_citation> citations;
  citations.reserve(diff.roadmap_citations_.size());
  for (auto const& c : diff.roadmap_citations_) {
    std::int64_t task_id = 0;
    if (c.existing_task_id_ > 0) {
      task_id = c.existing_task_id_;
    } else {
      auto resolved = resolve_task_from_roadmap_mapping(conn, diff.anchor_plan_id_, c.child_plan_title_, c.task_title_);
      if (!resolved) {
        return std::unexpected(apply_error{.message = "NotFound"});
      }
      task_id = *resolved;
    }
    citations.push_back({
        .task_id_        = task_id,
        .artifact_id_    = artifact_id,
        .source_locator_ = c.source_locator_,
        .source_text_    = c.source_text_,
    });
    auto ins = conn.prepare("insert into temp.spec_ingest_roadmap_citations (task_id) values (?)");
    if (!ins || !ins->bind_int64(1, task_id) || !ins->step()) {
      return std::unexpected(apply_error{.message = "QueryFailed"});
    }
    auto linked =
        ensure_link(conn, el::entity_kind::task, task_id, el::entity_kind::artifact, artifact_id, el::relationship::cites);
    if (!linked) {
      return std::unexpected(apply_error{.message = linked.error()});
    }
  }

  auto cleanup = conn.prepare("delete from entity_links "
                              "where from_kind = 'task' and to_kind = 'artifact' and to_id = ? "
                              "  and relationship = 'cites' "
                              "  and from_id in (select task_id from routing_task_facts "
                              "    where fact_kind = 'cited_artifact_section' "
                              "      and source_entity_kind = 'artifact' and source_entity_id = ? "
                              "      and (source_locator like 'roadmap#milestone:%' or source_locator like 'roadmap#task:%')) "
                              "  and from_id not in (select task_id from temp.spec_ingest_roadmap_citations)");
  if (!cleanup || !cleanup->bind_int64(1, artifact_id) || !cleanup->bind_int64(2, artifact_id) || !cleanup->step()) {
    return std::unexpected(apply_error{.message = "QueryFailed"});
  }

  return citations;
}

/// @brief Reads a task slug, if the task still has one.
/// @param conn Database connection.
/// @param id Task id.
/// @return Stored slug when present.
auto read_task_slug(db::connection& conn, std::int64_t id) -> std::optional<std::string> {
  auto stmt = conn.prepare("select slug from tasks where id = ?");
  if (!stmt || !stmt->bind_int64(1, id)) {
    return std::nullopt;
  }
  auto stepped = stmt->step();
  if (!stepped || *stepped != db::step_result::row || stmt->is_null(0)) {
    return std::nullopt;
  }
  return stmt->column_text(0);
}

/// @brief Cancels a task and clears its slug so the namespace is free for a
/// replacement. Its caller's outer apply transaction gives it the oracle's
/// all-or-nothing rollback boundary.
/// @param conn Database connection.
/// @param id Task id to retire.
/// @return Success, or retirement failure text.
auto retire_task_for_spec_removal(db::connection& conn, std::int64_t id) -> std::expected<void, std::string> {
  auto cancelled = pl::mark_cancelled(conn, id);
  if (!cancelled) {
    return std::unexpected(std::string{name_of(cancelled.error())});
  }
  auto stmt = conn.prepare("update tasks set slug = null, updated_at = strftime('%Y-%m-%dT%H:%M:%fZ','now') where id = ?");
  if (!stmt || !stmt->bind_int64(1, id) || !stmt->step()) {
    return std::unexpected(std::string{"QueryFailed"});
  }
  return {};
}

/// @brief Renames a plan's slug and force-abandons it, bypassing the
/// operator transition matrix (engine-internal retirement, not an operator
/// move). Mirrors zig's `retirePlanForSpecRemoval`.
/// @param conn Database connection.
/// @param id Plan id to retire.
/// @return Success, or retirement failure text.
auto retire_plan_for_spec_removal(db::connection& conn, std::int64_t id) -> std::expected<void, std::string> {
  auto current = pl::show_plan(conn, id);
  if (!current) {
    return std::unexpected(std::string{name_of(current.error())});
  }
  auto const stale_slug = std::format("stale-{}-{}", id, current->slug);
  auto       renamed    = pl::update_plan(conn, id, {.slug = stale_slug});
  if (!renamed) {
    return std::unexpected(std::string{name_of(renamed.error())});
  }
  auto stmt =
      conn.prepare("update plans set status = 'abandoned', updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') where id = ?");
  if (!stmt || !stmt->bind_int64(1, id) || !stmt->step()) {
    return std::unexpected(std::string{"QueryFailed"});
  }
  return {};
}

/// @brief Creates a `Verify: <task title>` scenario and links it to the
/// task. Mirrors zig's `scenarios.draftScenario`.
/// @param conn Database connection.
/// @param task_id Verified task id.
/// @param task_title Verified task title.
/// @param scope_slug Scope inherited by the scenario.
/// @return Success, or scenario/link failure text.
auto draft_scenario(db::connection& conn, std::int64_t task_id, std::string_view task_title,
                    std::optional<std::string_view> scope_slug) -> std::expected<void, std::string> {
  auto const title = std::format("Verify: {}", task_title);
  auto const body  = std::format("Acceptance scenario auto-drafted by the ingestor.\n\nTask: {}", task_title);
  auto       sc    = pl::create_scenario(
      conn, {.title = title,
             .body  = body,
             .scope = scope_slug.has_value() ? std::optional<std::string>{std::string(*scope_slug)} : std::nullopt});
  if (!sc) {
    return std::unexpected(std::string{name_of(sc.error())});
  }
  auto linked =
      ensure_link(conn, el::entity_kind::test_scenario, sc->id, el::entity_kind::task, task_id, el::relationship::verifies);
  if (!linked) {
    return std::unexpected(linked.error());
  }
  return {};
}

/// @brief Retire only ingestor-owned placeholders that duplicate an authored
/// scenario's coverage. The body marker is the durable ownership boundary:
/// user-authored scenarios are never inferred from a title or removed here.
auto retire_covered_placeholders(db::connection& conn, std::int64_t anchor_plan_id, apply_result& res)
    -> std::expected<void, std::string> {
  auto stmt = conn.prepare(R"(select distinct placeholder.id
from test_scenarios placeholder
join entity_links pv on pv.from_kind = 'test_scenario' and pv.from_id = placeholder.id
  and pv.to_kind = 'task' and pv.relationship = 'verifies'
join tasks t on t.id = pv.to_id
join plans p on p.id = t.plan_id
where p.id in (
  with recursive plan_tree(id) as (
    select id from plans where id = ?
    union all select child.id from plans child join plan_tree parent on child.parent_plan_id = parent.id
  ) select id from plan_tree
)
  and placeholder.status != 'retired'
  and placeholder.body like 'Acceptance scenario auto-drafted by the ingestor.%'
  and exists (
    select 1 from entity_links av
    join test_scenarios authored on authored.id = av.from_id
    where av.from_kind = 'test_scenario' and av.to_kind = 'task'
      and av.relationship = 'verifies' and av.to_id = t.id
      and authored.id != placeholder.id and authored.status != 'retired'
      and authored.body not like 'Acceptance scenario auto-drafted by the ingestor.%'
  ) order by placeholder.id)");
  if (!stmt || !stmt->bind_int64(1, anchor_plan_id)) {
    return std::unexpected(std::string{"QueryFailed"});
  }
  std::vector<std::int64_t> ids;
  while (true) {
    auto stepped = stmt->step();
    if (!stepped)
      return std::unexpected(std::string{"QueryFailed"});
    if (*stepped == db::step_result::done)
      break;
    ids.push_back(stmt->column_int64(0));
  }
  for (const auto id : ids) {
    auto retired = pl::retire_scenario(conn, id, "authored test-spec coverage supersedes generated placeholder");
    if (!retired)
      return std::unexpected(std::string{name_of(retired.error())});
    ++res.scenarios_retired;
  }
  return {};
}

/// @brief Attach the accepted decisions reviewed for an anchor to every
/// descendant task through normal audited entity-link writes.
auto reconcile_accepted_decision_links(db::connection& conn, std::int64_t anchor_plan_id) -> std::expected<void, std::string> {
  auto stmt = conn.prepare(R"(with recursive plan_tree(id) as (
  select id from plans where id=?
  union all select child.id from plans child join plan_tree parent on child.parent_plan_id=parent.id
)
select t.id,d.id from tasks t join plan_tree pt on pt.id=t.plan_id
join entity_links el on el.from_kind='decision' and el.to_kind='plan' and el.to_id=? and el.relationship='derives-from'
join decisions d on d.id=el.from_id and d.status='accepted' order by t.id,d.id)");
  if (!stmt || !stmt->bind_int64(1, anchor_plan_id) || !stmt->bind_int64(2, anchor_plan_id))
    return std::unexpected(std::string{"QueryFailed"});
  while (true) {
    auto stepped = stmt->step();
    if (!stepped)
      return std::unexpected(std::string{"QueryFailed"});
    if (*stepped == db::step_result::done)
      break;
    auto linked = ensure_link(conn, el::entity_kind::task, stmt->column_int64(0), el::entity_kind::decision,
                              stmt->column_int64(1), el::relationship::cites);
    if (!linked)
      return linked;
  }
  return {};
}

/// @brief Records the best-effort ingestor read-session entry for preview.
/// @param conn Database connection.
/// @param anchor_plan_id Previewed anchor plan id.
auto append_read_entry(db::connection& conn, std::int64_t anchor_plan_id) -> void {
  auto const summary = std::format("spec ingest preview plan:{}", anchor_plan_id);
  auto       sid     = sess::ensure_active(conn, "ingestor", std::nullopt);
  if (!sid || *sid == 0) {
    return;
  }
  (void)sess::append_entry(conn, *sid, "read", summary);
}

/// @brief Records the best-effort ingestor action-session entry for apply.
/// @param conn Database connection.
/// @param anchor_plan_id Applied anchor plan id.
auto append_action_entry(db::connection& conn, std::int64_t anchor_plan_id) -> void {
  auto const summary = std::format("spec ingest apply plan:{}", anchor_plan_id);
  auto       sid     = sess::ensure_active(conn, "ingestor", std::nullopt);
  if (!sid || *sid == 0) {
    return;
  }
  (void)sess::append_entry(conn, *sid, "action", summary);
}

/// @brief Flags and anchor provenance supplied to one diff application.
struct apply_options {
  /// @brief Whether to persist the diff instead of rendering a preview.
  bool apply = false;
  /// @brief Whether proposed removals may be applied during persistence.
  bool apply_removals = false;
  /// @brief Anchor entity scope inherited by all derived planning entities.
  std::optional<std::string_view> scope;
};

/// @brief Commits `diff` to the database. Preview (`opts.apply == false`)
/// writes nothing but a best-effort read-session entry. A real apply owns one
/// outer transaction; nested engine CRUD transactions become SAVEPOINTs, so
/// every derived write rolls back together on failure.
/// @param conn Database connection.
/// @param diff Parsed specification diff.
/// @param opts Apply mode and inherited anchor scope.
/// @return Apply counts, or failure diagnostics.
auto apply_diff(db::connection& conn, const diff_ns::diff& diff, const apply_options& opts)
    -> std::expected<apply_result, apply_error> {
  apply_result res;
  if (!opts.apply) {
    append_read_entry(conn, diff.anchor_plan_id_);
    return res;
  }

  auto whole_apply = conn.begin_transaction();
  if (!whole_apply) {
    return std::unexpected(apply_error{.message = "QueryFailed"});
  }

  std::optional<std::string> scope_owned =
      opts.scope.has_value() ? std::optional<std::string>{std::string(*opts.scope)} : std::nullopt;
  std::optional<std::string_view> scope_slug =
      scope_owned.has_value() ? std::optional<std::string_view>{*scope_owned} : std::nullopt;

  auto const                         default_repo_id = sole_member_repo_id(conn, diff.anchor_plan_id_);
  std::set<std::string, std::less<>> authored_coverage;
  for (auto const& scenario : diff.scenarios_) {
    for (auto const& verified : scenario.verifies_) {
      if (verified.kind_ == "task" && !verified.slug_.empty())
        authored_coverage.insert(verified.slug_);
    }
  }

  // ---- removals ---------------------------------------------------------
  if (opts.apply_removals) {
    for (auto const& ot : diff.orphan_tasks_) {
      auto retired = retire_task_for_spec_removal(conn, ot.existing_id_);
      if (!retired) {
        return std::unexpected(apply_error{.message = retired.error()});
      }
      res.tasks_cancelled += 1;
    }
    for (auto const& op : diff.orphan_plans_) {
      for (auto const& ot : op.tasks_) {
        auto retired = retire_task_for_spec_removal(conn, ot.existing_id_);
        if (!retired) {
          return std::unexpected(apply_error{.message = retired.error()});
        }
        res.tasks_cancelled += 1;
      }
      auto retired = retire_plan_for_spec_removal(conn, op.existing_id_);
      if (!retired) {
        return std::unexpected(apply_error{.message = retired.error()});
      }
    }
  }

  // ---- child plans + tasks -----------------------------------------------
  for (auto const& cp : diff.child_plans_) {
    std::int64_t child_plan_id = 0;
    switch (cp.op_) {
    case diff_ns::op::add: {
      auto created = pl::create_plan(
          conn,
          {.title = cp.title_, .status = pl::plan_status::active, .parent_plan_id = diff.anchor_plan_id_, .scope = scope_owned});
      if (!created) {
        return std::unexpected(apply_error{.message = std::string{name_of(created.error())}});
      }
      child_plan_id = created->id;
      auto linked   = ensure_link(conn, el::entity_kind::plan, created->id, el::entity_kind::plan, diff.anchor_plan_id_,
                                  el::relationship::derives_from);
      if (!linked) {
        return std::unexpected(apply_error{.message = linked.error()});
      }
      res.plans_created += 1;
      break;
    }
    case diff_ns::op::update:
      child_plan_id = cp.existing_id_;
      // A reviewed ingest is an execution boundary. Previously-ingested
      // draft milestones must become executable on the same replay path as
      // newly-created milestones.
      {
        auto current = pl::show_plan(conn, child_plan_id);
        if (!current)
          return std::unexpected(apply_error{.message = std::string{name_of(current.error())}});
        if (current->status == pl::plan_status::draft) {
          auto activated = pl::update_plan(conn, child_plan_id, {.status = pl::plan_status::active});
          if (!activated)
            return std::unexpected(apply_error{.message = std::string{name_of(activated.error())}});
        }
      }
      res.plans_updated += 1;
      break;
    case diff_ns::op::remove:
      continue;
    }

    for (auto const& te : cp.tasks_) {
      switch (te.op_) {
      case diff_ns::op::add: {
        auto created = pl::create_task(
            conn, {
                      .title       = te.title_,
                      .body        = te.body_.empty() ? std::nullopt : std::optional<std::string>{te.body_},
                      .plan_id     = child_plan_id,
                      .next_action = te.next_action_.empty() ? std::optional<std::string>{"Implement per acceptance criteria."}
                                                             : std::optional<std::string>{te.next_action_},
                      .slug        = te.slug_.empty() ? std::nullopt : std::optional<std::string>{te.slug_},
                      .scope       = scope_owned,
                  });
        if (!created) {
          return std::unexpected(apply_error{.message = std::string{name_of(created.error())}});
        }
        auto linked = ensure_link(conn, el::entity_kind::task, created->id, el::entity_kind::plan, child_plan_id,
                                  el::relationship::derives_from);
        if (!linked) {
          return std::unexpected(apply_error{.message = linked.error()});
        }
        auto touched = apply_touches_links(conn, created->id, te.touches_, default_repo_id, res);
        if (!touched) {
          return std::unexpected(apply_error{.message = touched.error()});
        }
        if (diff_ns::is_non_trivial(te.body_) && !authored_coverage.contains(te.slug_)) {
          auto drafted = draft_scenario(conn, created->id, te.title_, scope_slug);
          if (!drafted) {
            return std::unexpected(apply_error{.message = drafted.error()});
          }
          res.scenarios_added += 1;
        }
        res.tasks_created += 1;
        break;
      }
      case diff_ns::op::update: {
        if (!te.body_.empty()) {
          auto stmt = conn.prepare("update tasks set body = ?, updated_at = strftime('%Y-%m-%dT%H:%M:%fZ','now') where id = ?");
          if (!stmt || !stmt->bind_text(1, te.body_) || !stmt->bind_int64(2, te.existing_id_) || !stmt->step()) {
            return std::unexpected(apply_error{.message = "QueryFailed"});
          }
        }
        if (!te.next_action_.empty()) {
          auto stmt =
              conn.prepare("update tasks set next_action = ?, updated_at = strftime('%Y-%m-%dT%H:%M:%fZ','now') where id = ?");
          if (!stmt || !stmt->bind_text(1, te.next_action_) || !stmt->bind_int64(2, te.existing_id_) || !stmt->step()) {
            return std::unexpected(apply_error{.message = "QueryFailed"});
          }
        }

        auto touched = apply_touches_links(conn, te.existing_id_, te.touches_, default_repo_id, res);
        if (!touched) {
          return std::unexpected(apply_error{.message = touched.error()});
        }

        if (!te.slug_.empty()) {
          auto const current  = read_task_slug(conn, te.existing_id_);
          bool const set_slug = !current.has_value() || current->empty();
          if (set_slug) {
            auto updated = pl::update_task(conn, te.existing_id_, {.slug = te.slug_});
            if (!updated) {
              return std::unexpected(apply_error{.message = std::string{name_of(updated.error())}});
            }
          }
          // A mismatch is logged, not refused, matching zig (which keeps
          // the DB value and emits a `std.log.warn`). Left silent here:
          // this port has no equivalent structured-log sink at this layer.
        }

        res.tasks_updated += 1;
        break;
      }
      case diff_ns::op::remove:
        continue;
      }
    }
  }

  auto citations = reconcile_roadmap_citations(conn, diff);
  if (!citations) {
    return std::unexpected(citations.error());
  }

  // ---- decisions ----------------------------------------------------------
  for (auto const& de : diff.decisions_) {
    switch (de.op_) {
    case diff_ns::op::add: {
      auto const body    = de.body_.empty() ? std::string{"(no body)"} : de.body_;
      auto       created = pl::create_decision(conn, {.title = de.title_, .body = body, .scope = scope_owned});
      if (!created) {
        return std::unexpected(apply_error{.message = std::string{name_of(created.error())}});
      }
      auto linked = ensure_link(conn, el::entity_kind::decision, created->id, el::entity_kind::plan, diff.anchor_plan_id_,
                                el::relationship::derives_from);
      if (!linked) {
        return std::unexpected(apply_error{.message = linked.error()});
      }
      res.decisions_added += 1;
      break;
    }
    case diff_ns::op::update: {
      auto stmt = conn.prepare("update decisions set body = ?, updated_at = strftime('%Y-%m-%dT%H:%M:%fZ','now') where id = ?");
      if (!stmt || !stmt->bind_text(1, de.body_) || !stmt->bind_int64(2, de.existing_id_) || !stmt->step()) {
        return std::unexpected(apply_error{.message = "QueryFailed"});
      }
      break;
    }
    case diff_ns::op::remove:
      continue;
    }
  }

  // ---- dependency edges -----------------------------------------------------
  for (auto const& cp : diff.child_plans_) {
    for (auto const& te : cp.tasks_) {
      if (te.depends_.empty()) {
        continue;
      }
      std::int64_t task_id = 0;
      if (te.op_ == diff_ns::op::add) {
        auto id = task_id_by_slug_in_tree(conn, te.slug_, diff.anchor_plan_id_);
        if (!id) {
          res.depends_unresolved += te.depends_.size();
          continue;
        }
        task_id = *id;
      } else if (te.op_ == diff_ns::op::update) {
        task_id = te.existing_id_;
      } else {
        continue;
      }
      for (auto const& dep_slug : te.depends_) {
        auto blocker = task_id_by_slug_in_tree(conn, dep_slug, diff.anchor_plan_id_);
        if (!blocker) {
          res.depends_unresolved += 1;
          continue;
        }
        if (*blocker == task_id) {
          continue;
        }
        auto linked =
            ensure_link(conn, el::entity_kind::task, task_id, el::entity_kind::task, *blocker, el::relationship::depends_on);
        if (!linked) {
          return std::unexpected(apply_error{.message = linked.error()});
        }
        res.depends_written += 1;
      }
    }
  }

  // ---- test-spec scenarios ---------------------------------------------------
  for (auto const& se : diff.scenarios_) {
    auto resolved = resolve_slug_refs(conn, se.verifies_, se.title_, diff.anchor_plan_id_);
    if (!resolved) {
      return std::unexpected(apply_error{.message = resolved.error()});
    }
    switch (se.op_) {
    case diff_ns::op::add: {
      auto created = pl::create_scenario(conn, {.title = se.title_,
                                                .body  = se.body_.empty() ? std::nullopt : std::optional<std::string>{se.body_},
                                                .scope = scope_owned});
      if (!created) {
        return std::unexpected(apply_error{.message = std::string{name_of(created.error())}});
      }
      auto linked = ensure_link(conn, el::entity_kind::test_scenario, created->id, el::entity_kind::plan, diff.anchor_plan_id_,
                                el::relationship::derives_from);
      if (!linked) {
        return std::unexpected(apply_error{.message = linked.error()});
      }
      for (auto const& r : *resolved) {
        auto v = ensure_link(conn, el::entity_kind::test_scenario, created->id, el::entity_kind::task, r.id,
                             el::relationship::verifies);
        if (!v) {
          return std::unexpected(apply_error{.message = v.error()});
        }
      }
      res.scenarios_added += 1;
      break;
    }
    case diff_ns::op::update: {
      auto stmt =
          conn.prepare("update test_scenarios set body = ?, updated_at = strftime('%Y-%m-%dT%H:%M:%fZ','now') where id = ?");
      if (!stmt || !stmt->bind_text(1, se.body_) || !stmt->bind_int64(2, se.existing_id_) || !stmt->step()) {
        return std::unexpected(apply_error{.message = "QueryFailed"});
      }
      for (auto const& r : *resolved) {
        auto v = ensure_link(conn, el::entity_kind::test_scenario, se.existing_id_, el::entity_kind::task, r.id,
                             el::relationship::verifies);
        if (!v) {
          return std::unexpected(apply_error{.message = v.error()});
        }
      }
      break;
    }
    case diff_ns::op::remove:
      continue;
    }
  }

  if (auto retired = retire_covered_placeholders(conn, diff.anchor_plan_id_, res); !retired) {
    return std::unexpected(apply_error{.message = retired.error()});
  }

  // ---- new questions ------------------------------------------------------
  for (auto const& q : diff.new_questions_) {
    auto created = pl::create_question(conn, {.title   = q.title_,
                                              .body    = q.body_.empty() ? std::nullopt : std::optional<std::string>{q.body_},
                                              .scope   = scope_owned,
                                              .plan_id = diff.anchor_plan_id_});
    if (!created) {
      return std::unexpected(apply_error{.message = std::string{name_of(created.error())}});
    }
    if (!q.resolution_.empty()) {
      auto answered = pl::answer_question(conn, created->id, q.resolution_);
      if (!answered) {
        return std::unexpected(apply_error{.message = std::string{name_of(answered.error())}});
      }
      res.questions_answered += 1;
    }
    res.questions_added += 1;
  }

  // ---- question status flips -----------------------------------------------
  for (auto const& sc : diff.updated_question_status_) {
    auto answered = pl::answer_question(conn, sc.question_id_, sc.answer_);
    if (!answered) {
      return std::unexpected(apply_error{.message = std::string{name_of(answered.error())}});
    }
    res.questions_answered += 1;
  }

  if (auto linked = reconcile_accepted_decision_links(conn, diff.anchor_plan_id_); !linked) {
    return std::unexpected(apply_error{.message = linked.error()});
  }

  auto reconciled = mat_ns::reconcile(conn, diff.anchor_plan_id_, *citations);
  if (!reconciled) {
    if (reconciled.error().kind_ == mat_ns::materialize_error_kind::invalid_citation) {
      return std::unexpected(apply_error{.message = "InvalidCitation", .citations = reconciled.error().citations_});
    }
    return std::unexpected(apply_error{.message = "QueryFailed"});
  }

  // ---- flip anchor draft -> active -------------------------------------------
  if (diff.current_status_ == "draft") {
    auto updated = pl::update_plan(conn, diff.anchor_plan_id_, {.status = pl::plan_status::active});
    if (!updated) {
      return std::unexpected(apply_error{.message = std::string{name_of(updated.error())}});
    }
    res.anchor_activated = true;
  }

  append_action_entry(conn, diff.anchor_plan_id_);
  if (auto committed = whole_apply->commit(); !committed) {
    return std::unexpected(apply_error{.message = "QueryFailed"});
  }
  return res;
}

// ===========================================================================
// The handler
// ===========================================================================

/// @brief One plan argument, end to end: anchor lookup, cross-scope guard,
/// artifact read, parse, diff, render, strict gate, apply. Mirrors zig's
/// `runOnePlan`.
/// @param ctx Invocation context.
/// @param conn Database connection.
/// @param plan_arg Anchor id or slug.
/// @param apply_flag Whether to persist.
/// @param apply_removals Whether removal operations are enabled.
/// @param scope_flag Explicit operator write scope.
/// @param strict Whether coverage/collision checks refuse.
/// @param json_out Whether to render JSON.
/// @param multi Whether this is a batch invocation.
/// @param json_bodies Accumulator for batch JSON results.
/// @return Success, or handler failure.
auto run_one_plan(context& ctx, db::connection& conn, std::string_view plan_arg, bool apply_flag, bool apply_removals,
                  std::optional<std::string_view> scope_flag, bool strict, bool json_out, bool multi,
                  std::vector<std::string>& json_bodies) -> std::expected<void, domain_error> {
  auto found = fetch_anchor(conn, plan_arg);
  if (!found) {
    ctx.err() << std::format("plan '{}' not found: {}\n", plan_arg, found.error());
    return std::unexpected(error_from_body(domain_error_kind::not_found, std::format("plan '{}' not found", plan_arg)));
  }
  auto const& anc = *found;

  std::optional<std::string_view> entity_scope;
  if (!anc.scope_slug.empty()) {
    entity_scope = anc.scope_slug;
  }

  if (apply_flag) {
    auto resolution = resolve_write_scope(ctx, scope_flag, "spec ingest");
    if (!resolution) {
      return std::unexpected(resolution.error());
    }
    std::optional<std::string_view> write_view;
    if (resolution->scope.has_value()) {
      write_view = *resolution->scope;
    }
    if (!guard_with_membership(conn, entity_scope, write_view)) {
      ctx.err() << std::format("plan {} belongs to {} but the resolved write scope is {}. "
                               "Refusing cross-scope write; pass --scope {} or cd into the right repo.\n",
                               anc.id, entity_scope.value_or("global"), write_view.value_or("global"),
                               entity_scope.value_or("global"));
      return std::unexpected(error_from_body(domain_error_kind::scope_mismatch, "spec ingest: cross-scope write refused"));
    }
  }

  auto root = wb_root::resolve_root(ctx.env());
  if (!root) {
    ctx.err() << "resolving workbench root: Unresolved\n";
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "resolving workbench root"));
  }

  auto const key         = plan_key(conn, anc.id);
  auto const feature_dir = wb_feature::feature_dir(*root, anc.assoc_slug, key, anc.slug);

  auto tech_body = read_artifact_body(conn, feature_dir, anc.id, "tech_spec");
  if (!tech_body) {
    ctx.err() << std::format("plan {} ({}): reading tech_spec artifact: {}\n", anc.id, anc.slug, tech_body.error());
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "reading tech_spec artifact"));
  }
  auto roadmap_body = read_artifact_body(conn, feature_dir, anc.id, "roadmap");
  if (!roadmap_body) {
    ctx.err() << std::format("plan {} ({}): reading roadmap artifact: {}\n", anc.id, anc.slug, roadmap_body.error());
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "reading roadmap artifact"));
  }
  auto test_body = read_artifact_body(conn, feature_dir, anc.id, "test_spec");

  auto const                      decisions  = parse_ns::parse_tech_spec_decisions(*tech_body);
  auto const                      questions  = parse_ns::parse_tech_spec_open_questions(*tech_body);
  auto const                      milestones = parse_ns::parse_roadmap(*roadmap_body);
  std::vector<parse_ns::scenario> scenarios;
  if (test_body) {
    scenarios = parse_ns::parse_test_spec(*test_body);
  }

  if (decisions.empty() && parse_ns::section_has_content(*tech_body, "## Decisions")) {
    ctx.err() << "warning: '## Decisions' has content but 0 decisions extracted — "
                 "decisions need '### <title>' H3 headings or '- **Title.** body' bullets. "
                 "Nothing was written.\n";
  }
  if (questions.empty() && parse_ns::section_has_content(*tech_body, "## Open Questions")) {
    ctx.err() << "warning: '## Open Questions' has content but 0 questions extracted — "
                 "questions need '### <title>' H3 headings. Nothing was written.\n";
  }
  if (test_body.has_value() && scenarios.empty() && parse_ns::section_has_content(*test_body, "## Scenarios")) {
    ctx.err() << "warning: '## Scenarios' has content but 0 scenarios extracted — "
                 "scenarios need '### Scenario: <title>' H3 headings, '### <title>' H3 with '**Verifies:**', "
                 "or '#### Scenario: <title>' H4 items under a bucket H3. Nothing was written.\n";
  }

  auto diff = diff_ns::compute(conn, anc.id, milestones, decisions, questions, scenarios);
  if (!diff) {
    ctx.err() << std::format("plan {} ({}): computing diff: {}\n", anc.id, anc.slug,
                             diff.error() == diff_ns::diff_error::not_found ? "NotFound" : "QueryFailed");
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "computing diff"));
  }

  if (json_out) {
    auto body = render_ns::render_json(*diff);
    if (multi) {
      json_bodies.push_back(std::move(body));
    } else {
      ctx.out() << body;
    }
  } else {
    if (multi) {
      ctx.out() << std::format("=== plan {}: {} ===\n", anc.id, anc.slug);
    }
    ctx.out() << render_ns::render_text(*diff, apply_flag);
  }

  for (auto const& sc : diff->slug_collisions_) {
    ctx.err() << std::format("warning: task slug '{}' already exists (task {} on plan {}) — apply will fail with SlugConflict. "
                             "Rename it to a unique (feature-prefixed) slug.\n",
                             sc.slug_, sc.existing_task_id_, sc.existing_plan_id_);
  }

  if (strict) {
    auto const cov                 = cov_ns::compute(*diff);
    bool const has_coverage_gaps   = cov.has_gaps();
    bool const has_slug_collisions = !diff->slug_collisions_.empty();
    if (has_coverage_gaps || has_slug_collisions) {
      ctx.err() << std::format("plan {}: --strict refused: ", anc.id);
      bool printed = false;
      if (!cov.uncovered_task_slugs_.empty()) {
        ctx.err() << std::format("{} uncovered task slug(s): ", cov.uncovered_task_slugs_.size());
        for (std::size_t i = 0; i < cov.uncovered_task_slugs_.size(); ++i) {
          if (i > 0) {
            ctx.err() << ", ";
          }
          ctx.err() << cov.uncovered_task_slugs_[i];
        }
        printed = true;
      }
      if (!cov.orphan_scenarios_.empty()) {
        if (printed) {
          ctx.err() << "; ";
        }
        ctx.err() << std::format("{} orphan scenario(s): ", cov.orphan_scenarios_.size());
        for (std::size_t i = 0; i < cov.orphan_scenarios_.size(); ++i) {
          if (i > 0) {
            ctx.err() << "; ";
          }
          ctx.err() << cov.orphan_scenarios_[i];
        }
        printed = true;
      }
      if (has_slug_collisions) {
        if (printed) {
          ctx.err() << "; ";
        }
        ctx.err() << std::format("{} global slug collision(s): ", diff->slug_collisions_.size());
        for (std::size_t i = 0; i < diff->slug_collisions_.size(); ++i) {
          if (i > 0) {
            ctx.err() << ", ";
          }
          ctx.err() << diff->slug_collisions_[i].slug_;
        }
      }
      ctx.err() << "\n";
      return std::unexpected(error_from_body(domain_error_kind::invalid_input, "--strict refused"));
    }
  }

  apply_options const opts{
      .apply          = apply_flag,
      .apply_removals = apply_removals,
      // The resolved operator scope authorizes the write above; provenance
      // remains the anchor's stored scope, exactly as the Zig apply options
      // do. An association may authorize a repository anchor through
      // membership without widening every derived descendant to association
      // scope.
      .scope = entity_scope,
  };
  auto result = apply_diff(conn, *diff, opts);
  if (!result) {
    ctx.err() << std::format("plan {} ({}): apply failed: {}\n", anc.id, anc.slug, result.error().message);
    if (!result.error().citations.empty()) {
      for (auto const& cd : result.error().citations) {
        ctx.err() << std::format("  task {} cites artifact {} section \"{}\", which that artifact does not contain\n",
                                 cd.task_id_, cd.artifact_id_, cd.wanted_);
        if (!cd.available_.empty()) {
          std::string joined;
          for (std::size_t i = 0; i < cd.available_.size(); ++i) {
            if (i > 0) {
              joined += ", ";
            }
            joined += cd.available_[i];
          }
          ctx.err() << std::format("    artifact {} has: {}\n", cd.artifact_id_, joined);
        } else {
          ctx.err() << std::format("    artifact {} has no `## ` sections to cite\n", cd.artifact_id_);
        }
      }
      ctx.err() << "  a citation runs to end-of-line unless stopped by `,`, `)` or `]` — "
                   "write [artifact:<id>#Section] when prose follows on the same line\n";
      ctx.err() << "  no facts were materialized for any task under this anchor\n";
    }
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "apply failed"));
  }

  if (apply_flag && !json_out) {
    ctx.err() << std::format("plan {} ({}) applied: {} plans created, {} tasks created, {} tasks updated, {} decisions added, "
                             "{} questions added, {} questions answered",
                             anc.id, anc.slug, result->plans_created, result->tasks_created, result->tasks_updated,
                             result->decisions_added, result->questions_added, result->questions_answered);
    if (result->tasks_cancelled > 0) {
      ctx.err() << std::format(", {} tasks cancelled", result->tasks_cancelled);
    }
    if (result->scenarios_retired > 0) {
      ctx.err() << std::format(", {} generated scenarios retired", result->scenarios_retired);
    }
    if (result->touch_paths_written > 0) {
      ctx.err() << std::format(", {} path touches declared", result->touch_paths_written);
    }
    if (result->depends_written > 0) {
      ctx.err() << std::format(", {} dependency edges", result->depends_written);
    }
    if (result->anchor_activated) {
      ctx.err() << " (anchor plan activated)";
    }
    ctx.err() << "\n";

    if (result->depends_unresolved > 0) {
      ctx.err() << std::format("warning: {} `[depends: …]` slug(s) named no task under this anchor; "
                               "the ordering they describe was NOT recorded\n",
                               result->depends_unresolved);
    }
    if (result->touches_unresolved > 0) {
      ctx.err() << std::format("warning: {} `[touches: …]` entr{} resolved to neither a registered repo "
                               "slug nor a usable path and {} recorded\n"
                               "         qualify a path as `<repo-slug>:<path>`; a bare path needs the "
                               "anchor's association to have exactly one member repo\n",
                               result->touches_unresolved, result->touches_unresolved == 1 ? "y" : "ies",
                               result->touches_unresolved == 1 ? "was not" : "were not");
    }
  }

  return {};
}

auto spec_ingest(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto const plan_arg = cliapp::positional_string(args, "plan").value_or(std::string{});
  auto const extra    = cliapp::positional_strings(args, "extra-plans");

  bool const apply_removals = cliapp::flag_bool(args, "--apply-removals");
  bool const apply_flag     = cliapp::flag_bool(args, "--apply");
  if (apply_removals && !apply_flag) {
    return std::unexpected(error_from_body(domain_error_kind::invalid_input,
                                           "--apply-removals requires --apply; use both flags together to commit removals"));
  }

  auto const                      format     = cliapp::flag_string(args, "--format").value_or("text");
  bool const                      json_out   = cliapp::flag_bool(args, "--json") || format == "json";
  bool const                      strict     = cliapp::flag_bool(args, "--strict");
  auto const                      scope_flag = cliapp::flag_string(args, "--scope");
  std::optional<std::string_view> scope_view;
  if (scope_flag.has_value()) {
    scope_view = *scope_flag;
  }

  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  bool const multi = !extra.empty();

  std::vector<std::string> json_bodies;
  bool                     any_err = false;

  std::vector<std::string> plans;
  plans.push_back(plan_arg);
  for (auto const& p : extra) {
    plans.push_back(p);
  }

  for (auto const& p : plans) {
    auto result = run_one_plan(ctx, **conn, p, apply_flag, apply_removals, scope_view, strict, json_out, multi, json_bodies);
    if (!result) {
      any_err = true;
    }
  }

  if (json_out && multi) {
    ctx.out() << "[";
    for (std::size_t i = 0; i < json_bodies.size(); ++i) {
      if (i > 0) {
        ctx.out() << ",";
      }
      ctx.out() << json_bodies[i];
    }
    ctx.out() << "]\n";
  }

  if (any_err) {
    return std::unexpected(error_from_body(domain_error_kind::invalid_input, "one or more plans failed to ingest"));
  }
  return {};
}

/// @brief Declare the `spec` group and its `ingest` leaf.
///
/// MOVED HERE FROM `tree.cpp` at M11.3f, and the `extra-plans` line
/// is moved BYTE-FOR-BYTE rather than expressed through a `declare`
/// primitive, because it is the one declaration in this binary that no
/// gate and no test can see.
///
/// `spec ingest <p1> <p2> <p3>` batches every argument (the oracle's
/// `rest_field = "extra_plans"`), so `plan` is REQUIRED and a second,
/// HIDDEN positional takes `expected(0, -1)` to catch the rest.
/// `->group("")` is what hides it, and `planar.cliapp.walk` treats an
/// empty group as hidden and PRUNES THE SUBTREE — so the option appears
/// neither in the `schema` catalog nor on any help page, which are the
/// only three things `scripts/surface-snapshot.sh` hashes. Nothing in
/// the test suite mentions `extra-plans` either. Verify it by hand
/// (`planar spec ingest <plan> <extra>` must still accept the trailing
/// positional); see task 6661 for the class this belongs to.
///
/// Its one sibling in that class, `capture commits shas`, is defended
/// from both directions and lives in `handlers/capture.cpp`.
///
/// ONE NORMALIZATION, recorded because it is the only call below that is
/// not a mechanical transcription. `tree.cpp` declared `--format` with
/// CLI11's `default_val("text")`; this uses `add_string_default`, which
/// is `default_str`. That is the mechanism `apply_surface`'s
/// `declare_flag` used for every OTHER declared default in the tree, so
/// it is the convention rather than a deviation, and the catalog reports
/// `"default":"text"` either way (verified byte-identical). It is
/// unobservable at runtime as well: the handler reads the flag as
/// `flag_string(args, "--format").value_or("text")` and never depends on
/// CLI11 filling the value in.
auto declare_spec(CLI::App& root) -> void {
  CLI::App* spec = root.add_subcommand(
      "spec",
      "Commands for the planning pipeline spec surface.\n\n  'spec ingest' decomposes workbench planning documents into a\n  "
      "structured task graph in the database.\n  'spec draft' generates initial spec artifacts from a goal statement.");
  spec->require_subcommand(0);

  CLI::App* ingest = spec->add_subcommand("ingest", "Decompose workbench spec documents into the task graph.");
  add_bool(*ingest, "--apply");
  add_bool(*ingest, "--apply-removals");
  add_string_default(*ingest, "--format", "text");
  add_string(*ingest, "--scope");
  add_bool(*ingest, "--strict");
  add_json(*ingest);
  add_positional(*ingest, "plan");
  // Hidden variadic "rest" positional -- see this function's header.
  ingest->add_option("extra-plans")->expected(0, -1)->group("");
}

} // namespace planar::cmd::handlers
