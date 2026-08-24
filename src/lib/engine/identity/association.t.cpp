// @file association.t.cpp
// @brief Unit tests for `planar.engine.identity.association` (plan 996,
// task cpp-scope-assoc). Exercises association CRUD and the
// project-membership surface the cross-scope guard's callers need,
// including a project belonging to two unrelated associations at once —
// the unguarded, by-design cross-scope link case (docs/concepts.md
// §cross-scope-guard, "Which verbs are deliberately not guarded":
// `*_link`/`assoc add` verbs create edges that may legitimately cross
// scope boundaries).
//
// Include-before-import is deliberate (see db/db.t.cpp).
#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.db.migrate;
import planar.engine.identity.association;

namespace {

using planar::engine::identity::add_member;
using planar::engine::identity::add_member_source;
using planar::engine::identity::association_error;
using planar::engine::identity::association_kind;
using planar::engine::identity::create;
using planar::engine::identity::create_args;
using planar::engine::identity::list_all;
using planar::engine::identity::members;
using planar::engine::identity::remove_member;
using planar::engine::identity::show_by_slug;

struct scratch_db_path {
  std::filesystem::path path_;

  scratch_db_path()
      : path_(std::filesystem::temp_directory_path() / std::format("planar_identity_assoc_test_{}_{}.db",
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

auto open_migrated(const scratch_db_path& scratch) -> planar::db::connection {
  auto conn = planar::db::connection::open(scratch.path_.string());
  REQUIRE(conn.has_value());
  auto applied = planar::db::apply_all(*conn);
  REQUIRE(applied.has_value());
  return std::move(*conn);
}

} // namespace

TEST_CASE("create + show_by_slug round-trip", "[association][crud]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto created = create(conn, create_args{.slug = "acme", .name = "Acme Corp", .kind = association_kind::org});
  REQUIRE(created.has_value());
  CHECK(created->slug == "acme");
  CHECK(created->name == "Acme Corp");
  CHECK(created->kind == association_kind::org);
  CHECK_FALSE(created->auto_detected);

  auto fetched = show_by_slug(conn, "acme");
  REQUIRE(fetched.has_value());
  CHECK(fetched->id == created->id);
}

TEST_CASE("create defaults name to slug and kind to ad-hoc when unset", "[association][crud]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto created = create(conn, create_args{.slug = "bare"});
  REQUIRE(created.has_value());
  CHECK(created->name == "bare");
  CHECK(created->kind == association_kind::ad_hoc);
}

TEST_CASE("create rejects a duplicate slug", "[association][crud]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  REQUIRE(create(conn, create_args{.slug = "dupe"}).has_value());
  auto second = create(conn, create_args{.slug = "dupe"});
  REQUIRE_FALSE(second.has_value());
  CHECK(second.error() == association_error::slug_conflict);
}

TEST_CASE("show_by_slug on an unknown slug is not_found", "[association][crud]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto res = show_by_slug(conn, "ghost");
  REQUIRE_FALSE(res.has_value());
  CHECK(res.error() == association_error::not_found);
}

TEST_CASE("list_all returns every association ordered by slug", "[association][crud]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  REQUIRE(create(conn, create_args{.slug = "zeta", .kind = association_kind::org}).has_value());
  REQUIRE(create(conn, create_args{.slug = "alpha", .kind = association_kind::client}).has_value());

  auto all = list_all(conn);
  REQUIRE(all.has_value());
  REQUIRE(all->size() == 2);
  CHECK((*all)[0].slug == "alpha");
  CHECK((*all)[1].slug == "zeta");
}

TEST_CASE("list_all on an empty database is an empty (not absent) result", "[association][crud][empty]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto all = list_all(conn);
  REQUIRE(all.has_value());
  CHECK(all->empty());
}

TEST_CASE("add_member creates the project on first touch, then refuses a duplicate link", "[association][membership]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  REQUIRE(create(conn, create_args{.slug = "myorg", .kind = association_kind::org}).has_value());

  REQUIRE(add_member(conn, "myorg", "/repos/foo").has_value());
  auto second = add_member(conn, "myorg", "/repos/foo");
  REQUIRE_FALSE(second.has_value());
  CHECK(second.error() == association_error::already_member);

  auto m = members(conn, "myorg");
  REQUIRE(m.has_value());
  REQUIRE(m->size() == 1);
  CHECK((*m)[0].slug == "foo");
  REQUIRE((*m)[0].root_path.has_value());
  CHECK(*(*m)[0].root_path == "/repos/foo");
}

TEST_CASE("add_member against an unknown association is not_found", "[association][membership]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto res = add_member(conn, "no-such-assoc", "/repos/foo");
  REQUIRE_FALSE(res.has_value());
  CHECK(res.error() == association_error::not_found);
}

TEST_CASE("add_member links one project into TWO unrelated associations (unguarded, by-design cross-scope link)",
          "[association][membership][unguarded-link]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  REQUIRE(create(conn, create_args{.slug = "org-a", .kind = association_kind::org}).has_value());
  REQUIRE(create(conn, create_args{.slug = "org-b", .kind = association_kind::org}).has_value());

  // A project belonging to org-a can ALSO be linked into org-b: membership
  // (project_associations) is many-to-many, and the guard is explicitly
  // not applied to this add-member path (docs/concepts.md
  // §cross-scope-guard, "All `*_add` / `*_create` verbs" +
  // "All `*_link` and `links add/remove` verbs" are deliberately
  // unguarded — an association add is exactly this shape).
  REQUIRE(add_member(conn, "org-a", "/repos/shared").has_value());
  REQUIRE(add_member(conn, "org-b", "/repos/shared").has_value());

  auto in_a = members(conn, "org-a");
  auto in_b = members(conn, "org-b");
  REQUIRE(in_a.has_value());
  REQUIRE(in_b.has_value());
  REQUIRE(in_a->size() == 1);
  REQUIRE(in_b->size() == 1);
  CHECK((*in_a)[0].slug == "shared");
  CHECK((*in_b)[0].slug == "shared");
  CHECK((*in_a)[0].id == (*in_b)[0].id); // same underlying project row
}

TEST_CASE("add_member resolves a basename collision with a numeric suffix", "[association][membership]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  REQUIRE(create(conn, create_args{.slug = "x"}).has_value());

  REQUIRE(add_member(conn, "x", "/work/foo").has_value());
  REQUIRE(add_member(conn, "x", "/personal/foo").has_value());

  auto m = members(conn, "x");
  REQUIRE(m.has_value());
  REQUIRE(m->size() == 2);
  CHECK((*m)[0].slug != (*m)[1].slug);
}

TEST_CASE("remove_member drops the join row", "[association][membership]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  REQUIRE(create(conn, create_args{.slug = "rm"}).has_value());
  REQUIRE(add_member(conn, "rm", "/repos/bar").has_value());

  REQUIRE(remove_member(conn, "rm", "/repos/bar").has_value());

  auto m = members(conn, "rm");
  REQUIRE(m.has_value());
  CHECK(m->empty());
}

TEST_CASE("remove_member on a project never linked (or never registered) is not_a_member (absent case)",
          "[association][membership][empty]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  REQUIRE(create(conn, create_args{.slug = "rm2"}).has_value());

  auto res = remove_member(conn, "rm2", "/never/registered");
  REQUIRE_FALSE(res.has_value());
  CHECK(res.error() == association_error::not_a_member);
}

TEST_CASE("members on an association with no member projects is empty (absent case)", "[association][membership][empty]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  REQUIRE(create(conn, create_args{.slug = "lonely"}).has_value());

  auto m = members(conn, "lonely");
  REQUIRE(m.has_value());
  CHECK(m->empty());
}

TEST_CASE("add_member_source renders the exact project_associations.source wire text", "[association][membership]") {
  using planar::engine::identity::add_member_source_to_text;
  CHECK(add_member_source_to_text(add_member_source::user) == "user");
  CHECK(add_member_source_to_text(add_member_source::auto_git_remote) == "auto:git-remote");
  CHECK(add_member_source_to_text(add_member_source::auto_path) == "auto:path");
  CHECK(add_member_source_to_text(add_member_source::auto_lang) == "auto:lang");
}

TEST_CASE("render_text emits the config line only when config_json is set", "[association][render][6133]") {
  // `planar assoc create` declares no `--config` flag, so the ONE
  // conditional line in this renderer is unreachable from the verb and a
  // handler-level test cannot cover it. Reached directly here instead of
  // being left untested — the same arm the oracle carries
  // (zig/src/engine/identity/association.zig:353).
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto plain = create(conn, {.slug = "plain"});
  REQUIRE(plain.has_value());
  auto const plain_text = planar::engine::identity::render_text(*plain);
  CHECK(plain_text.starts_with("id:        1\n"
                               "slug:      plain\n"
                               "name:      plain\n"
                               "kind:      ad-hoc\n"
                               "auto:      no\n"
                               "created:   "));
  CHECK_FALSE(plain_text.contains("config:"));
  CHECK(plain_text.ends_with("\n"));

  auto configured = create(conn, {.slug = "configured", .config_json = R"({"k":"v"})"});
  REQUIRE(configured.has_value());
  auto const configured_text = planar::engine::identity::render_text(*configured);
  // Between `auto:` and `created:`, not appended at the end.
  CHECK(configured_text.contains("auto:      no\n"
                                 "config:    {\"k\":\"v\"}\n"
                                 "created:   "));
}

TEST_CASE("render_json escapes operator-supplied text and keeps config_json a STRING", "[association][render][6133]") {
  // `name` and `config_json` are operator-supplied and can carry quotes and
  // backslashes; a `std::format` that interpolated them raw would emit
  // invalid JSON. `config_json` is a `?[]const u8` on the Zig side, so it
  // serialises as a JSON string, NOT as an inlined object — an
  // implementation that spliced the blob in unquoted would look more
  // useful and would not match the oracle.
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto tricky =
      create(conn, {.slug = "tricky", .name = R"(a "b" c\d)", .kind = association_kind::org, .config_json = R"({"k":"v"})"});
  REQUIRE(tricky.has_value());
  auto const json = planar::engine::identity::render_json(*tricky);
  CHECK(json.starts_with(R"({"id":1,"slug":"tricky","name":"a \"b\" c\\d","kind":"org",)"
                         R"("auto_detected":false,"config_json":"{\"k\":\"v\"}",)"));
  // A FRAGMENT: the caller appends the terminator (see the @return).
  CHECK(json.ends_with("}"));
  CHECK_FALSE(json.ends_with("}\n"));
}
