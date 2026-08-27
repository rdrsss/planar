/// @file artifact.cpp
/// @brief Implementation of `planar.engine.planning.artifact` (see
/// artifact.cppm).

module;

module planar.engine.planning.artifact;

import std;
import planar.db;
import planar.json_text;
import planar.scope_ref;
import planar.policy;
import planar.engine.planning.transitions;

namespace planar::engine::planning {

using json_text::json_string;

namespace audit = planar::policy::audit;

namespace {

/// @brief The column list every read in this module selects, in the order
/// `read_row` decodes. `slug` is deliberately absent — the oracle's
/// `readRow` does not select it either, and no `artifact` leaf emits it.
constexpr std::string_view k_select_columns =
    "select id, scope_kind, scope_id, kind, title, body, source_path, status, created_at, updated_at from artifacts";

/// @brief Append one `audit_log` row, mapping a write failure into this
/// module's error surface. Called AFTER the artifact's own write succeeds.
/// @param conn An open, migrated connection.
/// @param args The row to write.
/// @return Nothing, or `audit_write_failed`.
auto record_audit(db::connection& conn, const audit::record_args& args) -> std::expected<void, artifact_error> {
  if (auto ok = audit::record(conn, args); !ok) {
    return std::unexpected(artifact_error::audit_write_failed);
  }
  return {};
}

/// @brief Render a scope kind as its column text.
/// @param k The scope kind.
/// @return The column text.
auto scope_kind_to_text(artifact_scope_kind k) -> std::string_view {
  switch (k) {
  case artifact_scope_kind::global:
    return "global";
  case artifact_scope_kind::association:
    return "association";
  case artifact_scope_kind::repo:
    return "repo";
  }
  return "global"; // unreachable
}

/// @brief Translate `planar.scope_ref`'s `scope_kind` onto this module's own.
/// @param k The layer-1 scope kind.
/// @return This module's corresponding scope kind.
auto to_artifact_kind(scope_ref::scope_kind k) -> artifact_scope_kind {
  switch (k) {
  case scope_ref::scope_kind::global:
    return artifact_scope_kind::global;
  case scope_ref::scope_kind::association:
    return artifact_scope_kind::association;
  case scope_ref::scope_kind::repo:
    return artifact_scope_kind::repo;
  }
  return artifact_scope_kind::global; // unreachable
}

/// @brief Map a `planar.scope_ref` failure onto this module's error surface.
/// @param e The layer-1 failure.
/// @return The corresponding member.
auto to_artifact_error(scope_ref::error e) -> artifact_error {
  switch (e) {
  case scope_ref::error::query_failed:
    return artifact_error::query_failed;
  case scope_ref::error::slug_not_found:
    return artifact_error::slug_not_found;
    // `scope_ref::error` has exactly these TWO members — there is no
    // `unsupported_scope` at layer 1, so `artifact_error`'s member of that
    // name is unreachable from here. It stays in the enum because zig's
    // `artifact.Error` carries `UnsupportedScope` and the mapping must be
    // total against the ORACLE's surface, not against this layer's.
  }
  return artifact_error::query_failed; // unreachable
}

using resolved_scope = std::pair<artifact_scope_kind, std::optional<std::int64_t>>;

/// @brief Resolve `scope` (when present) to a `(kind,id)` pair, else global.
///
/// The "no scope argument at all -> global" fold is distinct from the
/// literal string `"global"`, which goes through `scope_ref::resolve` like
/// any other slug.
/// @param conn An open, migrated connection.
/// @param scope The scope slug, or unset.
/// @return The resolved pair, or the mapped failure.
auto resolve_scope_or_global(db::connection& conn, const std::optional<std::string>& scope)
    -> std::expected<resolved_scope, artifact_error> {
  if (!scope.has_value()) {
    return std::make_pair(artifact_scope_kind::global, std::optional<std::int64_t>{});
  }
  auto resolved = scope_ref::resolve(conn, *scope);
  if (!resolved) {
    return std::unexpected(to_artifact_error(resolved.error()));
  }
  return std::make_pair(to_artifact_kind(resolved->kind), resolved->id);
}

/// @brief Resolve `filter.scope` followed by every member of
/// `filter.scopes` into ONE ordered disjunction.
///
/// An unresolvable slug ANYWHERE in the set fails the whole call rather
/// than being skipped: a read verb that quietly drops one member of its
/// scope set returns a short list that looks complete.
/// @param conn An open, migrated connection.
/// @param filter The filter carrying the scope members.
/// @return The resolved set in filter order, or the first failure.
auto resolve_scope_set(db::connection& conn, const artifact_list_filter& filter)
    -> std::expected<std::vector<resolved_scope>, artifact_error> {
  std::vector<resolved_scope> refs;
  if (filter.scope.has_value()) {
    auto resolved = scope_ref::resolve(conn, *filter.scope);
    if (!resolved) {
      return std::unexpected(to_artifact_error(resolved.error()));
    }
    refs.emplace_back(to_artifact_kind(resolved->kind), resolved->id);
  }
  for (const auto& s : filter.scopes) {
    auto resolved = scope_ref::resolve(conn, s);
    if (!resolved) {
      return std::unexpected(to_artifact_error(resolved.error()));
    }
    refs.emplace_back(to_artifact_kind(resolved->kind), resolved->id);
  }
  return refs;
}

/// @brief The status predicate for a list query.
///
/// THE EMPTY ARM IS NOT "no predicate". It is the literal `status in
/// ('draft','active')`, which is what keeps `superseded` and `retired`
/// rows off a bare `artifact list`. `scenario`'s empty arm emits nothing
/// at all and `question`'s emits `status = 'open'`; copying either here
/// changes which rows an operator sees.
/// @param filter The filter carrying the statuses.
/// @return The SQL fragment, leading space included. Never empty.
auto status_clause(const artifact_list_filter& filter) -> std::string {
  if (filter.statuses.empty()) {
    return " and status in ('draft','active')";
  }
  std::string out = " and status in (";
  for (std::size_t i = 0; i < filter.statuses.size(); ++i) {
    if (i > 0) {
      out += ",";
    }
    out += "?";
  }
  out += ")";
  return out;
}

/// @brief The kind predicate for a list query. An empty set emits nothing.
/// @param filter The filter carrying the kinds.
/// @return The SQL fragment, leading space included.
auto kind_clause(const artifact_list_filter& filter) -> std::string {
  if (filter.kinds.empty()) {
    return {};
  }
  std::string out = " and kind in (";
  for (std::size_t i = 0; i < filter.kinds.size(); ++i) {
    if (i > 0) {
      out += ",";
    }
    out += "?";
  }
  out += ")";
  return out;
}

/// @brief The scope disjunction for a list query.
///
/// `global` matches on `scope_kind` ALONE, with no bound parameter; the
/// two other kinds carry their kind as a LITERAL and bind only the id.
/// @param refs The resolved scope set; an empty set yields an empty string.
/// @return The SQL fragment, leading space included.
auto scope_clause(std::span<const resolved_scope> refs) -> std::string {
  if (refs.empty()) {
    return {};
  }
  std::string out = " and (";
  for (std::size_t i = 0; i < refs.size(); ++i) {
    if (i > 0) {
      out += " or ";
    }
    if (refs[i].first == artifact_scope_kind::global) {
      out += "scope_kind='global'";
    } else if (refs[i].first == artifact_scope_kind::association) {
      out += "(scope_kind='association' and scope_id=?)";
    } else {
      out += "(scope_kind='repo' and scope_id=?)";
    }
  }
  out += ")";
  return out;
}

/// @brief Decode one row of `k_select_columns` into an `artifact`.
/// @param stmt A statement positioned on a row.
/// @return The decoded row, or `query_failed` for an unparseable enum column.
auto read_row(db::statement& stmt) -> std::expected<artifact, artifact_error> {
  const auto          scope_kind_text = stmt.column_text(1);
  artifact_scope_kind sk{};
  if (scope_kind_text == "global") {
    sk = artifact_scope_kind::global;
  } else if (scope_kind_text == "association") {
    sk = artifact_scope_kind::association;
  } else if (scope_kind_text == "repo") {
    sk = artifact_scope_kind::repo;
  } else {
    return std::unexpected(artifact_error::query_failed);
  }

  const auto kind = artifact_kind_from_text(stmt.column_text(3));
  if (!kind.has_value()) {
    return std::unexpected(artifact_error::query_failed);
  }
  const auto status = artifact_status_from_text(stmt.column_text(7));
  if (!status.has_value()) {
    return std::unexpected(artifact_error::query_failed);
  }

  return artifact{
      .id         = stmt.column_int64(0),
      .scope_kind = sk,
      .scope_id   = stmt.is_null(2) ? std::optional<std::int64_t>{} : std::optional<std::int64_t>{stmt.column_int64(2)},
      .kind       = *kind,
      .title      = stmt.column_text(4),
      // `body` is nullable: an absent body is NULL and renders `null`,
      // while `--body ""` is `''` and renders `""`. Distinct values.
      .body        = stmt.is_null(5) ? std::optional<std::string>{} : std::optional<std::string>{stmt.column_text(5)},
      .source_path = stmt.is_null(6) ? std::optional<std::string>{} : std::optional<std::string>{stmt.column_text(6)},
      .status      = *status,
      .created_at  = stmt.column_text(8),
      .updated_at  = stmt.column_text(9),
  };
}

/// @brief Map a transition-matrix failure onto this module's error surface.
///
/// Both members fold onto `illegal_transition`, matching zig's
/// `validateTransition`, which maps `error.UnknownStatus` onto
/// `Error.IllegalTransition` rather than surfacing it separately. The
/// operator therefore sees the same tag either way.
/// @param e The matrix failure.
/// @return `illegal_transition`.
auto to_artifact_error(transition_error e) -> artifact_error {
  static_cast<void>(e);
  return artifact_error::illegal_transition;
}

} // namespace

auto artifact_kind_from_text(std::string_view s) -> std::optional<artifact_kind> {
  if (s == "tech_spec") {
    return artifact_kind::tech_spec;
  }
  if (s == "adr") {
    return artifact_kind::adr;
  }
  if (s == "design_note") {
    return artifact_kind::design_note;
  }
  if (s == "summary") {
    return artifact_kind::summary;
  }
  if (s == "readme") {
    return artifact_kind::readme;
  }
  if (s == "generated") {
    return artifact_kind::generated;
  }
  if (s == "other") {
    return artifact_kind::other;
  }
  if (s == "product_spec") {
    return artifact_kind::product_spec;
  }
  if (s == "roadmap") {
    return artifact_kind::roadmap;
  }
  if (s == "research") {
    return artifact_kind::research;
  }
  if (s == "getting_started") {
    return artifact_kind::getting_started;
  }
  if (s == "changelog_entry") {
    return artifact_kind::changelog_entry;
  }
  if (s == "glossary_term") {
    return artifact_kind::glossary_term;
  }
  if (s == "test_spec") {
    return artifact_kind::test_spec;
  }
  return std::nullopt;
}

auto artifact_kind_to_text(artifact_kind k) -> std::string_view {
  switch (k) {
  case artifact_kind::tech_spec:
    return "tech_spec";
  case artifact_kind::adr:
    return "adr";
  case artifact_kind::design_note:
    return "design_note";
  case artifact_kind::summary:
    return "summary";
  case artifact_kind::readme:
    return "readme";
  case artifact_kind::generated:
    return "generated";
  case artifact_kind::other:
    return "other";
  case artifact_kind::product_spec:
    return "product_spec";
  case artifact_kind::roadmap:
    return "roadmap";
  case artifact_kind::research:
    return "research";
  case artifact_kind::getting_started:
    return "getting_started";
  case artifact_kind::changelog_entry:
    return "changelog_entry";
  case artifact_kind::glossary_term:
    return "glossary_term";
  case artifact_kind::test_spec:
    return "test_spec";
  }
  return "other"; // unreachable
}

auto artifact_status_from_text(std::string_view s) -> std::optional<artifact_status> {
  if (s == "draft") {
    return artifact_status::draft;
  }
  if (s == "active") {
    return artifact_status::active;
  }
  if (s == "superseded") {
    return artifact_status::superseded;
  }
  if (s == "retired") {
    return artifact_status::retired;
  }
  return std::nullopt;
}

auto artifact_status_to_text(artifact_status s) -> std::string_view {
  switch (s) {
  case artifact_status::draft:
    return "draft";
  case artifact_status::active:
    return "active";
  case artifact_status::superseded:
    return "superseded";
  case artifact_status::retired:
    return "retired";
  }
  return "draft"; // unreachable
}

auto read_body(std::string_view value) -> std::optional<std::string> {
  if (value.empty() || value.front() != '@') {
    return std::string{value};
  }
  const std::filesystem::path path{std::string{value.substr(1)}};
  std::ifstream               in{path, std::ios::binary};
  if (!in) {
    return std::nullopt;
  }
  std::ostringstream buf;
  buf << in.rdbuf();
  if (in.bad()) {
    return std::nullopt;
  }
  // Byte-for-byte. No front-matter stripping happens on this path in the
  // oracle, so a file that opens with a `---` block reaches the column
  // WITH that block intact.
  return buf.str();
}

auto create_artifact(db::connection& conn, const artifact_create_args& args) -> std::expected<artifact, artifact_error> {
  auto scope = resolve_scope_or_global(conn, args.scope);
  if (!scope) {
    return std::unexpected(scope.error());
  }

  // `--plan` is checked BEFORE the insert, so a nonexistent plan writes
  // NOTHING — not the artifact, not the audit row, not the edge. That is
  // the opposite of `scenario add --plan`, which never checks and leaves a
  // dangling edge behind.
  if (args.plan_id.has_value()) {
    auto stmt = conn.prepare("select count(*) from plans where id = ?");
    if (!stmt) {
      return std::unexpected(artifact_error::query_failed);
    }
    if (auto b = stmt->bind_int64(1, *args.plan_id); !b) {
      return std::unexpected(artifact_error::query_failed);
    }
    auto step = stmt->step();
    if (!step || *step != db::step_result::row) {
      return std::unexpected(artifact_error::query_failed);
    }
    if (stmt->column_int64(0) == 0) {
      return std::unexpected(artifact_error::not_found);
    }
  }

  std::int64_t id = 0;
  {
    // `status` IS in the column list here, unlike `create_scenario`, which
    // lets the column default apply. The `artifacts.status` default is
    // `active` while `artifact add`'s CLI default is `draft`; binding it
    // explicitly is what makes a bare `artifact add` land `draft`.
    auto stmt = conn.prepare("insert into artifacts (scope_kind, scope_id, kind, title, body, source_path, status) "
                             "values (?, ?, ?, ?, ?, ?, ?) returning id");
    if (!stmt) {
      return std::unexpected(artifact_error::query_failed);
    }
    if (auto b = stmt->bind_text(1, scope_kind_to_text(scope->first)); !b) {
      return std::unexpected(artifact_error::query_failed);
    }
    auto b2 = scope->second.has_value() ? stmt->bind_int64(2, *scope->second) : stmt->bind_null(2);
    if (!b2) {
      return std::unexpected(artifact_error::query_failed);
    }
    if (auto b = stmt->bind_text(3, artifact_kind_to_text(args.kind)); !b) {
      return std::unexpected(artifact_error::query_failed);
    }
    if (auto b = stmt->bind_text(4, args.title); !b) {
      return std::unexpected(artifact_error::query_failed);
    }
    auto b5 = args.body.has_value() ? stmt->bind_text(5, *args.body) : stmt->bind_null(5);
    if (!b5) {
      return std::unexpected(artifact_error::query_failed);
    }
    auto b6 = args.source_path.has_value() ? stmt->bind_text(6, *args.source_path) : stmt->bind_null(6);
    if (!b6) {
      return std::unexpected(artifact_error::query_failed);
    }
    if (auto b = stmt->bind_text(7, artifact_status_to_text(args.status)); !b) {
      return std::unexpected(artifact_error::query_failed);
    }
    auto step = stmt->step();
    if (!step || *step != db::step_result::row) {
      return std::unexpected(artifact_error::query_failed);
    }
    id = stmt->column_int64(0);
  }

  // ORACLE: summary is `create artifact '<title>' (kind=<kind>)` — the
  // kind is part of it, where `scenario`'s summary is the title alone.
  if (auto a = record_audit(conn, audit::record_args{.verb    = audit::verb::create,
                                                     .entity  = {.kind = "artifact", .id = id},
                                                     .summary = std::format("create artifact '{}' (kind={})", args.title,
                                                                            artifact_kind_to_text(args.kind))});
      !a) {
    return std::unexpected(a.error());
  }

  if (args.plan_id.has_value()) {
    // No `link` audit row — `artifact add --plan` writes exactly one
    // `audit_log` row and its verb is `create`.
    auto stmt = conn.prepare("insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) "
                             "values ('artifact', ?, 'plan', ?, 'derives-from')");
    if (!stmt) {
      return std::unexpected(artifact_error::query_failed);
    }
    if (auto b = stmt->bind_int64(1, id); !b) {
      return std::unexpected(artifact_error::query_failed);
    }
    if (auto b = stmt->bind_int64(2, *args.plan_id); !b) {
      return std::unexpected(artifact_error::query_failed);
    }
    if (!stmt->step().has_value()) {
      return std::unexpected(artifact_error::query_failed);
    }
  }

  return show_artifact(conn, id);
}

auto show_artifact(db::connection& conn, std::int64_t id) -> std::expected<artifact, artifact_error> {
  auto stmt = conn.prepare(std::string{k_select_columns} + " where id = ?");
  if (!stmt) {
    return std::unexpected(artifact_error::query_failed);
  }
  if (auto b = stmt->bind_int64(1, id); !b) {
    return std::unexpected(artifact_error::query_failed);
  }
  auto step = stmt->step();
  if (!step) {
    return std::unexpected(artifact_error::query_failed);
  }
  if (*step != db::step_result::row) {
    return std::unexpected(artifact_error::not_found);
  }
  return read_row(*stmt);
}

auto list_artifacts(db::connection& conn, const artifact_list_filter& filter)
    -> std::expected<std::vector<artifact>, artifact_error> {
  auto refs = resolve_scope_set(conn, filter);
  if (!refs) {
    return std::unexpected(refs.error());
  }

  std::string sql = std::string{k_select_columns} + " where 1=1";
  sql += status_clause(filter);
  sql += kind_clause(filter);
  if (filter.plan_id.has_value()) {
    sql += " and id in (select from_id from entity_links where from_kind='artifact' and to_kind='plan' and "
           "relationship='derives-from' and to_id=?)";
  }
  sql += scope_clause(*refs);
  sql += " order by id";

  auto stmt = conn.prepare(sql);
  if (!stmt) {
    return std::unexpected(artifact_error::query_failed);
  }

  // Bind order MUST match clause-emission order above: statuses, kinds,
  // plan, then the non-global scope ids.
  int index = 1;
  for (const auto& s : filter.statuses) {
    if (auto b = stmt->bind_text(index++, artifact_status_to_text(s)); !b) {
      return std::unexpected(artifact_error::query_failed);
    }
  }
  for (const auto& k : filter.kinds) {
    if (auto b = stmt->bind_text(index++, artifact_kind_to_text(k)); !b) {
      return std::unexpected(artifact_error::query_failed);
    }
  }
  if (filter.plan_id.has_value()) {
    if (auto b = stmt->bind_int64(index++, *filter.plan_id); !b) {
      return std::unexpected(artifact_error::query_failed);
    }
  }
  for (const auto& ref : *refs) {
    if (ref.first == artifact_scope_kind::global) {
      continue; // matched on `scope_kind` alone; binds nothing
    }
    if (auto b = stmt->bind_int64(index++, ref.second.value_or(0)); !b) {
      return std::unexpected(artifact_error::query_failed);
    }
  }

  std::vector<artifact> out;
  for (;;) {
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(artifact_error::query_failed);
    }
    if (*step == db::step_result::done) {
      break;
    }
    auto row = read_row(*stmt);
    if (!row) {
      return std::unexpected(row.error());
    }
    out.push_back(std::move(*row));
  }
  return out;
}

auto update_artifact(db::connection& conn, std::int64_t id, const artifact_update_args& patch)
    -> std::expected<artifact, artifact_error> {
  // Scope resolution runs FIRST — ahead of the existence check — so
  // `artifact update <missing-id> --scope <bad-slug>` reports the SLUG
  // failure, not `not_found`. That ordering is the oracle's and is
  // operator-visible.
  std::optional<resolved_scope> scope;
  if (patch.scope.has_value()) {
    auto resolved = scope_ref::resolve(conn, *patch.scope);
    if (!resolved) {
      return std::unexpected(to_artifact_error(resolved.error()));
    }
    scope = std::make_pair(to_artifact_kind(resolved->kind), resolved->id);
  }

  auto current = show_artifact(conn, id);
  if (!current) {
    return std::unexpected(current.error());
  }

  if (!patch.title.has_value() && !patch.body.has_value() && !patch.status.has_value() && !patch.source_path.has_value() &&
      !scope.has_value()) {
    return std::unexpected(artifact_error::no_fields);
  }

  if (patch.status.has_value()) {
    auto ok = check_transition(transition_kind::artifact, artifact_status_to_text(current->status),
                               artifact_status_to_text(*patch.status), false);
    if (!ok) {
      return std::unexpected(to_artifact_error(ok.error()));
    }
  }

  std::string              sql = "update artifacts set ";
  std::vector<std::string> text_params;
  bool                     first = true;
  auto const               sep   = [&](std::string_view fragment) {
    if (!first) {
      sql += ", ";
    }
    first = false;
    sql += fragment;
  };

  // The scope pair is emitted as TWO assignments, `scope_kind` then
  // `scope_id`, and comes first in the SET list.
  if (scope.has_value()) {
    sep("scope_kind = ?");
    sep("scope_id = ?");
  }
  if (patch.title.has_value()) {
    sep("title = ?");
  }
  if (patch.body.has_value()) {
    sep("body = ?");
  }
  if (patch.status.has_value()) {
    sep("status = ?");
  }
  if (patch.source_path.has_value()) {
    sep("source_path = ?");
  }
  // Always bumped, even when the only change was an identity status move.
  sep("updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') where id = ?");

  auto stmt = conn.prepare(sql);
  if (!stmt) {
    return std::unexpected(artifact_error::query_failed);
  }
  int index = 1;
  if (scope.has_value()) {
    if (auto b = stmt->bind_text(index++, scope_kind_to_text(scope->first)); !b) {
      return std::unexpected(artifact_error::query_failed);
    }
    auto b = scope->second.has_value() ? stmt->bind_int64(index++, *scope->second) : stmt->bind_null(index++);
    if (!b) {
      return std::unexpected(artifact_error::query_failed);
    }
  }
  if (patch.title.has_value()) {
    if (auto b = stmt->bind_text(index++, *patch.title); !b) {
      return std::unexpected(artifact_error::query_failed);
    }
  }
  if (patch.body.has_value()) {
    if (auto b = stmt->bind_text(index++, *patch.body); !b) {
      return std::unexpected(artifact_error::query_failed);
    }
  }
  if (patch.status.has_value()) {
    if (auto b = stmt->bind_text(index++, artifact_status_to_text(*patch.status)); !b) {
      return std::unexpected(artifact_error::query_failed);
    }
  }
  if (patch.source_path.has_value()) {
    if (auto b = stmt->bind_text(index++, *patch.source_path); !b) {
      return std::unexpected(artifact_error::query_failed);
    }
  }
  if (auto b = stmt->bind_int64(index++, id); !b) {
    return std::unexpected(artifact_error::query_failed);
  }
  if (!stmt->step().has_value()) {
    return std::unexpected(artifact_error::query_failed);
  }

  // The verb depends on whether a status moved, and the summary is
  // genuinely NULL either way — unlike `create`, which carries prose.
  const auto verb = patch.status.has_value() ? audit::verb::status_change : audit::verb::update;
  if (auto a = record_audit(conn, audit::record_args{.verb = verb, .entity = {.kind = "artifact", .id = id}}); !a) {
    return std::unexpected(a.error());
  }

  return show_artifact(conn, id);
}

auto render_text(const artifact& a) -> std::string {
  // Labels pad to THIRTEEN columns. `scenario`'s pad to twelve; the two
  // families genuinely differ and both were captured.
  std::string out;
  out += std::format("id:          {}\n", a.id);
  out += std::format("title:       {}\n", a.title);
  out += std::format("kind:        {}\n", artifact_kind_to_text(a.kind));
  out += std::format("status:      {}\n", artifact_status_to_text(a.status));
  out += std::format("scope:       {}", scope_kind_to_text(a.scope_kind));
  if (a.scope_id.has_value()) {
    out += std::format(":{}", *a.scope_id);
  }
  out += "\n";
  // `body:` is CONDITIONAL on the column being non-NULL, and sits between
  // `scope:` and `created:`. A present-but-EMPTY body still prints the
  // line, with nothing after the padding.
  if (a.body.has_value()) {
    out += std::format("body:        {}\n", *a.body);
  }
  out += std::format("created:     {}\n", a.created_at);
  out += std::format("updated:     {}\n", a.updated_at);
  return out;
}

auto render_json(const artifact& a) -> std::string {
  auto const opt_str = [](const std::optional<std::string>& v) -> std::string {
    return v.has_value() ? json_string(*v) : std::string{"null"};
  };
  auto const opt_int = [](const std::optional<std::int64_t>& v) -> std::string {
    return v.has_value() ? std::format("{}", *v) : std::string{"null"};
  };
  return std::format(R"({{"id":{},"scope_kind":"{}","scope_id":{},"kind":"{}","title":{},"body":{},)"
                     R"("source_path":{},"status":"{}","created_at":{},"updated_at":{}}})",
                     a.id, scope_kind_to_text(a.scope_kind), opt_int(a.scope_id), artifact_kind_to_text(a.kind),
                     json_string(a.title), opt_str(a.body), opt_str(a.source_path), artifact_status_to_text(a.status),
                     json_string(a.created_at), json_string(a.updated_at));
}

auto render_list_text(std::span<const artifact> items) -> std::string {
  if (items.empty()) {
    return "(no artifacts)\n";
  }
  std::string out;
  for (const auto& a : items) {
    // Columns: id right-aligned to 5, status left to 10, kind left to 16,
    // then the title. `kind` occupies a column `scenario`'s table spends
    // on `outcome`, so the two tables are NOT interchangeable.
    out += std::format("{:>5}  {:<10}  {:<16}  {}\n", a.id, artifact_status_to_text(a.status), artifact_kind_to_text(a.kind),
                       a.title);
  }
  return out;
}

auto render_list_json(std::span<const artifact> items) -> std::string {
  std::string out = "[";
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (i > 0) {
      out += ",";
    }
    out += render_json(items[i]);
  }
  out += "]";
  return out;
}

} // namespace planar::engine::planning
