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
  std::int64_t issue_counter_ = 0;
  std::size_t  creates_       = 0;
  std::size_t  links_         = 0;
  std::size_t  comments_      = 0;
  std::size_t  probes_        = 0;
  bool         probe_supported_ = true;

  auto probe(std::string_view, std::string_view) -> std::expected<void, parent_issue::gh_client_error> override {
    ++probes_;
    if (!probe_supported_) {
      return std::unexpected(parent_issue::gh_client_error::not_found);
    }
    return {};
  }

  auto create_issue(std::string_view, std::string_view, std::string_view, std::string_view, std::span<const std::string>)
      -> std::expected<parent_issue::created_issue, parent_issue::gh_client_error> override {
    ++issue_counter_;
    ++creates_;
    return parent_issue::created_issue{.number = issue_counter_, .node_id = std::format("N{}", issue_counter_)};
  }

  auto link_sub_issue(std::string_view, std::string_view, std::int64_t, std::int64_t)
      -> std::expected<void, parent_issue::gh_client_error> override {
    ++links_;
    return {};
  }

  auto post_comment(std::string_view, std::string_view) -> std::expected<void, parent_issue::gh_client_error> override {
    ++comments_;
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
  auto        rep = parent_issue::propagate_parent_issue_with_repo(
      conn, fake, anchor_id, "acme", "checkout",
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
  auto        rep2 = parent_issue::propagate_parent_issue_with_repo(
      conn, fake2, anchor_id, "acme", "checkout",
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
      conn, fake, anchor_id, "acme", "x",
      parent_issue::opts{.sys_id = system_id, .sys_slug = "gh", .dry_run = true});
  REQUIRE_FALSE(err(rep).has_value());
  CHECK(fake.creates_ == 0);
  CHECK(rep->created == 2);

  auto stmt = conn.prepare("select count(*) from external_links");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->step().has_value());
  CHECK(stmt->column_int64(0) == 0);
}

TEST_CASE("propagate_parent_issue_with_repo returns sub_issue_unsupported when probe 404s",
         "[engine][external][parent_issue]") {
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
  auto rep3 = parent_issue::propagate_parent_issue_with_repo(conn, fake3, anchor_id, "acme", "checkout", opts_val);
  REQUIRE_FALSE(err(rep3).has_value());
  CHECK(fake3.probes_ == 0);
  CHECK(rep3->skipped == 1);
}
