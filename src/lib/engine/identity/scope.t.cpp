// @file scope.t.cpp
// @brief Unit tests for `planar.engine.identity.scope` (plan 996, task
// cpp-scope-assoc). Exercises cwd-derived scope resolution, the
// `--scope` override precedence, the cross-scope guard's refusal
// decision (including the `--no-scope-check` bypass), and the
// `resolve_slug`/`slug_from_ref` scope-ref plumbing against a real,
// migrated on-disk SQLite database.
//
// Include-before-import is deliberate (see db/db.t.cpp): MSVC's supported
// direction for mixing textual std headers with IFC imports is
// include-then-import, not the reverse.
#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.db.migrate;
import planar.engine.identity.scope;

namespace {

using planar::engine::identity::check_scope_guard;
using planar::engine::identity::derive_from_cwd;
using planar::engine::identity::derive_reason;
using planar::engine::identity::guard_write;
using planar::engine::identity::meta_write_resolution;
using planar::engine::identity::resolve_for_write;
using planar::engine::identity::resolve_meta_workspace_write_scope;
using planar::engine::identity::resolve_slug;
using planar::engine::identity::scope_error;
using planar::engine::identity::scope_kind;
using planar::engine::identity::slug_from_ref;

/// @brief A unique scratch database path, removed (best-effort, including
/// SQLite's sidecar files) when the guard goes out of scope. Mirrors
/// db.t.cpp / migrate.t.cpp's identical helper — duplicated rather than
/// shared because this codebase has no header tree for first-party code.
struct scratch_db_path {
  std::filesystem::path path_;

  scratch_db_path()
      : path_(std::filesystem::temp_directory_path() / std::format("planar_identity_scope_test_{}_{}.db",
                                                                   std::chrono::steady_clock::now().time_since_epoch().count(),
                                                                   reinterpret_cast<std::uintptr_t>(this))) {
  }

  scratch_db_path(const scratch_db_path&)            = delete;
  scratch_db_path& operator=(const scratch_db_path&) = delete;

  ~scratch_db_path() {
    std::error_code ec;
    std::filesystem::remove(path_, ec);
    std::filesystem::remove(path_.string() + "-journal", ec);
    std::filesystem::remove(path_.string() + "-wal", ec);
    std::filesystem::remove(path_.string() + "-shm", ec);
  }
};

/// @brief Opens a fresh scratch database and applies every migration.
auto open_migrated(const scratch_db_path& scratch) -> planar::db::connection {
  auto conn = planar::db::connection::open(scratch.path_.string());
  REQUIRE(conn.has_value());
  auto applied = planar::db::apply_all(*conn);
  REQUIRE(applied.has_value());
  return std::move(*conn);
}

auto insert_project(planar::db::connection& conn, std::string_view slug, std::string_view root_path) -> std::int64_t {
  auto stmt = conn.prepare("insert into projects (slug, name, root_path) values (?, ?, ?) returning id");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->bind_text(1, slug).has_value());
  REQUIRE(stmt->bind_text(2, slug).has_value());
  REQUIRE(stmt->bind_text(3, root_path).has_value());
  auto step = stmt->step();
  REQUIRE(step.has_value());
  REQUIRE(*step == planar::db::step_result::row);
  return stmt->column_int64(0);
}

auto insert_association(planar::db::connection& conn, std::string_view slug) -> std::int64_t {
  auto stmt = conn.prepare("insert into associations (slug, name, kind) values (?, ?, 'org') returning id");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->bind_text(1, slug).has_value());
  REQUIRE(stmt->bind_text(2, slug).has_value());
  auto step = stmt->step();
  REQUIRE(step.has_value());
  REQUIRE(*step == planar::db::step_result::row);
  return stmt->column_int64(0);
}

auto link_project_association(planar::db::connection& conn, std::int64_t project_id, std::int64_t assoc_id) -> void {
  auto stmt = conn.prepare("insert into project_associations (project_id, association_id, source) values (?, ?, 'user')");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->bind_int64(1, project_id).has_value());
  REQUIRE(stmt->bind_int64(2, assoc_id).has_value());
  auto step = stmt->step();
  REQUIRE(step.has_value());
}

/// @brief Mark an association as a meta workspace rooted at `root_path`.
///
/// This is the registration `workspace init` will perform. It is done here
/// with raw SQL for the reason task 6134 exists: no verb in this tree can
/// create one yet, which is precisely why the missing arm was UNREACHABLE
/// and therefore invisible. Seeding the row directly makes it reachable now
/// instead of the day the verb lands.
/// @param conn The connection.
/// @param assoc_id The association row.
/// @param root_path The workspace root.
auto mark_meta_workspace(planar::db::connection& conn, std::int64_t assoc_id, std::string_view root_path) -> void {
  auto stmt = conn.prepare("update associations set config_json = json_object('workspace_shape', 'meta-repo', "
                           "'root_path', ?) where id = ?");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->bind_text(1, root_path).has_value());
  REQUIRE(stmt->bind_int64(2, assoc_id).has_value());
  auto step = stmt->step();
  REQUIRE(step.has_value());
}

} // namespace

// --- derive_from_cwd ---------------------------------------------------

TEST_CASE("derive_from_cwd resolves the right scope for a single-association project (cwd derivation)", "[scope][derive]") {
  scratch_db_path scratch;
  auto            conn       = open_migrated(scratch);
  const auto      project_id = insert_project(conn, "myrepo", "/work/myrepo");
  const auto      assoc_id   = insert_association(conn, "myorg");
  link_project_association(conn, project_id, assoc_id);

  auto res = derive_from_cwd(conn, "/work/myrepo/src/foo");
  REQUIRE(res.has_value());
  CHECK(res->reason == derive_reason::project_single_association);
  REQUIRE(res->scope.has_value());
  CHECK(*res->scope == "myorg");
  REQUIRE(res->project_slug.has_value());
  CHECK(*res->project_slug == "myrepo");
}

TEST_CASE("derive_from_cwd: longest-prefix project wins over a shorter parent match", "[scope][derive]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  const auto parent_id  = insert_project(conn, "parent", "/work");
  const auto child_id   = insert_project(conn, "child", "/work/child");
  const auto parent_org = insert_association(conn, "parent-org");
  const auto child_org  = insert_association(conn, "child-org");
  link_project_association(conn, parent_id, parent_org);
  link_project_association(conn, child_id, child_org);

  auto res = derive_from_cwd(conn, "/work/child/src");
  REQUIRE(res.has_value());
  REQUIRE(res->scope.has_value());
  CHECK(*res->scope == "child-org");
  CHECK(*res->project_slug == "child");
}

TEST_CASE("derive_from_cwd: zero associations yields unset scope with project_unassociated (empty membership case)",
          "[scope][derive][empty]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  insert_project(conn, "lone", "/work/lone");

  auto res = derive_from_cwd(conn, "/work/lone/src");
  REQUIRE(res.has_value());
  CHECK(res->reason == derive_reason::project_unassociated);
  CHECK_FALSE(res->scope.has_value());
  REQUIRE(res->project_slug.has_value());
  CHECK(*res->project_slug == "lone");
}

TEST_CASE("derive_from_cwd: multiple associations yields unset scope with project_multiple_associations", "[scope][derive]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  const auto      pid  = insert_project(conn, "multi", "/work/multi");
  const auto      a1   = insert_association(conn, "a1");
  const auto      a2   = insert_association(conn, "a2");
  link_project_association(conn, pid, a1);
  link_project_association(conn, pid, a2);

  auto res = derive_from_cwd(conn, "/work/multi/lib");
  REQUIRE(res.has_value());
  CHECK(res->reason == derive_reason::project_multiple_associations);
  CHECK_FALSE(res->scope.has_value());
}

TEST_CASE("derive_from_cwd: no registered project at all is the absent/empty cwd case", "[scope][derive][empty]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto res = derive_from_cwd(conn, "/home/nobody/unregistered");
  REQUIRE(res.has_value());
  CHECK(res->reason == derive_reason::no_project_match);
  CHECK_FALSE(res->scope.has_value());
  CHECK_FALSE(res->project_slug.has_value());
}

TEST_CASE("derive_from_cwd: relative path is rejected", "[scope][derive]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto res = derive_from_cwd(conn, "relative/path");
  REQUIRE_FALSE(res.has_value());
  CHECK(res.error() == scope_error::invalid_path);
}

// --- resolve_slug / slug_from_ref ---------------------------------------

TEST_CASE("resolve_slug: 'global' resolves to kind=global with no id", "[scope][resolve_slug]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto ref = resolve_slug(conn, "global");
  REQUIRE(ref.has_value());
  CHECK(ref->kind == scope_kind::global);
  CHECK_FALSE(ref->id.has_value());
}

TEST_CASE("resolve_slug: bare slug and 'assoc:' prefix resolve identically", "[scope][resolve_slug]") {
  scratch_db_path scratch;
  auto            conn     = open_migrated(scratch);
  const auto      assoc_id = insert_association(conn, "acme");

  auto bare = resolve_slug(conn, "acme");
  REQUIRE(bare.has_value());
  CHECK(bare->kind == scope_kind::association);
  CHECK(bare->id == assoc_id);

  auto prefixed = resolve_slug(conn, "assoc:acme");
  REQUIRE(prefixed.has_value());
  CHECK(prefixed->kind == scope_kind::association);
  CHECK(prefixed->id == assoc_id);
}

TEST_CASE("resolve_slug: 'repo:' prefix resolves against projects", "[scope][resolve_slug]") {
  scratch_db_path scratch;
  auto            conn       = open_migrated(scratch);
  const auto      project_id = insert_project(conn, "myrepo", "/work/myrepo");

  auto ref = resolve_slug(conn, "repo:myrepo");
  REQUIRE(ref.has_value());
  CHECK(ref->kind == scope_kind::repo);
  CHECK(ref->id == project_id);
}

TEST_CASE("resolve_slug: unknown slug is slug_not_found", "[scope][resolve_slug]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto ref = resolve_slug(conn, "no-such-slug");
  REQUIRE_FALSE(ref.has_value());
  CHECK(ref.error() == scope_error::slug_not_found);
}

TEST_CASE("slug_from_ref: association id round-trips to its slug; global is unset; repo carries the 'repo:' label",
          "[scope][slug_from_ref]") {
  scratch_db_path scratch;
  auto            conn       = open_migrated(scratch);
  const auto      assoc_id   = insert_association(conn, "acme");
  const auto      project_id = insert_project(conn, "myrepo", "/work/myrepo");

  auto global_slug = slug_from_ref(conn, scope_kind::global, std::nullopt);
  REQUIRE(global_slug.has_value());
  CHECK_FALSE(global_slug->has_value());

  auto assoc_slug = slug_from_ref(conn, scope_kind::association, assoc_id);
  REQUIRE(assoc_slug.has_value());
  REQUIRE(assoc_slug->has_value());
  CHECK(**assoc_slug == "acme");

  auto repo_slug = slug_from_ref(conn, scope_kind::repo, project_id);
  REQUIRE(repo_slug.has_value());
  REQUIRE(repo_slug->has_value());
  CHECK(**repo_slug == "repo:myrepo");
}

// --- resolve_for_write: the `--scope` override precedence ---------------

TEST_CASE("resolve_for_write: explicit --scope wins over cwd derivation, even when they disagree", "[scope][resolve_for_write]") {
  scratch_db_path scratch;
  auto            conn       = open_migrated(scratch);
  const auto      project_id = insert_project(conn, "myrepo", "/work/myrepo");
  const auto      cwd_org    = insert_association(conn, "cwd-org");
  insert_association(conn, "flag-org");
  link_project_association(conn, project_id, cwd_org);

  // cwd alone would derive "cwd-org"; the explicit flag names a DIFFERENT,
  // also-real association and must win outright (docs/concepts.md §Write
  // resolution: "the flag is the user's stated intent; never override it").
  auto res = resolve_for_write(conn, "flag-org", "/work/myrepo/src");
  REQUIRE(res.has_value());
  CHECK(res->from_explicit_flag);
  REQUIRE(res->scope.has_value());
  CHECK(*res->scope == "flag-org");
}

TEST_CASE("resolve_for_write: falls back to cwd derivation when no --scope flag is passed", "[scope][resolve_for_write]") {
  scratch_db_path scratch;
  auto            conn       = open_migrated(scratch);
  const auto      project_id = insert_project(conn, "myrepo", "/work/myrepo");
  const auto      assoc_id   = insert_association(conn, "myorg");
  link_project_association(conn, project_id, assoc_id);

  auto res = resolve_for_write(conn, std::nullopt, "/work/myrepo/src");
  REQUIRE(res.has_value());
  CHECK_FALSE(res->from_explicit_flag);
  REQUIRE(res->scope.has_value());
  CHECK(*res->scope == "myorg");
  CHECK(res->reason == derive_reason::project_single_association);
}

TEST_CASE("resolve_for_write: an unknown --scope flag value is threaded through verbatim, not validated against the DB",
          "[scope][resolve_for_write]") {
  // Mirrors zig's resolveForWrite (zig/src/cmd/planar/scope.zig:84-93):
  // the write path does NOT call resolve_slug/resolveSlug at all.
  // Validation is a read-path-only concern; an unknown scope surfaces
  // later, from whatever verb actually tries to use it.
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto res = resolve_for_write(conn, "no-such-scope", "/anywhere");
  REQUIRE(res.has_value());
  CHECK(res->from_explicit_flag);
  REQUIRE(res->scope.has_value());
  CHECK(*res->scope == "no-such-scope");
}

TEST_CASE("resolve_for_write: explicit --scope global is threaded through as the literal label, not collapsed to unset",
          "[scope][resolve_for_write]") {
  // Zig carries the literal string "global" as the resolved write scope
  // (no special-casing in resolveForWrite/resolve). check_scope_guard's
  // first branch (entity_scope unset -> any write allowed) means this is
  // observably identical to "unset" for global-scoped entities, and
  // correctly REFUSES a guarded write against a non-global entity, which
  // an unset write_scope would also refuse — so no behavior is lost by
  // preserving the literal label.
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto res = resolve_for_write(conn, "global", "/anywhere");
  REQUIRE(res.has_value());
  CHECK(res->from_explicit_flag);
  REQUIRE(res->scope.has_value());
  CHECK(*res->scope == "global");
}

// --- resolve_for_write: the meta-workspace arm (task 6134) --------------
//
// Zig's `resolveForWrite` calls `resolveMetaWorkspaceWriteScope` and can
// REFUSE with "ambiguous meta workspace root". This port went straight to
// `derive_from_cwd`, so the refusal simply did not exist. It was unreachable
// while nothing could register a meta workspace — and would have become a
// silent wrong answer the moment `workspace init` landed, which is the same
// class as tasks 6128 and 6132. These cases seed the registration by hand so
// the arm is reachable and pinned NOW.

TEST_CASE("resolve_for_write: an ambiguous meta workspace root REFUSES and names both choices",
          "[scope][resolve_for_write][6134]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  // The defining shape: the org's root_path and the root repo's root_path
  // are the SAME directory. Standing there, the cwd names both scopes
  // equally well and no default is safe.
  const auto project_id = insert_project(conn, "meta-root-repo", "/work/meta");
  const auto assoc_id   = insert_association(conn, "meta-org");
  link_project_association(conn, project_id, assoc_id);
  mark_meta_workspace(conn, assoc_id, "/work/meta");

  auto res = resolve_for_write(conn, std::nullopt, "/work/meta");
  REQUIRE_FALSE(res.has_value());
  CHECK(res.error().code == scope_error::scope_mismatch);
  // The refusal carries the two `--scope` values the operator may pick, in
  // the oracle's spelling. Without them the caller would have to re-run the
  // probe purely to render its own message.
  REQUIRE(res.error().ambiguity.has_value());
  CHECK(res.error().ambiguity->assoc_scope == "assoc:meta-org");
  CHECK(res.error().ambiguity->repo_scope == "repo:meta-root-repo");
}

TEST_CASE("resolve_for_write: an explicit --scope settles the meta ambiguity without probing",
          "[scope][resolve_for_write][6134]") {
  // The oracle returns on the override BEFORE it probes, and that ordering
  // is the whole remedy the refusal message points at: telling an operator
  // to pass `--scope` would be useless if passing it still refused.
  scratch_db_path scratch;
  auto            conn       = open_migrated(scratch);
  const auto      project_id = insert_project(conn, "meta-root-repo", "/work/meta");
  const auto      assoc_id   = insert_association(conn, "meta-org");
  link_project_association(conn, project_id, assoc_id);
  mark_meta_workspace(conn, assoc_id, "/work/meta");

  auto res = resolve_for_write(conn, "assoc:meta-org", "/work/meta");
  REQUIRE(res.has_value());
  CHECK(res->from_explicit_flag);
  REQUIRE(res->scope.has_value());
  CHECK(*res->scope == "assoc:meta-org");
}

TEST_CASE("resolve_for_write: BELOW a meta workspace root, the write lands on the member repo",
          "[scope][resolve_for_write][6134]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  // Org rooted at /work/meta, with a member repo one level down. There is
  // no ambiguity here — the cwd is under exactly one member — so the write
  // resolves to that repo rather than to the org.
  const auto org_project = insert_project(conn, "meta-root-repo", "/work/meta");
  const auto member      = insert_project(conn, "member-a", "/work/meta/member-a");
  const auto assoc_id    = insert_association(conn, "meta-org");
  link_project_association(conn, org_project, assoc_id);
  link_project_association(conn, member, assoc_id);
  mark_meta_workspace(conn, assoc_id, "/work/meta");

  auto res = resolve_for_write(conn, std::nullopt, "/work/meta/member-a/src");
  REQUIRE(res.has_value());
  CHECK_FALSE(res->from_explicit_flag);
  REQUIRE(res->scope.has_value());
  // `repo:` prefixed, NOT the association slug: a write inside a meta
  // workspace must land on a concrete repository.
  CHECK(*res->scope == "repo:member-a");
}

TEST_CASE("resolve_for_write: the member match is the LONGEST root, not the first", "[scope][resolve_for_write][6134]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  // A nested member inside another member. `order by length(root_path)
  // desc` is what makes the inner one win; inserting the outer FIRST means
  // a naive "first row that matches" implementation returns the wrong one.
  const auto outer    = insert_project(conn, "outer", "/work/meta/outer");
  const auto inner    = insert_project(conn, "inner", "/work/meta/outer/inner");
  const auto assoc_id = insert_association(conn, "meta-org");
  link_project_association(conn, outer, assoc_id);
  link_project_association(conn, inner, assoc_id);
  mark_meta_workspace(conn, assoc_id, "/work/meta");

  auto res = resolve_for_write(conn, std::nullopt, "/work/meta/outer/inner/src");
  REQUIRE(res.has_value());
  REQUIRE(res->scope.has_value());
  CHECK(*res->scope == "repo:inner");
}

TEST_CASE("resolve_for_write: a SIBLING path sharing a prefix is not inside the workspace", "[scope][resolve_for_write][6134]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  // `/work/meta-other` starts with `/work/meta` as a STRING but is a
  // different directory. A plain prefix test would pull an unrelated
  // repository into the workspace and silently rescope its writes.
  const auto org_project = insert_project(conn, "meta-root-repo", "/work/meta");
  const auto assoc_id    = insert_association(conn, "meta-org");
  link_project_association(conn, org_project, assoc_id);
  mark_meta_workspace(conn, assoc_id, "/work/meta");

  const auto other_project = insert_project(conn, "other", "/work/meta-other");
  const auto other_assoc   = insert_association(conn, "other-org");
  link_project_association(conn, other_project, other_assoc);

  auto res = resolve_for_write(conn, std::nullopt, "/work/meta-other/src");
  REQUIRE(res.has_value());
  REQUIRE(res->scope.has_value());
  CHECK(*res->scope == "other-org");
}

TEST_CASE("resolve_for_write: an org WITHOUT the meta-repo shape is not a meta workspace", "[scope][resolve_for_write][6134]") {
  // An ordinary `kind = 'org'` association with no `workspace_shape` must
  // not trip the meta arm — otherwise every existing association in every
  // database would start refusing writes at its own root.
  scratch_db_path scratch;
  auto            conn       = open_migrated(scratch);
  const auto      project_id = insert_project(conn, "myrepo", "/work/myrepo");
  const auto      assoc_id   = insert_association(conn, "myorg");
  link_project_association(conn, project_id, assoc_id);

  auto res = resolve_for_write(conn, std::nullopt, "/work/myrepo");
  REQUIRE(res.has_value());
  REQUIRE(res->scope.has_value());
  CHECK(*res->scope == "myorg");
}

TEST_CASE("resolve_meta_workspace_write_scope: reports `none` when no meta workspace is registered",
          "[scope][resolve_for_write][6134]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto probe = resolve_meta_workspace_write_scope(conn, "/anywhere");
  REQUIRE(probe.has_value());
  CHECK(probe->which == meta_write_resolution::arm::none);
  CHECK_FALSE(probe->repo_scope.has_value());
  CHECK_FALSE(probe->choices.has_value());
}

// --- check_scope_guard / guard_write: the cross-scope guard's refusal ---

TEST_CASE("check_scope_guard: a global entity (unset scope) accepts any write scope", "[scope][guard]") {
  CHECK(check_scope_guard(std::nullopt, std::nullopt).has_value());
  CHECK(check_scope_guard(std::nullopt, "acme").has_value());
}

TEST_CASE("check_scope_guard: refuses when the entity has a scope but the resolver could not pin one (guarded write refusing on "
          "mismatch)",
          "[scope][guard]") {
  auto res = check_scope_guard("acme", std::nullopt);
  REQUIRE_FALSE(res.has_value());
  CHECK(res.error() == scope_error::scope_mismatch);
}

TEST_CASE("check_scope_guard: refuses a genuine cross-scope mismatch", "[scope][guard]") {
  auto res = check_scope_guard("acme", "beta");
  REQUIRE_FALSE(res.has_value());
  CHECK(res.error() == scope_error::scope_mismatch);
}

TEST_CASE("check_scope_guard: allows a matching scope, with or without the 'assoc:' prefix on either side", "[scope][guard]") {
  CHECK(check_scope_guard("acme", "acme").has_value());
  CHECK(check_scope_guard("acme", "assoc:acme").has_value());
  CHECK(check_scope_guard("assoc:acme", "acme").has_value());
}

TEST_CASE("check_scope_guard: keeps repo scope distinct from association scope of the same name", "[scope][guard]") {
  CHECK(check_scope_guard("repo:acme", "repo:acme").has_value());
  CHECK_FALSE(check_scope_guard("repo:acme", "acme").has_value());
  CHECK_FALSE(check_scope_guard("acme", "repo:acme").has_value());
}

TEST_CASE("check_scope_guard's exit-code mapping matches the captured Zig oracle: scope_mismatch -> exit 5",
          "[scope][guard][oracle]") {
  // Oracle capture (zig/zig-out/bin/planar, `task update <id>` from a
  // mismatched cwd): `exit=5`, stderr "error: scope mismatch: task 1 is
  // in scope 'acme' but operator write scope is 'global'; pass --scope
  // acme to write to that scope from here". This module raises
  // `scope_error::scope_mismatch`; a future `cmd/` handler maps it onto
  // its own binary's `domain_error_kind::scope_mismatch`.
  //
  // This case used to ALSO assert `exit_code_for(scope_mismatch, ...) == 5`
  // against layer-1's shared, binary-parameterized table. Task 6123 deleted
  // that table: the exit-code policy now lives once per binary, in each
  // `src/cmd/<binary>/exit.cppm`, precisely so no shared helper can apply
  // the wrong binary's row (see those files' headers, and task 6066's
  // rename which fought the same hazard). An engine test cannot import a
  // `cmd_*` module — that is an upward layer-2 -> layer-3 edge and
  // `cmake/architecture.cmake` FATALs on it — so the `scope_mismatch -> 5`
  // assertion moved DOWN-STREAM rather than being dropped: all three of
  // `src/cmd/*/exit_codes.t.cpp` pin the complete table, scope_mismatch
  // included, which is strictly more coverage than the two lines removed
  // here. What stays here is the engine-level fact those tests key off.
  auto res = check_scope_guard("acme", std::nullopt);
  REQUIRE_FALSE(res.has_value());
  CHECK(res.error() == scope_error::scope_mismatch);
}

TEST_CASE("guard_write: --no-scope-check bypasses a genuine mismatch (the documented escape hatch)",
          "[scope][guard][no-scope-check]") {
  // Without the bypass this is a refusal (pinned above); with it, always allowed.
  REQUIRE_FALSE(check_scope_guard("acme", "beta").has_value());
  CHECK(guard_write("acme", "beta", true).has_value());
  CHECK(guard_write("acme", std::nullopt, true).has_value());
}

TEST_CASE("guard_write: with no-scope-check disabled, behaves exactly like check_scope_guard", "[scope][guard][no-scope-check]") {
  CHECK(guard_write("acme", "acme", false).has_value());
  CHECK_FALSE(guard_write("acme", "beta", false).has_value());
}
