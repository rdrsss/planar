/// @file entitylink.cpp
/// @brief Implementation of `planar.engine.entitylink` (plan 996, task
/// cpp-entity-links). See entitylink.cppm for the module's scope and
/// cut-list.

module planar.engine.entitylink;

import std;
import planar.db;
import planar.json_text;
import planar.log;
import planar.policy;

namespace planar::engine::entitylink {

namespace {

// SQLITE_CONSTRAINT_UNIQUE — same constant `engine_planning`'s plan.cpp /
// task.cpp already use to detect a UNIQUE-constraint violation without
// string-matching the driver's error message.
constexpr int k_sqlite_constraint_unique = 2067;

/// @brief Emit the oracle's inner `<op> exec failed: <ErrorName>` diagnostic
/// ahead of the outer handler error. Mirrors `engine::planning::exec_failed`
/// (task.cpp); see `zig/src/engine/entitylink.zig`'s `add`/`remove` for the
/// oracle shapes this ports.
auto exec_failed(std::string_view op, std::string_view zig_error_name) -> entity_link_error {
  log::diag_err(std::format("{} exec failed: {}", op, zig_error_name));
  return entity_link_error::query_failed;
}

auto is_unique_violation(const db::db_error& err) -> bool {
  return err.code_ == k_sqlite_constraint_unique;
}

// The table backing each entity kind, for the write-time existence check.
// entity_links has no FK constraint (the column pair is polymorphic), so
// this is a manual lookup, not a schema-derived one — mirrors zig's
// `tableFor`.
auto table_for(entity_kind k) -> std::string_view {
  switch (k) {
  case entity_kind::plan:
    return "plans";
  case entity_kind::plan_step:
    return "plan_steps";
  case entity_kind::task:
    return "tasks";
  case entity_kind::question:
    return "questions";
  case entity_kind::test_scenario:
    return "test_scenarios";
  case entity_kind::artifact:
    return "artifacts";
  case entity_kind::decision:
    return "decisions";
  case entity_kind::session:
    return "sessions";
  case entity_kind::repo:
    return "projects";
  case entity_kind::annotation:
    return "annotations";
  }
  return "plans"; // unreachable — every enumerator is handled above.
}

// True when `id` exists in the table backing `kind`. Fails OPEN (reports
// "exists") on any driver error — mirrors zig's `endpointExists`: this
// guard must never turn a transient DB problem into a refusal to record a
// legitimate link.
auto endpoint_exists(db::connection& conn, entity_kind kind, std::int64_t id) -> bool {
  auto sql  = std::format("select 1 from {} where id = ?", table_for(kind));
  auto stmt = conn.prepare(sql);
  if (!stmt) {
    return true;
  }
  if (auto b = stmt->bind_int64(1, id); !b) {
    return true;
  }
  auto step = stmt->step();
  if (!step) {
    return true;
  }
  return *step == db::step_result::row;
}

constexpr std::string_view select_columns = "id, from_kind, from_id, to_kind, to_id, relationship, created_at";

auto read_row(const db::statement& stmt) -> std::expected<entity_link, entity_link_error> {
  auto from_kind = entity_kind_from_text(stmt.column_text(1));
  if (!from_kind) {
    return std::unexpected(entity_link_error::query_failed);
  }
  auto to_kind = entity_kind_from_text(stmt.column_text(3));
  if (!to_kind) {
    return std::unexpected(entity_link_error::query_failed);
  }
  auto rel = relationship_from_text(stmt.column_text(5));
  if (!rel) {
    return std::unexpected(entity_link_error::query_failed);
  }
  return entity_link{
      .id            = stmt.column_int64(0),
      .from_kind     = *from_kind,
      .from_id       = stmt.column_int64(2),
      .to_kind       = *to_kind,
      .to_id         = stmt.column_int64(4),
      .relationship_ = *rel,
      .created_at    = stmt.column_text(6),
  };
}

} // namespace

auto entity_kind_from_text(std::string_view s) -> std::optional<entity_kind> {
  if (s == "plan")
    return entity_kind::plan;
  if (s == "plan_step")
    return entity_kind::plan_step;
  if (s == "task")
    return entity_kind::task;
  if (s == "question")
    return entity_kind::question;
  if (s == "test_scenario")
    return entity_kind::test_scenario;
  if (s == "artifact")
    return entity_kind::artifact;
  if (s == "decision")
    return entity_kind::decision;
  if (s == "session")
    return entity_kind::session;
  if (s == "repo")
    return entity_kind::repo;
  if (s == "annotation")
    return entity_kind::annotation;
  return std::nullopt;
}

auto entity_kind_to_text(entity_kind k) -> std::string_view {
  switch (k) {
  case entity_kind::plan:
    return "plan";
  case entity_kind::plan_step:
    return "plan_step";
  case entity_kind::task:
    return "task";
  case entity_kind::question:
    return "question";
  case entity_kind::test_scenario:
    return "test_scenario";
  case entity_kind::artifact:
    return "artifact";
  case entity_kind::decision:
    return "decision";
  case entity_kind::session:
    return "session";
  case entity_kind::repo:
    return "repo";
  case entity_kind::annotation:
    return "annotation";
  }
  return "plan"; // unreachable
}

auto relationship_from_text(std::string_view s) -> std::optional<relationship> {
  if (s == "derives-from")
    return relationship::derives_from;
  if (s == "depends-on")
    return relationship::depends_on;
  // "blocks" is the pre-migration-00033 spelling — deliberately rejected,
  // not aliased. See this function's doc comment in entitylink.cppm.
  if (s == "blocks")
    return std::nullopt;
  if (s == "addresses")
    return relationship::addresses;
  if (s == "verifies")
    return relationship::verifies;
  if (s == "cites")
    return relationship::cites;
  if (s == "supersedes")
    return relationship::supersedes;
  if (s == "touches")
    return relationship::touches;
  return std::nullopt;
}

auto relationship_to_text(relationship r) -> std::string_view {
  switch (r) {
  case relationship::derives_from:
    return "derives-from";
  case relationship::depends_on:
    return "depends-on";
  case relationship::addresses:
    return "addresses";
  case relationship::verifies:
    return "verifies";
  case relationship::cites:
    return "cites";
  case relationship::supersedes:
    return "supersedes";
  case relationship::touches:
    return "touches";
  }
  return "derives-from"; // unreachable
}

auto parse_ref(std::string_view s) -> std::expected<parsed_ref, entity_link_error> {
  auto colon = s.find_last_of(':');
  if (colon == std::string_view::npos) {
    return std::unexpected(entity_link_error::invalid_ref);
  }
  auto kind_text = s.substr(0, colon);
  auto ref_text  = s.substr(colon + 1);
  if (kind_text.empty() || ref_text.empty()) {
    return std::unexpected(entity_link_error::invalid_ref);
  }

  auto kind = entity_kind_from_text(kind_text);
  if (!kind) {
    return std::unexpected(entity_link_error::invalid_ref);
  }

  // Try to parse ref_text as a positive integer id. A PARTIAL parse (e.g.
  // "42abc" — digits followed by trailing garbage) does NOT count as an
  // integer id: `ptr` must land exactly at the end of `ref_text`. This
  // matches zig's `std.fmt.parseInt`, which also rejects trailing garbage —
  // but zig's `parseRef` treats ANY parseInt failure (partial OR total) as
  // "not an integer, fall through to slug", not as `Error.InvalidRef`. So
  // "42abc" is a valid (if unusual) slug ref here, same as the oracle.
  std::int64_t id_val = 0;
  auto [ptr, ec]      = std::from_chars(ref_text.data(), ref_text.data() + ref_text.size(), id_val);
  if (ec == std::errc{} && ptr == ref_text.data() + ref_text.size()) {
    if (id_val <= 0) {
      return std::unexpected(entity_link_error::invalid_ref);
    }
    return parsed_ref{.value = parsed_ref::id_ref{.kind = *kind, .id = id_val}};
  }
  return parsed_ref{.value = parsed_ref::slug_ref{.kind = *kind, .slug = std::string(ref_text)}};
}

auto missing_endpoint_of(db::connection& conn, const entity_link_add_args& args) -> std::optional<missing_endpoint> {
  if (!endpoint_exists(conn, args.from_kind, args.from_id)) {
    return missing_endpoint::from;
  }
  if (!endpoint_exists(conn, args.to_kind, args.to_id)) {
    return missing_endpoint::to;
  }
  return std::nullopt;
}

auto add(db::connection& conn, const entity_link_add_args& args) -> std::expected<entity_link, entity_link_error> {
  if (args.scope.has_value()) {
    return std::unexpected(entity_link_error::unsupported_scope);
  }

  // Existence is checked here, not by the schema (see this file's
  // `endpoint_exists` comment). This is NOT a scope check — link verbs stay
  // deliberately unguarded so polyrepo edges can cross scopes; a legitimate
  // cross-scope link still succeeds because "does this entity exist" and
  // "is it in my scope" are different questions.
  if (missing_endpoint_of(conn, args).has_value()) {
    return std::unexpected(entity_link_error::endpoint_not_found);
  }

  auto stmt = conn.prepare("insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) "
                           "values (?, ?, ?, ?, ?) returning id");
  if (!stmt) {
    return std::unexpected(exec_failed("entitylink.add", "PrepareFailed"));
  }
  if (auto b = stmt->bind_text(1, entity_kind_to_text(args.from_kind)); !b) {
    return std::unexpected(exec_failed("entitylink.add", "BindFailed"));
  }
  if (auto b = stmt->bind_int64(2, args.from_id); !b) {
    return std::unexpected(exec_failed("entitylink.add", "BindFailed"));
  }
  if (auto b = stmt->bind_text(3, entity_kind_to_text(args.to_kind)); !b) {
    return std::unexpected(exec_failed("entitylink.add", "BindFailed"));
  }
  if (auto b = stmt->bind_int64(4, args.to_id); !b) {
    return std::unexpected(exec_failed("entitylink.add", "BindFailed"));
  }
  if (auto b = stmt->bind_text(5, relationship_to_text(args.relationship_)); !b) {
    return std::unexpected(exec_failed("entitylink.add", "BindFailed"));
  }

  auto step = stmt->step();
  if (!step) {
    if (is_unique_violation(step.error())) {
      return std::unexpected(entity_link_error::link_exists);
    }
    return std::unexpected(exec_failed("entitylink.add", "StepFailed"));
  }
  if (*step != db::step_result::row) {
    return std::unexpected(exec_failed("entitylink.add", "StepFailed"));
  }
  auto new_id = stmt->column_int64(0);

  // AFTER the insert has gone through, never before — a refused `add`
  // leaves no audit row in the oracle. Wired at task 6193 alongside
  // `trail()`, the reader that proves it: the write half alone had no
  // consumer, which is why the previous cycle deferred both together.
  if (auto rec = policy::audit::record(conn,
                                       policy::audit::record_args{
                                           .verb    = policy::audit::verb::link,
                                           .entity  = {.kind = "entity_link", .id = new_id},
                                           .actor   = std::nullopt,
                                           .scope   = std::nullopt,
                                           .summary = std::nullopt,
                                       });
      !rec) {
    return std::unexpected(entity_link_error::audit_write_failed);
  }

  return show(conn, new_id);
}

auto remove(db::connection& conn, std::int64_t id) -> std::expected<void, entity_link_error> {
  // Verify existence so we return not_found rather than a silent no-op.
  auto existing = show(conn, id);
  if (!existing) {
    return std::unexpected(existing.error());
  }

  auto stmt = conn.prepare("delete from entity_links where id = ?");
  if (!stmt) {
    return std::unexpected(exec_failed("entitylink.remove", "PrepareFailed"));
  }
  if (auto b = stmt->bind_int64(1, id); !b) {
    return std::unexpected(exec_failed("entitylink.remove", "BindFailed"));
  }
  auto step = stmt->step();
  if (!step) {
    return std::unexpected(exec_failed("entitylink.remove", "StepFailed"));
  }

  // Same ordering rule as `add`: the audit row records a delete that has
  // already happened. It deliberately OUTLIVES the row it describes — see
  // `trail`'s doc comment.
  if (auto rec = policy::audit::record(conn,
                                       policy::audit::record_args{
                                           .verb    = policy::audit::verb::unlink,
                                           .entity  = {.kind = "entity_link", .id = id},
                                           .actor   = std::nullopt,
                                           .scope   = std::nullopt,
                                           .summary = std::nullopt,
                                       });
      !rec) {
    return std::unexpected(entity_link_error::audit_write_failed);
  }
  return {};
}

auto trail(db::connection& conn, std::int64_t id) -> std::expected<std::vector<audit_row>, entity_link_error> {
  // Existence FIRST: "no such link" and "link with an empty trail" are
  // different answers and the oracle gives them different exit codes.
  auto existing = show(conn, id);
  if (!existing) {
    return std::unexpected(existing.error());
  }

  auto stmt = conn.prepare("select id, verb, entity_kind, entity_id, actor, scope, summary, recorded_at "
                           "from audit_log where entity_kind = 'entity_link' and entity_id = ? order by id");
  if (!stmt) {
    return std::unexpected(entity_link_error::query_failed);
  }
  if (auto b = stmt->bind_int64(1, id); !b) {
    return std::unexpected(entity_link_error::query_failed);
  }

  // SQL NULL must stay distinguishable from `''`: `actor`, `scope` and
  // `summary` are all nullable and all three are NULL on the CLI path, so
  // a `column_text` that folded NULL to "" would make `"actor":null` and
  // `"actor":""` render identically.
  auto const text_opt = [&stmt](int index) -> std::optional<std::string> {
    if (stmt->is_null(index)) {
      return std::nullopt;
    }
    return stmt->column_text(index);
  };

  std::vector<audit_row> rows;
  for (;;) {
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(entity_link_error::query_failed);
    }
    if (*step != db::step_result::row) {
      break;
    }
    rows.push_back(audit_row{
        .id          = stmt->column_int64(0),
        .verb        = stmt->column_text(1),
        .entity_kind = stmt->column_text(2),
        .entity_id   = stmt->column_int64(3),
        .actor       = text_opt(4),
        .scope       = text_opt(5),
        .summary     = text_opt(6),
        .recorded_at = stmt->column_text(7),
    });
  }
  return rows;
}

auto show(db::connection& conn, std::int64_t id) -> std::expected<entity_link, entity_link_error> {
  auto stmt = conn.prepare(std::format("select {} from entity_links where id = ?", select_columns));
  if (!stmt) {
    return std::unexpected(entity_link_error::query_failed);
  }
  if (auto b = stmt->bind_int64(1, id); !b) {
    return std::unexpected(entity_link_error::query_failed);
  }
  auto step = stmt->step();
  if (!step) {
    return std::unexpected(entity_link_error::query_failed);
  }
  if (*step == db::step_result::done) {
    return std::unexpected(entity_link_error::not_found);
  }
  return read_row(*stmt);
}

auto list(db::connection& conn, const entity_link_list_filter& filter)
    -> std::expected<std::vector<entity_link>, entity_link_error> {
  if (filter.scope.has_value()) {
    return std::unexpected(entity_link_error::unsupported_scope);
  }

  std::string sql = std::format("select {} from entity_links where 1 = 1", select_columns);
  if (filter.from_kind.has_value()) {
    sql += " and from_kind = ?";
  }
  if (filter.from_id.has_value()) {
    sql += " and from_id = ?";
  }
  if (filter.to_kind.has_value()) {
    sql += " and to_kind = ?";
  }
  if (filter.to_id.has_value()) {
    sql += " and to_id = ?";
  }
  if (filter.relationship_.has_value()) {
    sql += " and relationship = ?";
  }
  sql += " order by id";

  auto stmt = conn.prepare(sql);
  if (!stmt) {
    return std::unexpected(entity_link_error::query_failed);
  }

  int idx = 1;
  if (filter.from_kind.has_value()) {
    if (auto b = stmt->bind_text(idx++, entity_kind_to_text(*filter.from_kind)); !b) {
      return std::unexpected(entity_link_error::query_failed);
    }
  }
  if (filter.from_id.has_value()) {
    if (auto b = stmt->bind_int64(idx++, *filter.from_id); !b) {
      return std::unexpected(entity_link_error::query_failed);
    }
  }
  if (filter.to_kind.has_value()) {
    if (auto b = stmt->bind_text(idx++, entity_kind_to_text(*filter.to_kind)); !b) {
      return std::unexpected(entity_link_error::query_failed);
    }
  }
  if (filter.to_id.has_value()) {
    if (auto b = stmt->bind_int64(idx++, *filter.to_id); !b) {
      return std::unexpected(entity_link_error::query_failed);
    }
  }
  if (filter.relationship_.has_value()) {
    if (auto b = stmt->bind_text(idx++, relationship_to_text(*filter.relationship_)); !b) {
      return std::unexpected(entity_link_error::query_failed);
    }
  }

  std::vector<entity_link> out;
  while (true) {
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(entity_link_error::query_failed);
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

// ===========================================================================
// task_touch_paths
// ===========================================================================

auto add_touch_path(db::connection& conn, std::int64_t task_id, std::int64_t repo_id, std::string_view path)
    -> std::expected<void, entity_link_error> {
  auto stmt = conn.prepare("insert or ignore into task_touch_paths (task_id, repo_id, path) values (?, ?, ?)");
  if (!stmt) {
    return std::unexpected(entity_link_error::query_failed);
  }
  if (auto b = stmt->bind_int64(1, task_id); !b) {
    return std::unexpected(entity_link_error::query_failed);
  }
  if (auto b = stmt->bind_int64(2, repo_id); !b) {
    return std::unexpected(entity_link_error::query_failed);
  }
  if (auto b = stmt->bind_text(3, path); !b) {
    return std::unexpected(entity_link_error::query_failed);
  }
  auto step = stmt->step();
  if (!step) {
    return std::unexpected(entity_link_error::query_failed);
  }
  return {};
}

auto remove_touch_path(db::connection& conn, std::int64_t task_id, std::int64_t repo_id, std::string_view path)
    -> std::expected<void, entity_link_error> {
  // Existence is checked with a SELECT first (mirrors zig's
  // `removeTouchPath`: a DELETE's "success" says nothing about whether any
  // row actually matched, so a typo'd path would otherwise look identical
  // to a real withdrawal).
  {
    auto stmt = conn.prepare("select 1 from task_touch_paths where task_id = ? and repo_id = ? and path = ?");
    if (!stmt) {
      return std::unexpected(entity_link_error::query_failed);
    }
    if (auto b = stmt->bind_int64(1, task_id); !b) {
      return std::unexpected(entity_link_error::query_failed);
    }
    if (auto b = stmt->bind_int64(2, repo_id); !b) {
      return std::unexpected(entity_link_error::query_failed);
    }
    if (auto b = stmt->bind_text(3, path); !b) {
      return std::unexpected(entity_link_error::query_failed);
    }
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(entity_link_error::query_failed);
    }
    if (*step == db::step_result::done) {
      return std::unexpected(entity_link_error::not_found);
    }
  }

  auto del = conn.prepare("delete from task_touch_paths where task_id = ? and repo_id = ? and path = ?");
  if (!del) {
    return std::unexpected(entity_link_error::query_failed);
  }
  if (auto b = del->bind_int64(1, task_id); !b) {
    return std::unexpected(entity_link_error::query_failed);
  }
  if (auto b = del->bind_int64(2, repo_id); !b) {
    return std::unexpected(entity_link_error::query_failed);
  }
  if (auto b = del->bind_text(3, path); !b) {
    return std::unexpected(entity_link_error::query_failed);
  }
  auto step = del->step();
  if (!step) {
    return std::unexpected(entity_link_error::query_failed);
  }
  return {};
}

auto touched_paths(db::connection& conn, std::int64_t task_id) -> std::expected<std::vector<touch_path>, entity_link_error> {
  auto stmt = conn.prepare("select repo_id, path from task_touch_paths where task_id = ? order by repo_id, path");
  if (!stmt) {
    return std::unexpected(entity_link_error::query_failed);
  }
  if (auto b = stmt->bind_int64(1, task_id); !b) {
    return std::unexpected(entity_link_error::query_failed);
  }

  std::vector<touch_path> out;
  while (true) {
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(entity_link_error::query_failed);
    }
    if (*step == db::step_result::done) {
      break;
    }
    out.push_back(touch_path{.repo_id = stmt->column_int64(0), .path = stmt->column_text(1)});
  }
  return out;
}

auto touched_repo_ids(db::connection& conn, std::int64_t task_id) -> std::expected<std::vector<std::int64_t>, entity_link_error> {
  auto stmt = conn.prepare("select to_id from entity_links where from_kind = 'task' and from_id = ? "
                           "and to_kind = 'repo' and relationship = 'touches' order by id");
  if (!stmt) {
    return std::unexpected(entity_link_error::query_failed);
  }
  if (auto b = stmt->bind_int64(1, task_id); !b) {
    return std::unexpected(entity_link_error::query_failed);
  }

  std::vector<std::int64_t> out;
  while (true) {
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(entity_link_error::query_failed);
    }
    if (*step == db::step_result::done) {
      break;
    }
    out.push_back(stmt->column_int64(0));
  }
  return out;
}

// ===========================================================================
// Renderers
// ===========================================================================

namespace {

/// @brief `"<kind>:<id>"`, the ref spelling every one of these payloads uses.
/// @param kind The entity kind.
/// @param id The entity id.
/// @return The rendered ref.
auto ref_text(entity_kind kind, std::int64_t id) -> std::string {
  return std::format("{}:{}", entity_kind_to_text(kind), id);
}

/// @brief A JSON `null` or a quoted, escaped string.
/// @param value The optional text.
/// @return `"null"` when unset, otherwise the quoted JSON string.
auto json_opt(const std::optional<std::string>& value) -> std::string {
  return value.has_value() ? json_text::json_string(*value) : std::string{"null"};
}

} // namespace

auto merge_directed(std::span<const entity_link> from_links, std::span<const entity_link> to_links)
    -> std::vector<directed_link> {
  std::vector<directed_link> rows;
  rows.reserve(from_links.size() + to_links.size());
  for (auto const& link : from_links) {
    rows.push_back(directed_link{.link = link, .outbound = true});
  }
  for (auto const& link : to_links) {
    // A self-link satisfies BOTH half-queries. Listing it twice would
    // report two edges where the table holds one.
    auto const seen = std::ranges::any_of(from_links, [&link](const entity_link& f) { return f.id == link.id; });
    if (seen) {
      continue;
    }
    rows.push_back(directed_link{.link = link, .outbound = false});
  }
  return rows;
}

auto render_link_list_text(std::span<const directed_link> rows, entity_kind subject_kind, std::int64_t subject_id)
    -> std::string {
  if (rows.empty()) {
    return std::format("no links for {}\n", ref_text(subject_kind, subject_id));
  }
  std::string out = std::format("{:<6} {:<10} {:<16} {}\n", "id", "direction", "relationship", "peer");
  for (auto const& row : rows) {
    // `{:<+6}` — left-aligned, width 6, FORCED SIGN. The `+` is the
    // oracle's; see `render_link_list_text`'s doc comment.
    auto const peer = row.outbound ? ref_text(row.link.to_kind, row.link.to_id) : ref_text(row.link.from_kind, row.link.from_id);
    out += std::format("{:<+6} {:<10} {:<16} {}\n", row.link.id, row.outbound ? "from" : "to",
                       relationship_to_text(row.link.relationship_), peer);
  }
  return out;
}

auto render_link_list_json(std::span<const directed_link> rows) -> std::string {
  std::string out;
  for (auto const& row : rows) {
    out +=
        std::format(R"({{"id":{},"from_kind":"{}","from_id":{},"to_kind":"{}","to_id":{},)"
                    R"("relationship":"{}","created_at":{}}})"
                    "\n",
                    row.link.id, entity_kind_to_text(row.link.from_kind), row.link.from_id, entity_kind_to_text(row.link.to_kind),
                    row.link.to_id, relationship_to_text(row.link.relationship_), json_text::json_string(row.link.created_at));
  }
  return out;
}

auto render_trail_text(std::span<const audit_row> rows, std::int64_t link_id) -> std::string {
  if (rows.empty()) {
    return std::format("no audit trail for entity_link:{}\n", link_id);
  }
  std::string out = std::format("{:<6} {:<12} {:<12} {}\n", "id", "verb", "actor", "recorded_at");
  for (auto const& row : rows) {
    // `(none)` is the oracle's literal for a NULL actor — NOT a blank
    // column, which is what an empty-string fold would produce.
    out += std::format("{:<+6} {:<12} {:<12} {}\n", row.id, row.verb, row.actor.value_or("(none)"), row.recorded_at);
  }
  return out;
}

auto render_trail_json(std::span<const audit_row> rows) -> std::string {
  std::string out;
  for (auto const& row : rows) {
    out += std::format(R"({{"id":{},"verb":{},"entity_kind":{},"entity_id":{},)"
                       R"("actor":{},"scope":{},"summary":{},"recorded_at":{}}})"
                       "\n",
                       row.id, json_text::json_string(row.verb), json_text::json_string(row.entity_kind), row.entity_id,
                       json_opt(row.actor), json_opt(row.scope), json_opt(row.summary), json_text::json_string(row.recorded_at));
  }
  return out;
}

auto render_links_add_text(const entity_link& link, std::string_view relationship_text) -> std::string {
  // TWO spaces before `(link id:` — the oracle's.
  return std::format("created entity_link: {} --[{}]--> {}  (link id: {})\n", ref_text(link.from_kind, link.from_id),
                     relationship_text, ref_text(link.to_kind, link.to_id), link.id);
}

auto render_links_add_json(const entity_link& link, std::string_view relationship_text) -> std::string {
  return std::format(R"({{"ok":true,"id":{},"from_kind":"{}","from_id":{},"to_kind":"{}","to_id":{},)"
                     R"("relationship":{}}})"
                     "\n",
                     link.id, entity_kind_to_text(link.from_kind), link.from_id, entity_kind_to_text(link.to_kind), link.to_id,
                     json_text::json_string(relationship_text));
}

auto render_links_remove_text(std::int64_t link_id) -> std::string {
  return std::format("entity link {} removed\n", link_id);
}

auto render_links_remove_json(std::int64_t link_id) -> std::string {
  return std::format(R"({{"ok":true,"id":{}}})"
                     "\n",
                     link_id);
}

auto render_entity_link_text(entity_kind subject_kind, std::int64_t subject_id, const entity_link& link,
                             std::string_view relationship_text) -> std::string {
  // DOUBLE spaces around the `[relationship]` group — the oracle's.
  return std::format("linked {} -> {}  [{}]  (link id: {})\n", ref_text(subject_kind, subject_id),
                     ref_text(link.to_kind, link.to_id), relationship_text, link.id);
}

auto render_entity_link_json(std::string_view subject_id_key, std::int64_t subject_id, const entity_link& link,
                             std::string_view relationship_text) -> std::string {
  return std::format(R"({{"ok":true,"id":{},"{}":{},"to_kind":"{}","to_id":{},"relationship":{}}})"
                     "\n",
                     link.id, subject_id_key, subject_id, entity_kind_to_text(link.to_kind), link.to_id,
                     json_text::json_string(relationship_text));
}

auto render_link_exists_error(entity_kind from_kind, std::int64_t from_id, entity_kind to_kind, std::int64_t to_id,
                              std::string_view relationship_text, bool unicode_arrow) -> std::string {
  return std::format("link {} {} {} [{}] already exists", ref_text(from_kind, from_id), unicode_arrow ? "→" : "->",
                     ref_text(to_kind, to_id), relationship_text);
}

} // namespace planar::engine::entitylink
