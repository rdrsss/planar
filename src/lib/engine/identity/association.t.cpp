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
using planar::engine::identity::project_ref;
using planar::engine::identity::remove_member;
using planar::engine::identity::render_member_list_json;
using planar::engine::identity::render_member_list_text;
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

// ===========================================================================
// task 6135 — auto-registered project naming, through `add_member`
// ===========================================================================

TEST_CASE("add_member derives the project basename the way zig does", "[association][member][6135]") {
  // The divergence this test exists for: the implementation used
  // `std::filesystem::path::filename`, which returns EMPTY for a path
  // ending in a separator, where zig's `std.fs.path.basename` strips
  // trailing separators first. Both binaries exited 0 with byte-identical
  // stdout; only the ROW differed — `slug='_', name=''` here against
  // `slug='repo-2', name='repo'` on the oracle. `_` is
  // `slugify_path_segment`'s empty-input fallback, which is the tell that
  // the basename came back empty.
  //
  // `path_basename` is a file-local helper with no exported surface, so
  // it is exercised the only way a caller can reach it: through the row
  // `add_member` writes.
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  REQUIRE(create(conn, {.slug = "acme"}).has_value());

  struct expectation {
    std::string_view path;
    std::string_view slug;
    std::string_view name;
  };
  // Each path is DISTINCT as a string, so each registers its own project;
  // the derived slugs collide, and the numeric-suffix retry resolves them
  // in insertion order.
  for (auto const& want : std::vector<expectation>{
           {"/a/b/repo", "repo", "repo"},
           {"/a/b/repo/", "repo-2", "repo"},   // ONE trailing separator
           {"/a/b/repo//", "repo-3", "repo"},  // several
           {"/a/b/repo///", "repo-4", "repo"}, //
           {"relative/path", "path", "path"},  // not absolute, still accepted
           {"bare", "bare", "bare"},           // no separator at all
           {"/", "_", ""},                     // the root: EMPTY basename
       }) {
    INFO("repo_path " << want.path);
    REQUIRE(add_member(conn, "acme", want.path).has_value());
  }

  auto listed = members(conn, "acme");
  REQUIRE(listed.has_value());
  REQUIRE(listed->size() == 7);

  // `members` orders by project slug, so index by root_path instead.
  auto const find = [&listed](std::string_view path) -> std::optional<std::string> {
    for (auto const& p : *listed) {
      if (p.root_path.has_value() && *p.root_path == path) {
        return p.slug + "|" + p.name;
      }
    }
    return std::nullopt;
  };
  CHECK(find("/a/b/repo") == "repo|repo");
  CHECK(find("/a/b/repo/") == "repo-2|repo"); // NOT "_|"
  CHECK(find("/a/b/repo//") == "repo-3|repo");
  CHECK(find("/a/b/repo///") == "repo-4|repo");
  CHECK(find("relative/path") == "path|path");
  CHECK(find("bare") == "bare|bare");
  // The root path's basename really is empty; `name` is '' and the slug
  // falls back to `_`. Reaching the binder as a NULL-data `string_view`
  // here bound SQL NULL into a NOT NULL column and failed the whole verb.
  CHECK(find("/") == "_|");
}

// --- member-list renderers (plan 996, task 6188) -------------------------

TEST_CASE("render_member_list_text: the second column is root_path, not name", "[association][render]") {
  // The two are equal for a project registered by its own directory name,
  // which is every project in the simple fixtures above — so the case that
  // actually discriminates needs slug, name and path all DIFFERENT. This
  // one was derived by running the oracle against a project at
  // `../aaaa…`, which printed the PATH.
  const std::vector<project_ref> rows{
      {.id = 4, .slug = "_", .name = ".", .root_path = "."},
      {.id = 3, .slug = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", .name = "widgets", .root_path = "../elsewhere"},
  };
  CHECK(render_member_list_text(rows) == "_                     .\n"
                                         "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa  ../elsewhere\n");
  // Neither `name` appears anywhere in the output — the assertion above
  // would still pass if the renderer emitted `name` for row 1, whose two
  // fields happen to match.
  CHECK_FALSE(render_member_list_text(rows).contains("widgets"));
}

TEST_CASE("render_member_list_text: an unset root_path prints `(no root)`", "[association][render][null]") {
  // `projects.root_path` is genuinely nullable, and a blank column would
  // make an unregistered project indistinguishable from one rooted at "".
  const std::vector<project_ref> unset{{.id = 1, .slug = "nullroot", .name = "Null Root", .root_path = std::nullopt}};
  const std::vector<project_ref> empty_path{{.id = 1, .slug = "nullroot", .name = "Null Root", .root_path = std::string{}}};
  CHECK(render_member_list_text(unset) == "nullroot              (no root)\n");
  CHECK(render_member_list_text(empty_path) == "nullroot              \n");
}

TEST_CASE("render_member_list_*: the EMPTY case is a word, and the two forms differ", "[association][render][empty]") {
  CHECK(render_member_list_text({}) == "(no members)\n");
  CHECK(render_member_list_json({}) == "[]");
}

TEST_CASE("render_member_list_json: field order, null root_path, escaping", "[association][render]") {
  const std::vector<project_ref> rows{
      {.id = 2, .slug = "repo-2", .name = "repo", .root_path = "repo"},
      {.id = 5, .slug = R"(q"uote)", .name = "n", .root_path = std::nullopt},
  };
  // Declaration order (id, slug, name, root_path), single line, `null` for
  // the unset path — and the slug IS escaped here, unlike `assoc add
  // --json`'s deliberately raw interpolation.
  CHECK(render_member_list_json(rows) == R"([{"id":2,"slug":"repo-2","name":"repo","root_path":"repo"},)"
                                         R"({"id":5,"slug":"q\"uote","name":"n","root_path":null}])");
  CHECK(render_member_list_json(rows).find('\n') == std::string::npos);
}

TEST_CASE("members feeds the renderers in project-slug order", "[association][membership][render]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  REQUIRE(create(conn, create_args{.slug = "acme", .name = "Acme", .kind = association_kind::org}).has_value());
  REQUIRE(add_member(conn, "acme", "/w/zebra").has_value());
  REQUIRE(add_member(conn, "acme", "/w/alpha").has_value());

  auto listed = members(conn, "acme");
  REQUIRE(listed.has_value());
  // Ordering is the ENGINE's, and the renderer must not re-sort: the
  // insertion order here is the reverse of the slug order.
  CHECK(render_member_list_text(*listed) == "alpha                 /w/alpha\n"
                                            "zebra                 /w/zebra\n");
  CHECK(render_member_list_json(*listed).starts_with(R"([{"id":)"));
  CHECK(render_member_list_json(*listed).contains(R"("slug":"alpha")"));
}

// ===========================================================================
// Auto-detection: proposals, enrichment, apply (plan 996, task 6325)
// ===========================================================================
//
// ## THE HEURISTIC HAS NO THRESHOLDS, AND THAT IS THE FIRST FINDING
//
// `assoc detect` reads like a scoring engine and is not one. There is no
// score, no cutoff, no minimum signal count and no confidence value
// anywhere in the oracle. Every present signal contributes its proposals
// unconditionally, and the four arms (git-remote host, git-remote org,
// parent directory, language marker) are INDEPENDENT — none gates another.
// The cases below therefore probe each arm on its own fixture rather than
// inferring three of them from one multi-signal run.
//
// ## THE ORDER IS APPEND ORDER, NOT A HASH ARTIFACT
//
// Task 6274 found the routing table's `languages` order to be a Zig
// `StringHashMap` artifact — unspecified and unsafe to pin. This list is
// NOT that: the oracle appends to a plain `ArrayList` in a fixed sequence,
// so host/org/path/lang is a real, reproducible contract and pinning it
// pins behavior rather than an implementation detail. No divergence to
// record.
//
// ## THERE IS NO DEDUP PASS, AND NONE IS NEEDED
//
// Each arm emits at most one proposal and each uses a distinct slug
// prefix, so two proposals cannot collide within a run. Idempotency ACROSS
// runs is `apply_proposals`' job (`insert or ignore` + an upsert), not the
// proposal builder's, and it is pinned separately below.
//
// ## THE EMPTY CASE IS NEARLY UNREACHABLE, WHICH IS A VACUITY HAZARD
//
// The parent-directory arm fires for essentially every real cwd, so a
// detector fixture almost always yields at least `path:<parent>`. The
// danger runs the other way from the usual one: a fixture that somehow
// produces NOTHING makes every "the proposals contain X" assertion pass
// vacuously. Each case below therefore asserts its own fixture's shape —
// how many proposals — before comparing anything against it.

namespace {

using planar::engine::identity::apply_proposals;
using planar::engine::identity::association;
using planar::engine::identity::detect_signals;
using planar::engine::identity::enrich_proposals;
using planar::engine::identity::parse_remote;
using planar::engine::identity::proposal;
using planar::engine::identity::proposal_action_label;
using planar::engine::identity::proposals_from_signals;
using planar::engine::identity::render_detect_json;
using planar::engine::identity::render_detect_text;

/// @brief The slugs of `items`, in order, for compact comparison.
auto slugs_of(std::span<const proposal> items) -> std::vector<std::string> {
  std::vector<std::string> out;
  out.reserve(items.size());
  for (const auto& p : items) {
    out.push_back(p.slug);
  }
  return out;
}

/// @brief The slugs of association rows, in the order `list_all` returned them.
auto assoc_slugs_of(std::span<const association> items) -> std::vector<std::string> {
  std::vector<std::string> out;
  out.reserve(items.size());
  for (const auto& a : items) {
    out.push_back(a.slug);
  }
  return out;
}

} // namespace

TEST_CASE("proposals_from_signals: every arm alone, and the full bundle's order", "[association][detect][signals]") {
  // 1. NO signals at all -> genuinely empty. Established FIRST so the
  //    non-empty cases below are known to be producing something.
  CHECK(proposals_from_signals(detect_signals{}).empty());

  // 2. Each arm ALONE. Probed separately rather than inferred from the
  //    bundle in (3): a bug that drops one arm only when another is
  //    present is invisible to a single multi-signal fixture.
  {
    auto const only_remote = proposals_from_signals(detect_signals{.git_remote = "git@github.com:AcmeCorp/My_Repo.git"});
    CHECK(slugs_of(only_remote) == std::vector<std::string>{"host:github.com", "org:acmecorp"});
    // The SOURCE is `auto:git-remote` for BOTH, and the two reasons differ.
    REQUIRE(only_remote.size() == 2);
    CHECK(only_remote[0].kind == association_kind::host);
    CHECK(only_remote[0].source == add_member_source::auto_git_remote);
    CHECK(only_remote[0].reason == "from git remote host");
    CHECK(only_remote[1].kind == association_kind::org);
    CHECK(only_remote[1].source == add_member_source::auto_git_remote);
    CHECK(only_remote[1].reason == "from git remote org");
  }
  {
    auto const only_path = proposals_from_signals(detect_signals{.parent_basename = "workspace"});
    CHECK(slugs_of(only_path) == std::vector<std::string>{"path:workspace"});
    REQUIRE(only_path.size() == 1);
    CHECK(only_path[0].kind == association_kind::path);
    CHECK(only_path[0].source == add_member_source::auto_path);
    CHECK(only_path[0].reason == "from parent directory");
  }
  {
    auto const only_lang = proposals_from_signals(detect_signals{.lang = "go"});
    CHECK(slugs_of(only_lang) == std::vector<std::string>{"lang:go"});
    REQUIRE(only_lang.size() == 1);
    CHECK(only_lang[0].kind == association_kind::lang);
    CHECK(only_lang[0].source == add_member_source::auto_lang);
    CHECK(only_lang[0].reason == "from detected language ecosystem");
  }

  // 3. ALL of them: four proposals in host, org, path, lang order. The
  //    inputs are chosen so the four slugs sort DIFFERENTLY than they
  //    emit (`host:` < `lang:` < `org:` < `path:` alphabetically), so a
  //    renderer that sorted would produce a different vector and this
  //    assertion would catch it.
  auto const all = proposals_from_signals(
      detect_signals{.git_remote = "git@github.com:AcmeCorp/My_Repo.git", .parent_basename = "workspace", .lang = "go"});
  CHECK(slugs_of(all) == std::vector<std::string>{"host:github.com", "org:acmecorp", "path:workspace", "lang:go"});
}

TEST_CASE("proposals_from_signals: a present signal can still contribute nothing", "[association][detect][signals]") {
  // Each of these has a signal PRESENT and non-empty, and still emits no
  // proposal for that arm. An "is the optional set?" implementation passes
  // none of them.

  // A remote that is not a forge URL at all. Both fields come back empty
  // and neither the host nor the org proposal fires.
  CHECK(proposals_from_signals(detect_signals{.git_remote = "/srv/git/repo.git"}).empty());
  // A bare `git@host` names no repository -- no colon, so no owner path.
  CHECK(proposals_from_signals(detect_signals{.git_remote = "git@github.com"}).empty());
  // A host with no owner: the HOST arm still fires, the ORG arm does not.
  // Asserting only "no org proposal" would pass against a total failure,
  // so the surviving host proposal is pinned in the same breath.
  CHECK(slugs_of(proposals_from_signals(detect_signals{.git_remote = "https://github.com/"})) ==
        std::vector<std::string>{"host:github.com"});

  // The three parent-basename literals that are skipped outright...
  CHECK(proposals_from_signals(detect_signals{.parent_basename = ""}).empty());
  CHECK(proposals_from_signals(detect_signals{.parent_basename = "."}).empty());
  CHECK(proposals_from_signals(detect_signals{.parent_basename = "/"}).empty());
  // ...and the subtler one: a name that survives the literal checks and
  // then sanitizes away to nothing. A `path:` proposal with an empty tail
  // would be a real slug in the database, so this arm matters.
  CHECK(proposals_from_signals(detect_signals{.parent_basename = "+++"}).empty());
  // The same shape is NOT empty once one keepable character is present --
  // otherwise the four assertions above would pass against a broken arm.
  CHECK(slugs_of(proposals_from_signals(detect_signals{.parent_basename = "+a+"})) == std::vector<std::string>{"path:a"});

  CHECK(proposals_from_signals(detect_signals{.lang = ""}).empty());
}

TEST_CASE("proposals_from_signals: host and path slugs sanitize by DIFFERENT rules", "[association][detect][slug]") {
  // The two sanitizers differ by exactly one character class, and neither
  // is this module's existing `slugify_path_segment` (which keeps ALNUM
  // ONLY and falls back to `_`). Reusing that helper for either arm would
  // silently produce different slugs -- different rows -- so each
  // difference is pinned against a same-input pair where possible.
  auto host_slug = [](std::string_view h) {
    return slugs_of(proposals_from_signals(detect_signals{.git_remote = std::format("https://{}/o/r.git", h)})).front();
  };
  auto path_slug = [](std::string_view p) {
    return slugs_of(proposals_from_signals(detect_signals{.parent_basename = std::string{p}})).front();
  };

  // THE discriminating input: a dot. Kept by the host rule, mapped to a
  // dash by the path rule. Same character, same position, two answers.
  CHECK(host_slug("dot.name") == "host:dot.name");
  CHECK(path_slug("dot.name") == "path:dot-name");

  // Both rules preserve `_` and `-` VERBATIM, including doubled runs --
  // this is where `slugify_path_segment` would diverge, collapsing them.
  CHECK(path_slug("a__b") == "path:a__b");
  CHECK(path_slug("a--b") == "path:a--b");
  CHECK(host_slug("my_host.example.com") == "host:my_host.example.com");
  CHECK(host_slug("HOST..com") == "host:host..com");

  // Both lowercase, and collapse a run of unkeepable characters to ONE
  // dash rather than one dash each.
  CHECK(path_slug("Mixed_Case-Name") == "path:mixed_case-name");
  CHECK(path_slug("My Weird.Dir++Name") == "path:my-weird-dir-name");
  // Leading and trailing junk produce NO leading or trailing dash.
  CHECK(path_slug("  spaced  ") == "path:spaced");
}

TEST_CASE("parse_remote: the two recognized shapes, and what falls outside them", "[association][detect][remote]") {
  // SCP style is matched on the literal `git@` PREFIX...
  CHECK(parse_remote("git@github.com:owner/repo.git").host == "github.com");
  CHECK(parse_remote("git@github.com:owner/repo.git").owner == "owner");
  // ...so an `ssh://` URL that merely CONTAINS `git@` takes the OTHER
  // branch. Both branches happen to agree here, which is exactly why the
  // case is worth pinning: it documents that the fallthrough is intended
  // and correct rather than an accident nobody exercised.
  CHECK(parse_remote("ssh://git@github.com/owner/repo.git").host == "github.com");
  CHECK(parse_remote("ssh://git@github.com/owner/repo.git").owner == "owner");

  // URL style strips the `user@` prefix and the `:port` suffix. NOTE the
  // host comes back with its ORIGINAL case -- `parse_remote` does not
  // lowercase, the sanitizer does. Pinned so a later "tidy" that moves the
  // lowercasing in here has to change this line deliberately.
  CHECK(parse_remote("https://user@Git.Example.COM:8443/Some Owner/proj.git").host == "Git.Example.COM");
  CHECK(parse_remote("https://user@Git.Example.COM:8443/Some Owner/proj.git").owner == "Some Owner");

  // Only the FIRST path segment is the owner; deeper groups are dropped.
  CHECK(parse_remote("https://gitlab.com/group/sub/repo.git").owner == "group");
  // `.git` is stripped from the owner in the degenerate one-segment shape.
  CHECK(parse_remote("https://github.com/onlyowner.git").owner == "onlyowner");

  // Outside both shapes: BOTH fields empty, no guessing.
  CHECK(parse_remote("/srv/git/repo.git").host.empty());
  CHECK(parse_remote("/srv/git/repo.git").owner.empty());
  CHECK(parse_remote("git@github.com").host.empty());
  CHECK(parse_remote("").host.empty());
  // A host with no owner keeps the host and empties the owner -- asserting
  // only the empty half would pass against a total parse failure.
  CHECK(parse_remote("https://github.com/").host == "github.com");
  CHECK(parse_remote("https://github.com/").owner.empty());
}

TEST_CASE("proposal_action_label: membership wins over existence", "[association][detect][label]") {
  // The ORDER of the two tests is the contract. A `member_exists` proposal
  // necessarily also has `assoc_exists`, so testing existence first would
  // label every existing member "already exists, will add" -- a plausible
  // sentence, which is what makes the bug survivable.
  CHECK(proposal_action_label(proposal{.assoc_exists = false, .member_exists = false}) == "will create");
  CHECK(proposal_action_label(proposal{.assoc_exists = true, .member_exists = false}) == "already exists, will add");
  CHECK(proposal_action_label(proposal{.assoc_exists = true, .member_exists = true}) == "already a member");
}

TEST_CASE("render_detect_json is NDJSON, and empty means ZERO LINES", "[association][detect][render]") {
  // Non-empty renders newline-delimited objects -- NOT a JSON array. The
  // payload as a whole is not parseable by one `JSON.parse`; each LINE is.
  // That half is unchanged by task 6326.
  auto const two = proposals_from_signals(detect_signals{.parent_basename = "ws", .lang = "go"});
  REQUIRE(two.size() == 2); // guards the assertions below against an empty render
  auto const json = render_detect_json(two);
  CHECK_FALSE(json.starts_with("["));
  CHECK(json == R"({"slug":"path:ws","kind":"path","source":"auto:path","reason":"from parent directory",)"
                R"("assoc_exists":false,"member_exists":false,"action":"will create"})"
                "\n"
                R"({"slug":"lang:go","kind":"lang","source":"auto:lang","reason":"from detected language ecosystem",)"
                R"("assoc_exists":false,"member_exists":false,"action":"will create"})"
                "\n");
  // EVERY line is terminated, the last one included, so the renderer owns
  // its own terminators and the empty case can be genuinely empty. Task
  // 6326 moved the trailing newline in here from the caller; before, the
  // caller appended one unconditionally, which is why an empty render had
  // to be a non-empty string.
  CHECK(json.ends_with("\n"));

  // EMPTY IS ZERO BYTES (task 6326). It used to be `{"proposals":[]}` — a
  // key the non-empty form NEVER emits, wrapping an array it never
  // produces, so a consumer written against either shape broke on the
  // other. The rule chosen for the whole shape-split family (6257 / 6270 /
  // 6326) is NDJSON with zero lines for an empty result: it leaves the
  // populated bytes above untouched, and it is what `links list --json`
  // already did.
  CHECK(render_detect_json({}).empty());
}

TEST_CASE("render_detect_text: asymmetric gutters, and the sentence for empty", "[association][detect][render]") {
  auto const two = proposals_from_signals(detect_signals{.parent_basename = "ws", .lang = "go"});
  REQUIRE(two.size() == 2);
  // ONE space after the padded slug, TWO after the closing paren. The
  // asymmetry came from the oracle's format string; a symmetric
  // transcription built cleanly and was caught only by a byte-level
  // differential, because every slug here is shorter than the 24-wide
  // field and the padding hides the difference.
  CHECK(render_detect_text(two) == "proposed associations:\n"
                                   "  path:ws                  (from parent directory)  [will create]\n"
                                   "  lang:go                  (from detected language ecosystem)  [will create]\n");

  // A slug LONGER than the field is not truncated; it pushes the rest of
  // the line right. This is the only fixture shape in which the single
  // space after the field is observable at all -- with a short slug the
  // padding supplies the gap and one space reads the same as two.
  std::vector<proposal> wide{proposal{.slug   = "path:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                                      .kind   = association_kind::path,
                                      .source = add_member_source::auto_path,
                                      .reason = "from parent directory"}};
  CHECK(render_detect_text(wide) == "proposed associations:\n"
                                    "  path:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa (from parent directory)  [will create]\n");

  // Empty is a SENTENCE, and a different one from this file's
  // parenthesized `(no associations)` / `(no members)` sentinels.
  CHECK(render_detect_text({}) == "no proposed associations\n");
}

TEST_CASE("enrich_proposals: reads state, writes none, and needs no registered project", "[association][detect][enrich]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto proposals = proposals_from_signals(detect_signals{.parent_basename = "ws", .lang = "go"});
  REQUIRE(proposals.size() == 2); // non-vacuity: there IS something to enrich

  // 1. Nothing exists yet -> both flags false on both proposals.
  REQUIRE(enrich_proposals(conn, proposals, "/w/repo").has_value());
  CHECK_FALSE(proposals[0].assoc_exists);
  CHECK_FALSE(proposals[0].member_exists);
  CHECK_FALSE(proposals[1].assoc_exists);

  // 2. Create ONE of the two associations, with no membership. Only that
  //    one flips, and only its `assoc_exists` half -- the untouched
  //    proposal is the control that proves the query discriminates.
  //    The project is NOT registered here and this is still success, not
  //    an error; that asymmetry with `apply_proposals` is what makes
  //    preview usable before `planar init`.
  REQUIRE(create(conn, create_args{.slug = "path:ws", .kind = association_kind::path}).has_value());
  REQUIRE(enrich_proposals(conn, proposals, "/w/repo").has_value());
  CHECK(proposals[0].assoc_exists);
  CHECK_FALSE(proposals[0].member_exists);
  CHECK_FALSE(proposals[1].assoc_exists);

  // 3. Register the project and link it -> `member_exists` flips too, and
  //    the OTHER proposal is still untouched, so this did not simply set
  //    every flag.
  REQUIRE(add_member(conn, "path:ws", "/w/repo").has_value());
  REQUIRE(enrich_proposals(conn, proposals, "/w/repo").has_value());
  CHECK(proposals[0].assoc_exists);
  CHECK(proposals[0].member_exists);
  CHECK_FALSE(proposals[1].assoc_exists);

  // 4. Membership is per-PROJECT. To see that, the other root must itself
  //    be REGISTERED -- see (5) for why an unregistered one proves nothing.
  REQUIRE(create(conn, create_args{.slug = "seed"}).has_value());
  REQUIRE(add_member(conn, "seed", "/w/elsewhere").has_value());
  REQUIRE(enrich_proposals(conn, proposals, "/w/elsewhere").has_value());
  CHECK(proposals[0].assoc_exists);
  CHECK_FALSE(proposals[0].member_exists); // registered, but not linked to path:ws

  // 5. THE FLAGS ARE WRITE-ONLY: enrichment SETS them and never CLEARS
  //    them. Two paths skip the membership probe entirely and leave
  //    whatever was already there -- an unregistered root (the project id
  //    resolves to zero and the guarded probe is skipped) and a proposal
  //    whose association does not exist (the loop `continue`s before
  //    either assignment).
  //
  //    So re-enriching the SAME slice against a different root does not
  //    reset it. This was asserted the other way round first and failed;
  //    re-reading the oracle confirmed the port is faithful and the
  //    expectation was wrong. It is invisible to operators because the
  //    handler builds a fresh proposal slice per invocation, but anything
  //    that reuses a slice inherits stale flags.
  REQUIRE(add_member(conn, "path:ws", "/w/elsewhere").has_value());
  REQUIRE(enrich_proposals(conn, proposals, "/w/elsewhere").has_value());
  REQUIRE(proposals[0].member_exists); // set to true by the probe...
  REQUIRE(enrich_proposals(conn, proposals, "/w/never-registered").has_value());
  CHECK(proposals[0].member_exists); // ...and NOT cleared by the unregistered root
}

TEST_CASE("apply_proposals: refuses before it writes anything", "[association][detect][apply]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto proposals = proposals_from_signals(detect_signals{.parent_basename = "ws", .lang = "go"});
  REQUIRE(proposals.size() == 2);

  // The refusal, probed BEFORE any success: an unregistered project root
  // is `not_found` and nothing is written.
  auto const refused = apply_proposals(conn, proposals, "/w/never-registered");
  REQUIRE_FALSE(refused.has_value());
  CHECK(refused.error() == association_error::not_found);
  auto const after = list_all(conn);
  REQUIRE(after.has_value());
  CHECK(after->empty()); // no association was created on the way to the refusal

  // The project lookup precedes the loop, so an EMPTY proposal set against
  // an unregistered root refuses identically rather than succeeding
  // vacuously. A guard placed inside the loop would return success here.
  auto const refused_empty = apply_proposals(conn, {}, "/w/never-registered");
  REQUIRE_FALSE(refused_empty.has_value());
  CHECK(refused_empty.error() == association_error::not_found);
}

TEST_CASE("apply_proposals: creates the association endpoint rather than validating it", "[association][detect][apply]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  // Register a project at an ABSOLUTE path. Task 6256: `assoc add <slug> .`
  // stores the literal `.`, a row nothing can match -- so the fixture uses
  // an absolute path, and the membership assertion below proves the seed
  // took rather than silently resolving to nothing.
  REQUIRE(create(conn, create_args{.slug = "seed"}).has_value());
  REQUIRE(add_member(conn, "seed", "/w/repo").has_value());

  auto proposals = proposals_from_signals(
      detect_signals{.git_remote = "git@github.com:AcmeCorp/My_Repo.git", .parent_basename = "ws", .lang = "go"});
  REQUIRE(proposals.size() == 4); // non-vacuity: four proposals to apply

  // None of the four associations exists yet -- so if apply VALIDATED its
  // link endpoints (the open question tasks 6197 / 6314 / 6283 circle) it
  // would refuse here. It does not: it CREATES them. Captured, not judged.
  REQUIRE(apply_proposals(conn, proposals, "/w/repo").has_value());

  auto listed = list_all(conn);
  REQUIRE(listed.has_value());
  CHECK(assoc_slugs_of(*listed) == std::vector<std::string>{"host:github.com", "lang:go", "org:acmecorp", "path:ws", "seed"});

  // Every created row is marked auto-detected and named for its slug.
  auto const created = show_by_slug(conn, "org:acmecorp");
  REQUIRE(created.has_value());
  CHECK(created->auto_detected);
  CHECK(created->name == "org:acmecorp");
  CHECK(created->kind == association_kind::org);

  // The membership rows exist too -- creating the association without
  // linking it would satisfy the listing assertion above on its own.
  auto const linked = members(conn, "org:acmecorp");
  REQUIRE(linked.has_value());
  REQUIRE(linked->size() == 1);
  CHECK(linked->front().root_path == std::optional<std::string>{"/w/repo"});

  // Re-applying is idempotent: same rows, no duplicate, no error.
  REQUIRE(apply_proposals(conn, proposals, "/w/repo").has_value());
  auto const again = list_all(conn);
  REQUIRE(again.has_value());
  CHECK(again->size() == listed->size());
}

TEST_CASE("apply_proposals: an operator-created association survives untouched", "[association][detect][apply]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  REQUIRE(create(conn, create_args{.slug = "seed"}).has_value());
  REQUIRE(add_member(conn, "seed", "/w/repo").has_value());

  // A HUMAN made this one: its own name, and `auto_detected` false.
  REQUIRE(create(conn, create_args{.slug = "org:acmecorp", .name = "Hand Made", .kind = association_kind::org}).has_value());
  auto const before = show_by_slug(conn, "org:acmecorp");
  REQUIRE(before.has_value());
  REQUIRE(before->name == "Hand Made"); // the seed took
  REQUIRE_FALSE(before->auto_detected);

  auto proposals = proposals_from_signals(detect_signals{.git_remote = "git@github.com:AcmeCorp/My_Repo.git"});
  REQUIRE(proposals.size() == 2);
  REQUIRE(apply_proposals(conn, proposals, "/w/repo").has_value());

  // `insert or ignore`, NOT an upsert: the name and the flag both survive.
  // An upsert would have rewritten `name` to the slug and the flag to 1,
  // silently relabelling an operator's association as machine-generated.
  auto const after = show_by_slug(conn, "org:acmecorp");
  REQUIRE(after.has_value());
  CHECK(after->name == "Hand Made");
  CHECK_FALSE(after->auto_detected);
  CHECK(after->id == before->id); // the same row, not a replacement

  // The MEMBERSHIP was still created against that pre-existing row -- so
  // "survives untouched" is about the association's columns, not about
  // apply declining to do its job.
  auto const linked = members(conn, "org:acmecorp");
  REQUIRE(linked.has_value());
  CHECK(linked->size() == 1);
}
