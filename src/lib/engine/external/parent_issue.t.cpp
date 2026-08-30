// @file parent_issue.t.cpp
// @brief Unit tests for `planar.engine.external.parent_issue` (plan 996,
// task 6353). Ports every `test` block from
// `zig/src/engine/extsync/parent_issue.zig`, driven the same way the
// oracle drives them: a stub `gh_client` implementation with no network
// edge (`fake_client` below mirrors the oracle's `FakeClient`), never a
// real HTTP fixture server — this module's whole point is that its
// orchestration is provably network-agnostic, so a socket here would
// prove nothing a call-count assertion doesn't already prove.
//
// The "second run, all skipped, zero client calls" case in
// "propagate_parent_issue_with_repo creates anchor + child + tasks..."
// below IS this task's request-log evidence: it is the engine-layer
// equivalent of asserting an empty fixture-server request log, because
// `fake_client`'s `creates_`/`links_` counters are incremented at exactly
// the call sites a real adapter's HTTP methods would be. See this file's
// TEST_CASE of that name for the exact assertion, and the task report for
// why a real `std::http` fixture server is not reachable at this layer
// yet (no `gh_client` implementation is wired to a real adapter).

#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.db.migrate;
import planar.engine.external;

#include "scratch_db.hpp"

namespace {

namespace parent_issue = planar::engine::external::parent_issue;
namespace link         = planar::engine::external::link;
namespace testing      = planar::engine::external::testing;

using testing::err;
using testing::open_migrated;
using testing::scratch_db_path;

auto insert_system(planar::db::connection& conn, std::string_view slug) -> std::int64_t {
  auto stmt = conn.prepare("insert into external_systems (kind, slug, default_project, auth_method, auth_ref) "
                           "values ('github-issues', ?, 'owner/repo', 'gh-cli', '') returning id");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->bind_text(1, slug).has_value());
  auto step = stmt->step();
  REQUIRE(step.has_value());
  REQUIRE(*step == planar::db::step_result::row);
  return stmt->column_int64(0);
}

auto insert_plan(planar::db::connection& conn, std::string_view title, std::string_view slug,
                 std::optional<std::int64_t> parent_plan_id = std::nullopt) -> std::int64_t {
  std::string sql = "insert into plans (scope_kind, title, slug";
  if (parent_plan_id.has_value()) {
    sql += ", parent_plan_id";
  }
  sql += ") values ('global', ?, ?";
  if (parent_plan_id.has_value()) {
    sql += ", ?";
  }
  sql += ") returning id";
  auto stmt = conn.prepare(sql);
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->bind_text(1, title).has_value());
  REQUIRE(stmt->bind_text(2, slug).has_value());
  if (parent_plan_id.has_value()) {
    REQUIRE(stmt->bind_int64(3, *parent_plan_id).has_value());
  }
  auto step = stmt->step();
  REQUIRE(step.has_value());
  REQUIRE(*step == planar::db::step_result::row);
  return stmt->column_int64(0);
}

auto insert_task_under_plan(planar::db::connection& conn, std::string_view title, std::int64_t plan_id) -> std::int64_t {
  auto stmt = conn.prepare("insert into tasks (scope_kind, title, plan_id) values ('global', ?, ?) returning id");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->bind_text(1, title).has_value());
  REQUIRE(stmt->bind_int64(2, plan_id).has_value());
  auto step = stmt->step();
  REQUIRE(step.has_value());
  REQUIRE(*step == planar::db::step_result::row);
  return stmt->column_int64(0);
}

/// @brief `entity_links(from_kind='task', to_kind='repo', relationship='touches')`
/// — the edge `distinct_repos_in_feature_ids`'s second CTE union arm reads.
/// Raw SQL rather than through `planar.engine.entitylink`: that bucket is a
/// layer-2 peer of `engine_external` and D18 forbids the edge (same
/// constraint `scratch_db.hpp`'s own `insert_task` documents).
auto insert_touches_edge(planar::db::connection& conn, std::int64_t task_id, std::int64_t project_id) -> void {
  auto stmt = conn.prepare("insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) "
                           "values ('task', ?, 'repo', ?, 'touches')");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->bind_int64(1, task_id).has_value());
  REQUIRE(stmt->bind_int64(2, project_id).has_value());
  auto step = stmt->step();
  REQUIRE(step.has_value());
}

/// @brief `entity_links(from_kind='task', to_kind='plan', relationship='derives-from')`
/// — the edge `tasks_under_plan`'s SECOND CTE union arm reads. Same D18
/// rationale as `insert_touches_edge` above for going through raw SQL.
auto insert_task_derives_from_plan(planar::db::connection& conn, std::int64_t task_id, std::int64_t plan_id) -> void {
  auto stmt = conn.prepare("insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) "
                           "values ('task', ?, 'plan', ?, 'derives-from')");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->bind_int64(1, task_id).has_value());
  REQUIRE(stmt->bind_int64(2, plan_id).has_value());
  auto step = stmt->step();
  REQUIRE(step.has_value());
}

/// @brief A task reachable ONLY via a `-derives-from-> plan_id` edge —
/// `plan_id` is left SQL NULL (`scope_kind='global'` requires it), so the
/// first CTE union arm (`tasks.plan_id = ?`) cannot find it. Only the
/// second arm, joining through `entity_links`, can.
auto insert_task_derives_from_only(planar::db::connection& conn, std::string_view title, std::int64_t plan_id) -> std::int64_t {
  auto stmt = conn.prepare("insert into tasks (scope_kind, title, plan_id) values ('global', ?, NULL) returning id");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->bind_text(1, title).has_value());
  auto step = stmt->step();
  REQUIRE(step.has_value());
  REQUIRE(*step == planar::db::step_result::row);
  auto const task_id = stmt->column_int64(0);
  insert_task_derives_from_plan(conn, task_id, plan_id);
  return task_id;
}

/// @brief A decision `-derives-from-> plan_id`, the edge
/// `post_decision_comments` reads.
auto insert_decision_derives_from(planar::db::connection& conn, std::int64_t plan_id, std::string_view title,
                                  std::string_view body) -> std::int64_t {
  auto stmt = conn.prepare("insert into decisions (scope_kind, title, body, status) "
                           "values ('global', ?, ?, 'proposed') returning id");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->bind_text(1, title).has_value());
  REQUIRE(stmt->bind_text(2, body).has_value());
  auto step = stmt->step();
  REQUIRE(step.has_value());
  REQUIRE(*step == planar::db::step_result::row);
  auto const decision_id = stmt->column_int64(0);

  auto link_stmt = conn.prepare("insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) "
                                "values ('decision', ?, 'plan', ?, 'derives-from')");
  REQUIRE(link_stmt.has_value());
  REQUIRE(link_stmt->bind_int64(1, decision_id).has_value());
  REQUIRE(link_stmt->bind_int64(2, plan_id).has_value());
  REQUIRE(link_stmt->step().has_value());
  return decision_id;
}

auto insert_project(planar::db::connection& conn, std::string_view slug, std::string_view git_remote = "") -> std::int64_t {
  auto stmt = conn.prepare("insert into projects (slug, name, git_remote) values (?, ?, ?) returning id");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->bind_text(1, slug).has_value());
  REQUIRE(stmt->bind_text(2, slug).has_value());
  if (git_remote.empty()) {
    REQUIRE(stmt->bind_null(3).has_value());
  } else {
    REQUIRE(stmt->bind_text(3, git_remote).has_value());
  }
  auto step = stmt->step();
  REQUIRE(step.has_value());
  REQUIRE(*step == planar::db::step_result::row);
  return stmt->column_int64(0);
}

/// @brief The stub `gh_client`, mirroring the oracle's `FakeClient`: an
/// in-process issue counter with no network edge whatsoever. Its call
/// counters ARE this test file's request log.
class fake_client final : public parent_issue::gh_client {
public:
  std::int64_t issue_counter_   = 0;
  std::size_t  creates_         = 0;
  std::size_t  links_           = 0;
  std::size_t  comments_        = 0;
  std::size_t  probes_          = 0;
  bool         probe_supported_ = true;
  /// @brief One entry per `create_issue` call, in call order — the
  /// title/body actually sent, so tests can pin `entity_for_create`'s
  /// output (M6: which column each entity kind reads, and the empty-body
  /// default) without a real HTTP request to inspect.
  std::vector<std::string> created_titles_;
  std::vector<std::string> created_bodies_;
  /// @brief The `external_id` every `post_comment` call targeted.
  std::vector<std::string> commented_on_;
  /// @brief The rendered comment BODY every `post_comment` call sent, in
  /// call order — pins `post_decision_comments`'s
  /// `"**Decision: {title}**\n\n{body}"` column ORDER (title first, then
  /// body), which nothing checked before R2: `commented_on_` alone only
  /// proves a comment landed on the right issue, not that its two halves
  /// were not swapped.
  std::vector<std::string> comment_bodies_;

  auto probe(std::string_view, std::string_view) -> std::expected<void, parent_issue::gh_client_error> override {
    ++probes_;
    if (!probe_supported_) {
      return std::unexpected(parent_issue::gh_client_error::not_found);
    }
    return {};
  }

  auto create_issue(std::string_view, std::string_view, std::string_view title, std::string_view body,
                    std::span<const std::string>)
      -> std::expected<parent_issue::created_issue, parent_issue::gh_client_error> override {
    ++issue_counter_;
    ++creates_;
    created_titles_.emplace_back(title);
    created_bodies_.emplace_back(body);
    return parent_issue::created_issue{.number = issue_counter_, .node_id = std::format("N{}", issue_counter_)};
  }

  auto link_sub_issue(std::string_view, std::string_view, std::int64_t, std::int64_t)
      -> std::expected<void, parent_issue::gh_client_error> override {
    ++links_;
    return {};
  }

  auto post_comment(std::string_view external_id, std::string_view body)
      -> std::expected<void, parent_issue::gh_client_error> override {
    ++comments_;
    commented_on_.emplace_back(external_id);
    comment_bodies_.emplace_back(body);
    return {};
  }
};

} // namespace

TEST_CASE("parse_github_repo accepts ssh, https, ssh-explicit forms", "[engine][external][parent_issue]") {
  {
    auto p = parent_issue::parse_github_repo("git@github.com:acme/api.git");
    REQUIRE(p.has_value());
    CHECK(p->owner == "acme");
    CHECK(p->repo == "api");
  }
  {
    auto p = parent_issue::parse_github_repo("https://github.com/acme/api");
    REQUIRE(p.has_value());
    CHECK(p->owner == "acme");
    CHECK(p->repo == "api");
  }
  {
    auto p = parent_issue::parse_github_repo("ssh://git@github.com/acme/api.git/");
    REQUIRE(p.has_value());
    CHECK(p->owner == "acme");
    CHECK(p->repo == "api");
  }
  CHECK_FALSE(parent_issue::parse_github_repo("https://gitlab.com/acme/api").has_value());
  CHECK_FALSE(parent_issue::parse_github_repo("").has_value());
  CHECK_FALSE(parent_issue::parse_github_repo("git@github.com:acme").has_value());
  CHECK_FALSE(parent_issue::parse_github_repo("https://github.com/acme/api/extra").has_value());
}

TEST_CASE("project_github_coords prefers git_remote, falls back to slug", "[engine][external][parent_issue]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto const p1 = insert_project(conn, "acme/api", "https://github.com/foo/bar.git");
  {
    auto coords = parent_issue::project_github_coords(conn, p1);
    REQUIRE_FALSE(err(coords).has_value());
    REQUIRE(coords->has_value());
    CHECK((*coords)->owner == "foo");
    CHECK((*coords)->repo == "bar");
  }

  auto const p2 = insert_project(conn, "o2/r2");
  {
    auto coords = parent_issue::project_github_coords(conn, p2);
    REQUIRE_FALSE(err(coords).has_value());
    REQUIRE(coords->has_value());
    CHECK((*coords)->owner == "o2");
    CHECK((*coords)->repo == "r2");
  }
}

TEST_CASE("propagate_parent_issue_with_repo creates anchor + child + tasks, second run skips all",
          "[engine][external][parent_issue]") {
  scratch_db_path scratch;
  auto            conn      = open_migrated(scratch);
  auto const      system_id = insert_system(conn, "gh");

  auto const anchor_id = insert_plan(conn, "anchor", "a");
  auto const child_id  = insert_plan(conn, "child", "c", anchor_id);
  insert_task_under_plan(conn, "t1", child_id);
  insert_task_under_plan(conn, "t2", child_id);

  fake_client fake;
  auto        rep = parent_issue::propagate_parent_issue_with_repo(conn, fake, anchor_id, "acme", "checkout",
                                                                   parent_issue::opts{.sys_id = system_id, .sys_slug = "gh"});
  REQUIRE_FALSE(err(rep).has_value());

  CHECK(rep->created == 4);
  CHECK(rep->skipped == 0);
  CHECK(rep->failed == 0);
  CHECK(fake.creates_ == 4);
  // 1 (child->anchor) + 2 (tasks->child) = 3 sub-issue link calls.
  CHECK(fake.links_ == 3);

  {
    auto stmt = conn.prepare("select count(*) from external_links where link_role='mirror'");
    REQUIRE(stmt.has_value());
    REQUIRE(stmt->step().has_value());
    CHECK(stmt->column_int64(0) == 4);
  }
  {
    auto stmt = conn.prepare("select count(*) from sync_events where outcome='ok'");
    REQUIRE(stmt.has_value());
    REQUIRE(stmt->step().has_value());
    CHECK(stmt->column_int64(0) == 4);
  }
  {
    auto stmt = conn.prepare("select config_json from external_links where entity_kind='plan' and entity_id=?");
    REQUIRE(stmt.has_value());
    REQUIRE(stmt->bind_int64(1, anchor_id).has_value());
    REQUIRE(stmt->step().has_value());
    auto const cfg = stmt->column_text(0);
    CHECK(cfg.find(R"("strategy":"github-parent-issue")") != std::string::npos);
    CHECK(cfg.find(R"("parent_issue_repo":"acme/checkout")") != std::string::npos);
    CHECK(cfg.find(R"("parent_issue_num":1)") != std::string::npos);
  }

  // Second run: all 4 skipped, and — this IS the request-log evidence —
  // the fake client's call counters do not move AT ALL. Zero requests on
  // an already-linked entity, the same shape `ext propagate-one`
  // implements (see this file's header).
  fake_client fake2;
  auto        rep2 = parent_issue::propagate_parent_issue_with_repo(conn, fake2, anchor_id, "acme", "checkout",
                                                                    parent_issue::opts{.sys_id = system_id, .sys_slug = "gh"});
  REQUIRE_FALSE(err(rep2).has_value());
  CHECK(rep2->created == 0);
  CHECK(rep2->skipped == 4);
  CHECK(fake2.creates_ == 0);
  CHECK(fake2.links_ == 0);
}

TEST_CASE("propagate_parent_issue_with_repo dry_run does not contact remote and creates no links",
          "[engine][external][parent_issue]") {
  scratch_db_path scratch;
  auto            conn      = open_migrated(scratch);
  auto const      system_id = insert_system(conn, "gh");

  auto const anchor_id = insert_plan(conn, "anchor", "a");
  insert_plan(conn, "child", "c", anchor_id);

  fake_client fake;
  auto        rep = parent_issue::propagate_parent_issue_with_repo(
      conn, fake, anchor_id, "acme", "x", parent_issue::opts{.sys_id = system_id, .sys_slug = "gh", .dry_run = true});
  REQUIRE_FALSE(err(rep).has_value());
  CHECK(fake.creates_ == 0);
  CHECK(rep->created == 2);

  auto stmt = conn.prepare("select count(*) from external_links");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->step().has_value());
  CHECK(stmt->column_int64(0) == 0);
}

TEST_CASE("propagate_parent_issue_with_repo returns sub_issue_unsupported when probe 404s", "[engine][external][parent_issue]") {
  scratch_db_path scratch;
  auto            conn      = open_migrated(scratch);
  auto const      system_id = insert_system(conn, "gh");
  auto const      anchor_id = insert_plan(conn, "anchor", "a");

  fake_client fake;
  fake.probe_supported_ = false;
  auto rep = parent_issue::propagate_parent_issue_with_repo(conn, fake, anchor_id, "o", "r",
                                                            parent_issue::opts{.sys_id = system_id, .sys_slug = "gh"});
  REQUIRE(err(rep).has_value());
  CHECK(*err(rep) == parent_issue::parent_issue_error::sub_issue_unsupported);
}

TEST_CASE("propagate_zero_repo rejects empty and malformed lead_repo", "[engine][external][parent_issue]") {
  scratch_db_path scratch;
  auto            conn      = open_migrated(scratch);
  auto const      system_id = insert_system(conn, "gh");
  auto const      anchor_id = insert_plan(conn, "anchor", "a");

  fake_client fake;
  auto const  o = parent_issue::opts{.sys_id = system_id, .sys_slug = "gh"};

  CHECK(*err(parent_issue::propagate_zero_repo(conn, fake, anchor_id, "", o)) == parent_issue::parent_issue_error::bad_config);
  CHECK(*err(parent_issue::propagate_zero_repo(conn, fake, anchor_id, "no-slash", o)) ==
        parent_issue::parent_issue_error::bad_config);
  CHECK(*err(parent_issue::propagate_zero_repo(conn, fake, anchor_id, "/missing-owner", o)) ==
        parent_issue::parent_issue_error::bad_config);
  CHECK(*err(parent_issue::propagate_zero_repo(conn, fake, anchor_id, "owner/", o)) ==
        parent_issue::parent_issue_error::bad_config);
}

TEST_CASE("detect_parent_issue_support's cache write is a no-op until the anchor link row exists, "
          "so it takes THREE propagate calls to observe a cache hit",
          "[engine][external][parent_issue]") {
  // This is oracle-derived, not the intuitive two-call shape (see this
  // file's header and CLAUDE.md's "derive from the oracle" warning).
  // `write_sub_issue_support_cache` is a no-op when the anchor has no
  // `external_links` mirror row yet:
  //
  //   run 1: detect_parent_issue_support probes, then tries to cache —
  //          NO-OP, because the anchor's mirror row does not exist until
  //          `create_or_skip_github_issue`'s `record_mirror_link` call a
  //          few lines later in the SAME run. `probes_` becomes 1.
  //   run 2: the anchor row now exists, but its `config_json` is
  //          `{"strategy":...}` with no `sub_issue_supported` key — that
  //          key was never written on run 1 — so the cache READ misses
  //          again, the probe runs a SECOND time, and THIS write lands
  //          (the row exists now). `probes_` becomes 1 again (a fresh
  //          fake client).
  //   run 3: the cache finally holds `sub_issue_supported`, so the probe
  //          does not run at all — `probes_` stays 0.
  scratch_db_path scratch;
  auto            conn      = open_migrated(scratch);
  auto const      system_id = insert_system(conn, "gh");
  auto const      anchor_id = insert_plan(conn, "anchor", "a");
  auto const      opts_val  = parent_issue::opts{.sys_id = system_id, .sys_slug = "gh"};

  fake_client fake1;
  auto        rep1 = parent_issue::propagate_parent_issue_with_repo(conn, fake1, anchor_id, "acme", "checkout", opts_val);
  REQUIRE_FALSE(err(rep1).has_value());
  CHECK(fake1.probes_ == 1);

  fake_client fake2;
  auto        rep2 = parent_issue::propagate_parent_issue_with_repo(conn, fake2, anchor_id, "acme", "checkout", opts_val);
  REQUIRE_FALSE(err(rep2).has_value());
  CHECK(fake2.probes_ == 1);
  CHECK(rep2->skipped == 1);

  // Now the cache is actually populated: a THIRD run with a probe that
  // would otherwise refuse must still succeed, because the probe never
  // runs at all.
  fake_client fake3;
  fake3.probe_supported_ = false;
  auto rep3              = parent_issue::propagate_parent_issue_with_repo(conn, fake3, anchor_id, "acme", "checkout", opts_val);
  REQUIRE_FALSE(err(rep3).has_value());
  CHECK(fake3.probes_ == 0);
  CHECK(rep3->skipped == 1);

  // M8: write_sub_issue_support_cache's merge must not CLOBBER the
  // strategy fields run 1 wrote — it is a MERGE, keyed on preserving
  // every other object member while only touching `sub_issue_supported`.
  // A mutation that dropped the `merged.object.emplace_back(key, val)`
  // preserve-arm would still leave every run above green (none of them
  // reads `config_json` after run 2's write), so this is the one
  // assertion that actually exercises the preserve path.
  {
    auto stmt = conn.prepare("select config_json from external_links where entity_kind='plan' and entity_id=?");
    REQUIRE(stmt.has_value());
    REQUIRE(stmt->bind_int64(1, anchor_id).has_value());
    REQUIRE(stmt->step().has_value());
    auto const cfg = stmt->column_text(0);
    CHECK(cfg.find(R"("strategy":"github-parent-issue")") != std::string::npos);
    CHECK(cfg.find(R"("parent_issue_repo":"acme/checkout")") != std::string::npos);
    CHECK(cfg.find(R"("sub_issue_supported":true)") != std::string::npos);
  }
}

TEST_CASE("resolve_target_repo and propagate_parent_issue reach a repo ONLY through recursion "
          "into a grandchild plan AND the touches-edge CTE arm",
          "[engine][external][parent_issue]") {
  // Neither `resolve_target_repo` nor `propagate_parent_issue` (the
  // repo-RESOLVING entry point, as opposed to `..._with_repo`) had any
  // test coverage before this case — every other test in this file drives
  // `..._with_repo` directly with a hand-supplied owner/repo, so
  // `distinct_repos_in_feature_ids`'s 28-line recursive CTE (M1/M2) had
  // zero callers anywhere in this suite.
  //
  // The fixture is built so BOTH of the following are load-bearing, not
  // incidental:
  //   - the touched task lives on a GRANDCHILD plan (anchor -> child ->
  //     grandchild), so the CTE's `plan_tree` recursion must run at
  //     least twice. A mutation that truncates the recursion to
  //     `select ?` (M2) finds only the anchor id and misses the task
  //     entirely.
  //   - the task reaches the repo via a `-touches->` entity_links edge,
  //     NOT via `scope_kind='repo'` — it is `scope_kind='global'`. A
  //     mutation that deletes the touches-edge CTE union arm (M1) finds
  //     no repo at all for this task.
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto const project_id = insert_project(conn, "acme/checkout", "https://github.com/acme/checkout.git");

  auto const anchor_id     = insert_plan(conn, "anchor", "a");
  auto const child_id      = insert_plan(conn, "child", "c", anchor_id);
  auto const grandchild_id = insert_plan(conn, "grandchild", "g", child_id);
  auto const task_id       = insert_task_under_plan(conn, "touches the repo", grandchild_id);
  insert_touches_edge(conn, task_id, project_id);

  {
    auto coords = parent_issue::resolve_target_repo(conn, anchor_id);
    REQUIRE_FALSE(err(coords).has_value());
    CHECK(coords->owner == "acme");
    CHECK(coords->repo == "checkout");
  }

  auto const  system_id = insert_system(conn, "gh");
  fake_client fake;
  auto        rep =
      parent_issue::propagate_parent_issue(conn, fake, anchor_id, parent_issue::opts{.sys_id = system_id, .sys_slug = "gh"});
  REQUIRE_FALSE(err(rep).has_value());
  // `propagate_parent_issue_with_repo`'s entity-creation walk (step 2) is
  // NOT the same recursion as the repo-resolution CTE above: it only
  // visits DIRECT children of the anchor and THEIR direct tasks
  // (`child_plans_of` / `tasks_under_plan`, both one level), so the
  // grandchild plan and its task are never turned into issues even
  // though they ARE what the repo resolution found. anchor + child = 2
  // created issues; this assertion is deliberately about the repo
  // resolution succeeding at all (proven above), not about walk depth.
  CHECK(rep->created == 2);
  CHECK(fake.creates_ == 2);
}

TEST_CASE("resolve_target_repo returns no_touched_repos when the anchor touches nothing", "[engine][external][parent_issue]") {
  // The absence case for the M1/M2 fixture above: without a repo-scoped
  // task or a touches edge anywhere in the tree, resolution must fail
  // rather than silently pick something. Pairs the presence assertion
  // with the CLAUDE.md rule that an absence check alone can pass for the
  // wrong reason.
  scratch_db_path scratch;
  auto            conn      = open_migrated(scratch);
  auto const      anchor_id = insert_plan(conn, "anchor", "a");
  insert_plan(conn, "child", "c", anchor_id);

  auto coords = parent_issue::resolve_target_repo(conn, anchor_id);
  REQUIRE(err(coords).has_value());
  CHECK(*err(coords) == parent_issue::parent_issue_error::no_touched_repos);
}

TEST_CASE("propagate_parent_issue_with_repo: direct-anchor tasks, plan summary as issue body, "
          "the empty-body default, and decision comments on the creating run",
          "[engine][external][parent_issue]") {
  scratch_db_path scratch;
  auto            conn      = open_migrated(scratch);
  auto const      system_id = insert_system(conn, "gh");

  auto const anchor_id = insert_plan(conn, "anchor", "a");
  {
    // `insert_plan` never sets `summary`; set it directly here so the
    // anchor's issue body can be pinned against a NON-empty value (M6:
    // `entity_for_create`'s plan arm reads `summary`, never `body` — the
    // `plans` table has no `body` column at all, so a mutation that swaps
    // to it fails the query outright rather than silently substituting
    // the wrong text).
    auto stmt = conn.prepare("update plans set summary = ? where id = ?");
    REQUIRE(stmt.has_value());
    REQUIRE(stmt->bind_text(1, "ANCHOR SUMMARY TEXT").has_value());
    REQUIRE(stmt->bind_int64(2, anchor_id).has_value());
    REQUIRE(stmt->step().has_value());
  }
  // M5: a task attached DIRECTLY to the anchor (plan_id = anchor_id, no
  // intervening child plan) — every other test in this file only exercises
  // tasks reached through a child plan, so the step-3 direct-anchor-tasks
  // loop had zero coverage.
  auto const direct_task_id = insert_task_under_plan(conn, "direct anchor task", anchor_id);
  static_cast<void>(direct_task_id);

  // M3: a decision `-derives-from-> anchor`, the edge `post_decision_comments` reads.
  insert_decision_derives_from(conn, anchor_id, "Use parent-issue strategy", "Because it is simplest for a single repo.");

  fake_client fake;
  auto        rep = parent_issue::propagate_parent_issue_with_repo(conn, fake, anchor_id, "acme", "checkout",
                                                                   parent_issue::opts{.sys_id = system_id, .sys_slug = "gh"});
  REQUIRE_FALSE(err(rep).has_value());

  // anchor + direct task = 2 created issues (no child plans in this fixture).
  CHECK(rep->created == 2);
  REQUIRE(fake.created_titles_.size() == 2);
  REQUIRE(fake.created_bodies_.size() == 2);

  // The anchor is always create_or_skip_github_issue's FIRST call.
  CHECK(fake.created_titles_[0] == "anchor");
  CHECK(fake.created_bodies_[0] == "ANCHOR SUMMARY TEXT");

  // The direct task's title is what was inserted; its body is EMPTY
  // (tasks.body was never set), so it must fall back to the default
  // placeholder rather than sending an empty string.
  CHECK(fake.created_titles_[1] == "direct anchor task");
  CHECK(fake.created_bodies_[1] == "_No description provided._");

  // M3: exactly one comment, posted on the anchor's OWN external_id
  // (`"acme/checkout#1"`, since the anchor is always issue #1 in a
  // fresh fixture) — this run CREATED the anchor, so the
  // `!parent->external_id.empty()` gate in
  // `propagate_parent_issue_with_repo` passes.
  CHECK(fake.comments_ == 1);
  REQUIRE(fake.commented_on_.size() == 1);
  CHECK(fake.commented_on_[0] == "acme/checkout#1");

  // R2: the exact rendered comment, title THEN body — pins
  // `post_decision_comments`' column order against the oracle's
  // `"**Decision: {s}**\n\n{s}"` (parent_issue.zig:938). The two seeded
  // strings are distinct enough that a title/body column swap changes
  // this string rather than leaving it accidentally equal.
  REQUIRE(fake.comment_bodies_.size() == 1);
  CHECK(fake.comment_bodies_[0] == "**Decision: Use parent-issue strategy**\n\nBecause it is simplest for a single repo.");
}

TEST_CASE("propagate_parent_issue_with_repo does NOT re-comment decisions on a re-run that skips the anchor",
          "[engine][external][parent_issue]") {
  // Companion to the case above, pinning the OTHER half of F5's fixed
  // doc comment: decisions are commented on the run that CREATES the
  // anchor issue, never again on a later run that finds it already
  // linked. Every earlier test's `comments_` was 0 because none of them
  // seeded a decision at all; this is the first assertion that a
  // re-run's `comments_` STAYS 0 rather than merely starting there.
  scratch_db_path scratch;
  auto            conn      = open_migrated(scratch);
  auto const      system_id = insert_system(conn, "gh");
  auto const      anchor_id = insert_plan(conn, "anchor", "a");
  insert_decision_derives_from(conn, anchor_id, "A decision", "Body text.");
  auto const opts_val = parent_issue::opts{.sys_id = system_id, .sys_slug = "gh"};

  fake_client fake1;
  auto        rep1 = parent_issue::propagate_parent_issue_with_repo(conn, fake1, anchor_id, "acme", "checkout", opts_val);
  REQUIRE_FALSE(err(rep1).has_value());
  CHECK(fake1.comments_ == 1);

  fake_client fake2;
  auto        rep2 = parent_issue::propagate_parent_issue_with_repo(conn, fake2, anchor_id, "acme", "checkout", opts_val);
  REQUIRE_FALSE(err(rep2).has_value());
  CHECK(rep2->skipped == 1);
  CHECK(fake2.comments_ == 0);
}

TEST_CASE("tasks_under_plan reaches a task via a derives-from edge ALONE, and dedups a task reachable BOTH ways",
          "[engine][external][parent_issue]") {
  // R1: `tasks_under_plan`'s SECOND CTE union arm (the `entity_links`
  // join, oracle-mandated per `parent_issue.zig:694-696` — "tasksUnderPlan
  // returns tasks attached to plan_id via either tasks.plan_id or
  // entity_links(task->plan, derives-from)") had NO coverage anywhere in
  // this suite before this case: `insert_task_under_plan` always sets
  // `plan_id` directly, so every earlier test only ever exercised the
  // FIRST arm. `task -derives-from-> plan` is what `spec ingest` writes
  // in production, so this is a live attachment path `ext propagate`
  // must reach.
  //
  // Three tasks, three shapes:
  //   t1  reachable ONLY via tasks.plan_id            (the arm every
  //       earlier test already covers)
  //   t2  reachable ONLY via the derives-from edge     (plan_id IS NULL —
  //       R1's fix)
  //   t3  reachable via BOTH tasks.plan_id AND a redundant derives-from
  //       edge to the SAME plan.
  //
  // t3's CREATED count alone does NOT pin `union` against `union all`:
  // under `union all`, `tasks_under_plan` would hand t3 back TWICE, but
  // `create_or_skip_github_issue`'s own skip-check absorbs the second
  // occurrence (it finds t3 already linked from the first pass and
  // reports `op::skipped`, not a second create) — so `created_titles_`
  // reads identically either way. What DOES differ is `rep->skipped`:
  // it stays 0 under `union`'s true dedup and becomes 1 under
  // `union all`'s phantom second pass. That is the assertion below that
  // actually discriminates the two, confirmed by break-probe (a first
  // attempt at this fixture asserted only the created-count shape and
  // SURVIVED a `union` -> `union all` mutation for exactly this reason).
  scratch_db_path scratch;
  auto            conn      = open_migrated(scratch);
  auto const      system_id = insert_system(conn, "gh");

  auto const anchor_id = insert_plan(conn, "anchor", "a");
  auto const child_id  = insert_plan(conn, "child", "c", anchor_id);

  insert_task_under_plan(conn, "via plan_id", child_id);
  insert_task_derives_from_only(conn, "via derives-from only", child_id);
  auto const both_id = insert_task_under_plan(conn, "via both paths", child_id);
  insert_task_derives_from_plan(conn, both_id, child_id);

  fake_client fake;
  auto        rep = parent_issue::propagate_parent_issue_with_repo(conn, fake, anchor_id, "acme", "checkout",
                                                                   parent_issue::opts{.sys_id = system_id, .sys_slug = "gh"});
  REQUIRE_FALSE(err(rep).has_value());

  // anchor + child + t1 + t2 + t3 = 5, NOT 6 — t3 is not double-counted.
  CHECK(rep->created == 5);
  CHECK(fake.creates_ == 5);
  REQUIRE(fake.created_titles_.size() == 5);
  // The actual dedup assertion — see this test's header comment on t3.
  CHECK(rep->skipped == 0);
  CHECK(rep->results.size() == 5);

  auto const count = [&](std::string_view title) {
    return std::count(fake.created_titles_.begin(), fake.created_titles_.end(), std::string{title});
  };
  CHECK(count("via plan_id") == 1);
  CHECK(count("via derives-from only") == 1);
  CHECK(count("via both paths") == 1); // the dedup assertion.
}
