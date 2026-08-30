/// @file parent_issue.cpp
/// @brief Implementation of `planar.engine.external.parent_issue`. See
/// parent_issue.cppm for the port scope and the layering rationale.

module planar.engine.external.parent_issue;

import std;
import planar.db;
import planar.engine.external.link;
import planar.json_dom;
import planar.json_text;

namespace planar::engine::external::parent_issue {

namespace {

// ---------------------------------------------------------------------------
// small row types (internal — no caller outside this file needs them yet)
// ---------------------------------------------------------------------------

struct plan_row {
  std::int64_t id = 0;
  std::string  title;
};

struct task_row {
  std::int64_t id = 0;
  std::string  title;
};

/// @brief `childPlansOf` — direct children of the anchor, in id order.
auto child_plans_of(db::connection& conn, std::int64_t anchor_plan_id)
    -> std::expected<std::vector<plan_row>, parent_issue_error> {
  auto stmt = conn.prepare("select id, coalesce(title,'') from plans where parent_plan_id = ? order by id");
  if (!stmt || !stmt->bind_int64(1, anchor_plan_id)) {
    return std::unexpected(parent_issue_error::query_failed);
  }
  std::vector<plan_row> out;
  while (true) {
    auto stepped = stmt->step();
    if (!stepped) {
      return std::unexpected(parent_issue_error::query_failed);
    }
    if (*stepped == db::step_result::done) {
      break;
    }
    out.push_back({.id = stmt->column_int64(0), .title = stmt->column_text(1)});
  }
  return out;
}

/// @brief `tasksUnderPlan` — tasks attached via `tasks.plan_id` OR a
/// `task -derives-from-> plan` entity_links edge, deduplicated by the
/// `union` and ordered by id. Mirrors the oracle exactly, including the
/// dual attachment path.
auto tasks_under_plan(db::connection& conn, std::int64_t plan_id) -> std::expected<std::vector<task_row>, parent_issue_error> {
  auto stmt = conn.prepare("select id, title from ("
                           "  select t.id, coalesce(t.title,'') as title from tasks t where t.plan_id = ?"
                           "  union"
                           "  select t.id, coalesce(t.title,'') as title from tasks t"
                           "  join entity_links el on el.from_kind='task' and el.from_id=t.id"
                           "  where el.to_kind='plan' and el.to_id=? and el.relationship='derives-from'"
                           ") order by id");
  if (!stmt || !stmt->bind_int64(1, plan_id) || !stmt->bind_int64(2, plan_id)) {
    return std::unexpected(parent_issue_error::query_failed);
  }
  std::vector<task_row> out;
  while (true) {
    auto stepped = stmt->step();
    if (!stepped) {
      return std::unexpected(parent_issue_error::query_failed);
    }
    if (*stepped == db::step_result::done) {
      break;
    }
    out.push_back({.id = stmt->column_int64(0), .title = stmt->column_text(1)});
  }
  return out;
}

/// @brief `directTasksOf` — the oracle spells this as a second name for
/// `tasksUnderPlan` called on the anchor itself; this port does the same.
auto direct_tasks_of(db::connection& conn, std::int64_t anchor_plan_id)
    -> std::expected<std::vector<task_row>, parent_issue_error> {
  return tasks_under_plan(conn, anchor_plan_id);
}

struct local_create {
  std::string title;
  std::string body;
};

/// @brief `entityForCreate` — title/body for a `plan` (title, summary) or
/// `task` (title, body) row.
auto entity_for_create(db::connection& conn, std::string_view entity_kind, std::int64_t entity_id)
    -> std::expected<local_create, parent_issue_error> {
  std::string_view sql;
  if (entity_kind == "plan") {
    sql = "select coalesce(title,''), coalesce(summary,'') from plans where id = ?";
  } else if (entity_kind == "task") {
    sql = "select coalesce(title,''), coalesce(body,'') from tasks where id = ?";
  } else {
    return std::unexpected(parent_issue_error::query_failed);
  }
  auto stmt = conn.prepare(sql);
  if (!stmt || !stmt->bind_int64(1, entity_id)) {
    return std::unexpected(parent_issue_error::query_failed);
  }
  auto stepped = stmt->step();
  if (!stepped) {
    return std::unexpected(parent_issue_error::query_failed);
  }
  if (*stepped == db::step_result::done) {
    return std::unexpected(parent_issue_error::query_failed);
  }
  return local_create{.title = stmt->column_text(0), .body = stmt->column_text(1)};
}

/// @brief `entityTitleOpt` — best-effort title lookup used only for the
/// "skipped" result row (an entity that vanished between propagate runs
/// still reports the empty string, matching the oracle's own `orelse ""`).
auto entity_title_opt(db::connection& conn, std::string_view entity_kind, std::int64_t entity_id) -> std::string {
  std::string_view sql;
  if (entity_kind == "plan") {
    sql = "select coalesce(title,'') from plans where id = ?";
  } else if (entity_kind == "task") {
    sql = "select coalesce(title,'') from tasks where id = ?";
  } else {
    return {};
  }
  auto stmt = conn.prepare(sql);
  if (!stmt || !stmt->bind_int64(1, entity_id)) {
    return {};
  }
  auto stepped = stmt->step();
  if (!stepped || *stepped == db::step_result::done) {
    return {};
  }
  return stmt->column_text(0);
}

/// @brief The `external_url` half of an existing mirror link, read
/// separately from `link::load_existing_mirror` (which returns only
/// `external_id` — task 6335's port needed nothing more at the time). The
/// oracle's own `loadExistingMirror` (`parent_issue.zig:582-601`) reads
/// both columns in one query; this is the narrower, second-read
/// alternative the reviewer named rather than widening the shared helper's
/// signature for every other caller.
/// @param conn An open, migrated connection.
/// @param entity_kind The entity kind TEXT, as stored.
/// @param entity_id The entity id.
/// @param system_id The registered system.
/// @return The URL, or the empty string when there is no mirror row or the
/// URL column is SQL NULL.
auto load_existing_mirror_url(db::connection& conn, std::string_view entity_kind, std::int64_t entity_id, std::int64_t system_id)
    -> std::string {
  auto stmt = conn.prepare("select coalesce(external_url, '') from external_links "
                           "where entity_kind = ? and entity_id = ? and system_id = ? and link_role = 'mirror' limit 1");
  if (!stmt || !stmt->bind_text(1, entity_kind) || !stmt->bind_int64(2, entity_id) || !stmt->bind_int64(3, system_id)) {
    return {};
  }
  auto stepped = stmt->step();
  if (!stepped || *stepped == db::step_result::done) {
    return {};
  }
  return stmt->column_text(0);
}

/// @brief `parseIssueNumberFromExternalID` — the number after the last `#`.
auto parse_issue_number_from_external_id(std::string_view external_id) -> std::optional<std::int64_t> {
  auto const hash = external_id.rfind('#');
  if (hash == std::string_view::npos || hash + 1 >= external_id.size()) {
    return std::nullopt;
  }
  std::int64_t value   = 0;
  auto const   digits  = external_id.substr(hash + 1);
  auto const [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), value);
  if (ec != std::errc{} || ptr != digits.data() + digits.size()) {
    return std::nullopt;
  }
  return value;
}

// ---------------------------------------------------------------------------
// sub-issue-support cache
// ---------------------------------------------------------------------------

/// @brief Compact (no whitespace) JSON encoding, used only for the
/// `config_json` cache merge. `stringify_indent2` is the DOM's only public
/// writer and is deliberately not reused here — a two-space-indented blob
/// in a single TEXT column would be a cosmetic parity break against the
/// oracle's `std.json.Stringify` default (compact) output with no
/// observable benefit.
///
/// **RECORDED FOLLOW-UP, not fixed this cycle**: this is a complete
/// second `json_dom::json_value` writer, TU-local to this file, with only
/// M8's indirect survival check exercising it. It duplicates
/// `stringify_indent2`'s node walk (`src/lib/json_dom/json_dom.cpp`) with
/// a different separator strategy. Moving it into `json_dom` itself as
/// `stringify_compact`, sharing one walk with `stringify_indent2` via a
/// whitespace-mode parameter, is the right home — `json_dom` is a
/// layer-1 base library every layer-2 bucket can already reach, and a
/// second bucket needing compact output (any future `config_json` writer)
/// would otherwise be a THIRD copy. Left local for this cycle to keep the
/// diff to `parent_issue.zig`'s own port; promoting it is follow-up work
/// for whichever task next needs compact JSON output outside this file.
auto write_compact(const json_dom::json_value& value, std::string& out) -> void {
  switch (value.kind) {
  case json_dom::json_kind::null_:
    out += "null";
    return;
  case json_dom::json_kind::boolean:
    out += value.boolean ? "true" : "false";
    return;
  case json_dom::json_kind::integer:
    out += std::to_string(value.integer);
    return;
  case json_dom::json_kind::floating:
    out += std::format("{}", value.floating);
    return;
  case json_dom::json_kind::number_raw:
    out += value.string;
    return;
  case json_dom::json_kind::string:
    json_text::append_json_string(out, value.string);
    return;
  case json_dom::json_kind::array:
    out += '[';
    for (std::size_t i = 0; i < value.array.size(); ++i) {
      if (i > 0) {
        out += ',';
      }
      write_compact(value.array[i], out);
    }
    out += ']';
    return;
  case json_dom::json_kind::object:
    out += '{';
    for (std::size_t i = 0; i < value.object.size(); ++i) {
      if (i > 0) {
        out += ',';
      }
      json_text::append_json_string(out, value.object[i].first);
      out += ':';
      write_compact(value.object[i].second, out);
    }
    out += '}';
    return;
  }
}

/// @brief `readSubIssueSupportCache` — the anchor mirror link's
/// `config_json.sub_issue_supported`, when the row and key both exist.
auto read_sub_issue_support_cache(db::connection& conn, std::int64_t anchor_plan_id, std::int64_t system_id)
    -> std::expected<std::optional<bool>, parent_issue_error> {
  auto stmt = conn.prepare("select coalesce(config_json, '') from external_links "
                           "where entity_kind = 'plan' and entity_id = ? and system_id = ? and link_role = 'mirror' limit 1");
  if (!stmt || !stmt->bind_int64(1, anchor_plan_id) || !stmt->bind_int64(2, system_id)) {
    return std::unexpected(parent_issue_error::query_failed);
  }
  auto stepped = stmt->step();
  if (!stepped) {
    return std::unexpected(parent_issue_error::query_failed);
  }
  if (*stepped == db::step_result::done) {
    return std::optional<bool>{std::nullopt};
  }
  auto const raw = stmt->column_text(0);
  if (raw.empty()) {
    return std::optional<bool>{std::nullopt};
  }
  auto parsed = json_dom::parse_json(raw);
  if (!parsed || parsed->kind != json_dom::json_kind::object) {
    return std::optional<bool>{std::nullopt};
  }
  auto const* v = parsed->find("sub_issue_supported");
  if (v == nullptr || v->kind != json_dom::json_kind::boolean) {
    return std::optional<bool>{std::nullopt};
  }
  return std::optional<bool>{v->boolean};
}

/// @brief `writeSubIssueSupportCache` — best-effort merge-write of
/// `sub_issue_supported` into the anchor mirror link's `config_json`,
/// preserving every other key already present. A no-op (not an error) when
/// the row does not exist yet — the very first `create_or_skip_github_issue`
/// call on the anchor writes the strategy cache that establishes it.
///
/// See `detect_parent_issue_support`'s doc comment (parent_issue.cppm) and
/// task 6354 for why this no-op means the cache needs THREE propagate
/// calls, not two, before it ever short-circuits a probe.
auto write_sub_issue_support_cache(db::connection& conn, std::int64_t anchor_plan_id, std::int64_t system_id, bool supported)
    -> void {
  auto stmt = conn.prepare("select coalesce(config_json, '{}') from external_links "
                           "where entity_kind = 'plan' and entity_id = ? and system_id = ? and link_role = 'mirror' limit 1");
  if (!stmt || !stmt->bind_int64(1, anchor_plan_id) || !stmt->bind_int64(2, system_id)) {
    return;
  }
  auto stepped = stmt->step();
  if (!stepped || *stepped == db::step_result::done) {
    return;
  }
  auto const existing = stmt->column_text(0);

  json_dom::json_value merged;
  merged.kind          = json_dom::json_kind::object;
  bool wrote_supported = false;

  auto parsed = json_dom::parse_json(existing);
  if (parsed && parsed->kind == json_dom::json_kind::object) {
    for (auto& [key, val] : parsed->object) {
      if (key == "sub_issue_supported") {
        json_dom::json_value b;
        b.kind    = json_dom::json_kind::boolean;
        b.boolean = supported;
        merged.object.emplace_back("sub_issue_supported", std::move(b));
        wrote_supported = true;
        continue;
      }
      merged.object.emplace_back(key, val);
    }
  }
  if (!wrote_supported) {
    json_dom::json_value b;
    b.kind    = json_dom::json_kind::boolean;
    b.boolean = supported;
    merged.object.emplace_back("sub_issue_supported", std::move(b));
  }

  std::string encoded;
  write_compact(merged, encoded);

  auto write = conn.prepare("update external_links set config_json = ? "
                            "where entity_kind = 'plan' and entity_id = ? and system_id = ? and link_role = 'mirror'");
  if (!write || !write->bind_text(1, encoded) || !write->bind_int64(2, anchor_plan_id) || !write->bind_int64(3, system_id)) {
    return;
  }
  static_cast<void>(write->step());
}

// ---------------------------------------------------------------------------
// createOrSkipGithubIssue — the core per-entity primitive
// ---------------------------------------------------------------------------

struct per_entity_result {
  std::int64_t number = 0;
  std::string  external_id;
};

/// @brief `createOrSkipGithubIssue` — create or skip a regular/sub-issue for
/// `entity_kind:entity_id`, appending exactly one row to `results`.
auto create_or_skip_github_issue(db::connection& conn, gh_client& client, const opts& options, std::string_view entity_kind,
                                 std::int64_t entity_id, std::int64_t parent_number, std::string_view owner,
                                 std::string_view repo, std::vector<entity_result>& results)
    -> std::expected<per_entity_result, parent_issue_error> {
  // Skip if already linked.
  auto existing = link::load_existing_mirror(conn, entity_kind, entity_id, options.sys_id);
  if (!existing) {
    return std::unexpected(parent_issue_error::query_failed);
  }
  if (!existing->empty()) {
    auto const title = entity_title_opt(conn, entity_kind, entity_id);
    auto const url   = load_existing_mirror_url(conn, entity_kind, entity_id, options.sys_id);
    results.push_back({
        .entity_kind  = std::string{entity_kind},
        .entity_id    = entity_id,
        .title        = title,
        .operation    = op::skipped,
        .external_id  = *existing,
        .external_url = url,
        .error_name   = {},
    });
    auto const num = parse_issue_number_from_external_id(*existing);
    return per_entity_result{.number = num.value_or(0), .external_id = {}};
  }

  auto local = entity_for_create(conn, entity_kind, entity_id);
  if (!local) {
    results.push_back({
        .entity_kind = std::string{entity_kind},
        .entity_id   = entity_id,
        .title       = {},
        .operation   = op::failed,
        .external_id = {},
        // "QueryFailed" (PascalCase), matching the oracle's `@errorName`
        // for this path: `entityForCreate` returns only `Error.QueryFailed`.
        .error_name = "QueryFailed",
    });
    return std::unexpected(parent_issue_error::query_failed);
  }

  if (options.dry_run) {
    results.push_back({
        .entity_kind  = std::string{entity_kind},
        .entity_id    = entity_id,
        .title        = local->title,
        .operation    = op::created,
        .external_id  = "(dry-run)",
        .external_url = {},
        .error_name   = {},
    });
    return per_entity_result{.number = 0, .external_id = {}};
  }

  std::string_view const body_or_default = local->body.empty() ? std::string_view{"_No description provided._"} : local->body;
  std::array<std::string, 0> const no_labels{};
  auto                             created = client.create_issue(owner, repo, local->title, body_or_default, no_labels);
  if (!created) {
    results.push_back({
        .entity_kind = std::string{entity_kind},
        .entity_id   = entity_id,
        .title       = local->title,
        .operation   = op::failed,
        .external_id = {},
        .error_name  = created.error() == gh_client_error::not_found ? "not_found" : "gh_client_error",
    });
    return std::unexpected(parent_issue_error::query_failed);
  }

  if (parent_number > 0) {
    // Non-fatal: the issue was already created. Mirrors the oracle's
    // warn-and-continue.
    static_cast<void>(client.link_sub_issue(owner, repo, parent_number, created->number));
  }

  auto const external_id  = std::format("{}/{}#{}", owner, repo, created->number);
  auto const external_url = std::format("https://github.com/{}/{}/issues/{}", owner, repo, created->number);

  auto recorded =
      link::record_mirror_link(conn, entity_kind, entity_id, options.sys_id, external_id, external_url, options.sync_direction_);
  if (!recorded) {
    return std::unexpected(parent_issue_error::query_failed);
  }

  results.push_back({
      .entity_kind  = std::string{entity_kind},
      .entity_id    = entity_id,
      .title        = local->title,
      .operation    = op::created,
      .external_id  = external_id,
      .external_url = external_url,
      .error_name   = {},
  });
  return per_entity_result{.number = created->number, .external_id = external_id};
}

// ---------------------------------------------------------------------------
// decision comments
// ---------------------------------------------------------------------------

/// @brief `postDecisionComments` — every decision `derives-from` the anchor,
/// posted as a comment on the parent issue. Errors are non-fatal.
auto post_decision_comments(db::connection& conn, gh_client& client, std::int64_t anchor_plan_id,
                            std::string_view parent_external_id) -> void {
  auto stmt = conn.prepare("select coalesce(d.title,''), coalesce(d.body,'') "
                           "from decisions d "
                           "join entity_links el on el.from_kind = 'decision' and el.from_id = d.id "
                           "                     and el.to_kind = 'plan' and el.to_id = ? "
                           "                     and el.relationship = 'derives-from' "
                           "order by d.id");
  if (!stmt || !stmt->bind_int64(1, anchor_plan_id)) {
    return;
  }
  while (true) {
    auto stepped = stmt->step();
    if (!stepped || *stepped == db::step_result::done) {
      return;
    }
    auto const title   = stmt->column_text(0);
    auto const body    = stmt->column_text(1);
    auto const comment = std::format("**Decision: {}**\n\n{}", title, body);
    static_cast<void>(client.post_comment(parent_external_id, comment));
  }
}

// ---------------------------------------------------------------------------
// repo resolution
// ---------------------------------------------------------------------------

/// @brief `distinctReposInFeatureIds` — the same recursive CTE
/// `ext propagate`'s strategy selector walks: every distinct `repo` scope
/// touched by the anchor plan's own tasks, its descendant plans' tasks, or
/// any task's `-touches->` entity_links edge to a repo.
auto distinct_repos_in_feature_ids(db::connection& conn, std::int64_t anchor_plan_id)
    -> std::expected<std::vector<std::int64_t>, parent_issue_error> {
  auto stmt = conn.prepare("with recursive plan_tree(id) as ("
                           "  select ? union all"
                           "  select p.id from plans p join plan_tree pt on p.parent_plan_id = pt.id"
                           "),"
                           "tasks_in_tree as ("
                           "  select t.id, t.scope_kind, t.scope_id"
                           "  from tasks t join plan_tree pt on t.plan_id = pt.id"
                           "  union"
                           "  select t.id, t.scope_kind, t.scope_id"
                           "  from tasks t"
                           "  join entity_links el on el.from_kind = 'task' and el.from_id = t.id"
                           "                       and el.to_kind = 'plan' and el.relationship = 'derives-from'"
                           "  join plan_tree pt on el.to_id = pt.id"
                           "),"
                           "task_repos as ("
                           "  select scope_id as repo_id from tasks_in_tree where scope_kind = 'repo'"
                           "  union"
                           "  select el2.to_id as repo_id"
                           "  from tasks_in_tree tit"
                           "  join entity_links el2 on el2.from_kind = 'task' and el2.from_id = tit.id"
                           "                        and el2.to_kind = 'repo' and el2.relationship = 'touches'"
                           ")"
                           "select distinct repo_id from task_repos where repo_id is not null order by repo_id");
  if (!stmt || !stmt->bind_int64(1, anchor_plan_id)) {
    return std::unexpected(parent_issue_error::query_failed);
  }
  std::vector<std::int64_t> out;
  while (true) {
    auto stepped = stmt->step();
    if (!stepped) {
      return std::unexpected(parent_issue_error::query_failed);
    }
    if (*stepped == db::step_result::done) {
      break;
    }
    out.push_back(stmt->column_int64(0));
  }
  return out;
}

} // namespace

auto parse_github_repo(std::string_view git_remote) -> std::optional<repo_coords> {
  if (git_remote.empty()) {
    return std::nullopt;
  }
  std::string_view path;
  if (git_remote.starts_with("git@github.com:")) {
    path = git_remote.substr(std::string_view{"git@github.com:"}.size());
  } else if (git_remote.starts_with("https://github.com/")) {
    path = git_remote.substr(std::string_view{"https://github.com/"}.size());
  } else if (git_remote.starts_with("ssh://git@github.com/")) {
    path = git_remote.substr(std::string_view{"ssh://git@github.com/"}.size());
  } else {
    return std::nullopt;
  }
  while (!path.empty() && path.back() == '/') {
    path.remove_suffix(1);
  }
  if (path.ends_with(".git")) {
    path.remove_suffix(4);
  }
  auto const slash = path.find('/');
  if (slash == std::string_view::npos || slash == 0 || slash + 1 >= path.size()) {
    return std::nullopt;
  }
  auto const owner = path.substr(0, slash);
  auto const repo  = path.substr(slash + 1);
  if (owner.empty() || repo.empty()) {
    return std::nullopt;
  }
  if (repo.find('/') != std::string_view::npos) {
    return std::nullopt;
  }
  return repo_coords{.owner = std::string{owner}, .repo = std::string{repo}};
}

auto project_github_coords(db::connection& conn, std::int64_t project_id)
    -> std::expected<std::optional<repo_coords>, parent_issue_error> {
  auto stmt = conn.prepare("select slug, coalesce(git_remote,'') from projects where id = ?");
  if (!stmt || !stmt->bind_int64(1, project_id)) {
    return std::unexpected(parent_issue_error::query_failed);
  }
  auto stepped = stmt->step();
  if (!stepped) {
    return std::unexpected(parent_issue_error::query_failed);
  }
  if (*stepped == db::step_result::done) {
    return std::optional<repo_coords>{std::nullopt};
  }
  auto const slug       = stmt->column_text(0);
  auto const git_remote = stmt->column_text(1);

  if (auto parsed = parse_github_repo(git_remote); parsed.has_value()) {
    return std::optional<repo_coords>{*parsed};
  }

  auto const slash = slug.find('/');
  if (slash != std::string::npos) {
    auto const o = std::string_view{slug}.substr(0, slash);
    auto const r = std::string_view{slug}.substr(slash + 1);
    if (!o.empty() && !r.empty() && r.find('/') == std::string_view::npos) {
      return std::optional<repo_coords>{repo_coords{.owner = std::string{o}, .repo = std::string{r}}};
    }
  }
  return std::optional<repo_coords>{std::nullopt};
}

auto resolve_target_repo(db::connection& conn, std::int64_t anchor_plan_id) -> std::expected<repo_coords, parent_issue_error> {
  auto repos = distinct_repos_in_feature_ids(conn, anchor_plan_id);
  if (!repos) {
    return std::unexpected(repos.error());
  }
  if (repos->empty()) {
    return std::unexpected(parent_issue_error::no_touched_repos);
  }
  auto coords = project_github_coords(conn, repos->front());
  if (!coords) {
    return std::unexpected(coords.error());
  }
  if (!coords->has_value()) {
    return std::unexpected(parent_issue_error::cannot_resolve_repo);
  }
  return **coords;
}

auto detect_parent_issue_support(db::connection& conn, gh_client& client, std::string_view owner, std::string_view repo,
                                 std::int64_t anchor_plan_id, std::int64_t system_id) -> std::expected<bool, parent_issue_error> {
  auto cached = read_sub_issue_support_cache(conn, anchor_plan_id, system_id);
  if (!cached) {
    return std::unexpected(cached.error());
  }
  if (cached->has_value()) {
    return **cached;
  }

  bool supported = true;
  auto probed    = client.probe(owner, repo);
  if (!probed && probed.error() == gh_client_error::not_found) {
    supported = false;
  }
  // Any other error (including success) leaves `supported == true` —
  // oracle behavior, see this function's doc comment.

  write_sub_issue_support_cache(conn, anchor_plan_id, system_id, supported);
  return supported;
}

auto propagate_parent_issue_with_repo(db::connection& conn, gh_client& client, std::int64_t anchor_plan_id,
                                      std::string_view owner, std::string_view repo, const opts& options)
    -> std::expected<report, parent_issue_error> {
  report                     rep;
  std::vector<entity_result> results;

  if (!options.dry_run) {
    auto supported = detect_parent_issue_support(conn, client, owner, repo, anchor_plan_id, options.sys_id);
    if (!supported) {
      return std::unexpected(supported.error());
    }
    if (!*supported) {
      return std::unexpected(parent_issue_error::sub_issue_unsupported);
    }
  }

  // ---- step 1: create or skip the anchor parent issue ----
  auto parent = create_or_skip_github_issue(conn, client, options, "plan", anchor_plan_id, 0, owner, repo, results);
  if (!parent) {
    return std::unexpected(parent.error());
  }

  if (!options.dry_run && !parent->external_id.empty()) {
    auto const cfg  = std::format(R"({{"strategy":"github-parent-issue","parent_issue_repo":"{}/{}","parent_issue_num":{}}})",
                                  owner, repo, parent->number);
    auto       stmt = conn.prepare("update external_links set config_json = ? "
                                   "where entity_kind = 'plan' and entity_id = ? and system_id = ? and link_role = 'mirror'");
    if (!stmt || !stmt->bind_text(1, cfg) || !stmt->bind_int64(2, anchor_plan_id) || !stmt->bind_int64(3, options.sys_id) ||
        !stmt->step()) {
      return std::unexpected(parent_issue_error::query_failed);
    }
  }

  // ---- step 2: child plans -> sub-issues; tasks -> sub-issues of child ----
  auto child_plans = child_plans_of(conn, anchor_plan_id);
  if (!child_plans) {
    return std::unexpected(child_plans.error());
  }
  for (auto const& cp : *child_plans) {
    auto cp_res = create_or_skip_github_issue(conn, client, options, "plan", cp.id, parent->number, owner, repo, results);
    if (!cp_res) {
      // Failure already recorded in results; continue with siblings.
      continue;
    }
    auto tasks = tasks_under_plan(conn, cp.id);
    if (!tasks) {
      return std::unexpected(tasks.error());
    }
    for (auto const& t : *tasks) {
      static_cast<void>(create_or_skip_github_issue(conn, client, options, "task", t.id, cp_res->number, owner, repo, results));
    }
  }

  // ---- step 3: direct anchor tasks -> sub-issues of parent ----
  auto direct = direct_tasks_of(conn, anchor_plan_id);
  if (!direct) {
    return std::unexpected(direct.error());
  }
  for (auto const& t : *direct) {
    static_cast<void>(create_or_skip_github_issue(conn, client, options, "task", t.id, parent->number, owner, repo, results));
  }

  // ---- step 4: decisions -> comments on parent (best-effort) ----
  if (!options.dry_run && !parent->external_id.empty()) {
    post_decision_comments(conn, client, anchor_plan_id, parent->external_id);
  }

  for (auto const& r : results) {
    switch (r.operation) {
    case op::created:
      ++rep.created;
      break;
    case op::skipped:
      ++rep.skipped;
      break;
    case op::failed:
      ++rep.failed;
      break;
    }
  }
  rep.results = std::move(results);
  return rep;
}

auto propagate_parent_issue(db::connection& conn, gh_client& client, std::int64_t anchor_plan_id, const opts& options)
    -> std::expected<report, parent_issue_error> {
  auto target = resolve_target_repo(conn, anchor_plan_id);
  if (!target) {
    return std::unexpected(target.error());
  }
  return propagate_parent_issue_with_repo(conn, client, anchor_plan_id, target->owner, target->repo, options);
}

auto propagate_zero_repo(db::connection& conn, gh_client& client, std::int64_t anchor_plan_id, std::string_view lead_repo,
                         const opts& options) -> std::expected<report, parent_issue_error> {
  if (lead_repo.empty()) {
    return std::unexpected(parent_issue_error::bad_config);
  }
  auto const slash = lead_repo.find('/');
  if (slash == std::string_view::npos || slash == 0 || slash + 1 >= lead_repo.size()) {
    return std::unexpected(parent_issue_error::bad_config);
  }
  auto const owner = lead_repo.substr(0, slash);
  auto const repo  = lead_repo.substr(slash + 1);
  if (owner.empty() || repo.empty()) {
    return std::unexpected(parent_issue_error::bad_config);
  }
  return propagate_parent_issue_with_repo(conn, client, anchor_plan_id, owner, repo, options);
}

} // namespace planar::engine::external::parent_issue
