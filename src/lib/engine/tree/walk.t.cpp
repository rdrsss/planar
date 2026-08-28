// @file walk.t.cpp
// @brief Unit tests for `planar.engine.tree.walk` (plan 996, task 6278).
//
// ## ORACLE PROVENANCE — every expected byte below was CAPTURED, not typed
//
// `tree`'s entire product is a rendered hierarchy, so a fixture that is
// shallow, narrow, or empty passes VACUOUSLY: the connectors, the indent
// extensions and the dirs-first grouping are only exercised by a tree with
// real depth AND siblings at more than one level. Two earlier cycles in
// this milestone were lost to exactly that (task 6256's scope-resolver
// suite passed against an empty answer; task 6090's absence assertion
// passed because the field could never have been present), so the fixture
// here is deliberately over-shaped for its size and every test that
// asserts an ABSENCE also asserts the corresponding PRESENCE.
//
// The transcript that produced the expected bytes:
//
// ```
// # migrate a scratch DB, then seed it with the identical SQL `seed()`
// # runs below, then run the oracle against it. Both streams piped —
// # the zig writer uses positional writes and an unpiped second write
// # lands at offset 0 and eats the first (see src/cmd/parity_harness.hpp).
// $ zig/zig-out/bin/planar init --json --allow-no-repo     # migrates
// $ sqlite3 $DB < fixture.sql                              # the seed below
// $ zig/zig-out/bin/planar tree --scope acme
// $ zig/zig-out/bin/planar tree --scope acme --json
// $ zig/zig-out/bin/planar tree --scope acme --kind task
// $ zig/zig-out/bin/planar tree --scope acme --kind artifact
// $ zig/zig-out/bin/planar tree --scope acme --depth 1
// $ zig/zig-out/bin/planar tree --scope acme --status draft
// $ zig/zig-out/bin/planar tree --scope acme --status done
// $ zig/zig-out/bin/planar tree --scope global
// ```
//
// Fixture shape — three plan levels, siblings at TWO of them, a subtask,
// and all four derived kinds hanging off plan 1:
//
// ```
// assoc:acme
// |-- plan 1 alpha  [draft]        + question/decision/scenario/artifact
// |   |-- plan 3 alpha-m1 [draft]
// |   |   `-- plan 5 alpha-sub     tasks 1 [todo], 2 [doing]
// |   |                              `-- task 3 [todo] under task 2
// |   `-- plan 4 alpha-m2 [draft]  (deliberately EMPTY)
// `-- plan 2 beta   [active]       task 4 [done]
// ```
//
// ## HAZARDS these cases are shaped around
//
// 1. `--kind artifact` renders an EMPTY tree even though the artifact
//    exists and is linked. That is the scaffold rule, not a bug: a plan
//    that survives only on derived leaves is dropped. A test that asserted
//    "the artifact appears" would be asserting the opposite of the
//    contract. Both directions are pinned below.
// 2. The summary footer pluralises per-count, so a fixture with 2+ of
//    everything cannot discriminate `plan` from `plans`. `--status done`
//    yields exactly one plan and one task and is kept for that reason.
// 3. The derived leaves are ordered by `entity_links.id`, NOT by kind and
//    NOT by entity id. The seed inserts them in a non-kind-sorted order so
//    a kind-sorted implementation would visibly diverge.
// 4. `--status ''` must match NOTHING, not everything. It is the empty
//    SET that means "any", and the empty STRING is a real value that no
//    row here carries.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.db.migrate;
import planar.engine.tree.walk;

namespace {

namespace tree = planar::engine::tree;

// @brief A scratch DB path that removes itself and its WAL siblings.
struct scratch_db_path {
  std::filesystem::path path_;

  scratch_db_path()
      : path_(std::filesystem::temp_directory_path() / std::format("planar_tree_test_{}_{}.db",
                                                                   std::chrono::steady_clock::now().time_since_epoch().count(),
                                                                   reinterpret_cast<std::uintptr_t>(this))) {
  }
  scratch_db_path(const scratch_db_path&)                    = delete;
  auto operator=(const scratch_db_path&) -> scratch_db_path& = delete;
  scratch_db_path(scratch_db_path&&)                         = delete;
  auto operator=(scratch_db_path&&) -> scratch_db_path&      = delete;

  ~scratch_db_path() {
    std::error_code ec;
    for (auto const* suffix : {"", "-journal", "-wal", "-shm"}) {
      std::filesystem::remove(std::filesystem::path{path_.string() + suffix}, ec);
    }
  }
};

auto open_migrated(const scratch_db_path& scratch) -> planar::db::connection {
  auto conn = planar::db::connection::open(scratch.path_.string());
  REQUIRE(conn.has_value());
  auto applied = planar::db::apply_all(*conn);
  REQUIRE(applied.has_value());
  return std::move(*conn);
}

auto exec(planar::db::connection& conn, std::string_view sql) -> void {
  auto ok = conn.execute(sql);
  REQUIRE(ok.has_value());
}

// @brief Seed the fixture documented in this file's header.
auto seed(planar::db::connection& conn) -> void {
  exec(conn, "insert into associations (id, slug, name, kind, created_at, updated_at) "
             "values (1, 'acme', 'Acme', 'project', '2026-01-01T00:00:00.000Z', '2026-01-01T00:00:00.000Z')");

  exec(conn,
       "insert into plans (id, scope_kind, scope_id, title, slug, status, parent_plan_id, created_at, updated_at) values"
       " (1,'association',1,'Alpha feature','alpha','draft',null,'2026-01-01T00:00:01.000Z','2026-01-01T00:00:01.000Z'),"
       " (2,'association',1,'Beta feature','beta','active',null,'2026-01-01T00:00:02.000Z','2026-01-01T00:00:02.000Z'),"
       " (3,'association',1,'Alpha M1','alpha-m1','draft',1,'2026-01-01T00:00:03.000Z','2026-01-01T00:00:03.000Z'),"
       " (4,'association',1,'Alpha M2','alpha-m2','draft',1,'2026-01-01T00:00:04.000Z','2026-01-01T00:00:04.000Z'),"
       " (5,'association',1,'Alpha M1 sub','alpha-m1-sub','draft',3,'2026-01-01T00:00:05.000Z','2026-01-01T00:00:05.000Z')");

  exec(conn,
       "insert into tasks (id, scope_kind, scope_id, plan_id, parent_task_id, title, status, priority, created_at, updated_at) "
       "values"
       " (1,'association',1,5,null,'Write the walker','todo',100,'2026-01-01T00:00:06.000Z','2026-01-01T00:00:06.000Z'),"
       " (2,'association',1,5,null,'Write the renderer','doing',50,'2026-01-01T00:00:07.000Z','2026-01-01T00:00:07.000Z'),"
       " (3,'association',1,5,2,'Renderer sub-step','todo',100,'2026-01-01T00:00:08.000Z','2026-01-01T00:00:08.000Z'),"
       " (4,'association',1,2,null,'Beta task','done',10,'2026-01-01T00:00:09.000Z','2026-01-01T00:00:09.000Z')");

  exec(conn, "insert into questions (id, scope_kind, scope_id, title, status, created_at, updated_at) values "
             "(1,'association',1,'Is the glyph set stable?','open','2026-01-01T00:00:10.000Z','2026-01-01T00:00:10.000Z')");
  exec(conn, "insert into decisions (id, scope_kind, scope_id, title, body, status, created_at, updated_at) values "
             "(1,'association',1,'Use unicode box glyphs','Chosen.','proposed','2026-01-01T00:00:11.000Z','2026-01-01T00:00:11."
             "000Z')");
  exec(conn, "insert into test_scenarios (id, scope_kind, scope_id, title, status, created_at, updated_at) values "
             "(1,'association',1,'renders three levels','draft','2026-01-01T00:00:12.000Z','2026-01-01T00:00:12.000Z')");
  exec(conn, "insert into artifacts (id, scope_kind, scope_id, title, kind, status, created_at, updated_at) values "
             "(1,'association',1,'tech-spec','tech_spec','draft','2026-01-01T00:00:13.000Z','2026-01-01T00:00:13.000Z')");

  // Inserted in a NON-kind-sorted order on purpose: the render must follow
  // entity_links.id, so a kind-sorted walk would reorder these visibly.
  exec(conn, "insert into entity_links (id, from_kind, from_id, to_kind, to_id, relationship, created_at) values"
             " (1,'question',1,'plan',1,'derives-from','2026-01-01T00:00:14.000Z'),"
             " (2,'decision',1,'plan',1,'derives-from','2026-01-01T00:00:15.000Z'),"
             " (3,'test_scenario',1,'plan',1,'derives-from','2026-01-01T00:00:16.000Z'),"
             " (4,'artifact',1,'plan',1,'derives-from','2026-01-01T00:00:17.000Z')");
}

// @brief Add the `sessions` row that `agent_actions.session_id` and
// `agent_work_claims.session_id` both require (`not null references
// sessions(id)`, and foreign keys are ON).
//
// Seeding activity without this silently rejects the row and leaves the
// test asserting against an entity that has no activity — which is
// indistinguishable from a walker that never populates the field. `exec`
// REQUIREs, so a rejected insert fails here rather than three assertions
// later.
auto seed_session(planar::db::connection& conn) -> void {
  exec(conn, "insert into sessions (id, vendor, started_at) values (1, 'claude', '2026-02-01T00:00:00.000Z')");
}

// @brief Walk the `acme` association scope with `filter`.
auto walk_acme(planar::db::connection& conn, tree::tree_filter filter) -> std::vector<tree::node> {
  filter.scope = "acme";
  auto built   = tree::build(conn, filter);
  REQUIRE(built.has_value());
  return *built;
}

// @brief Total node count under a root, excluding the root itself.
auto descendant_count(const tree::node& n) -> std::size_t {
  std::size_t total = n.children.size();
  for (auto const& c : n.children) {
    total += descendant_count(c);
  }
  return total;
}

} // namespace

TEST_CASE("tree.walk: the fixture is DEEP and WIDE enough to discriminate", "[tree]") {
  // The anti-vacuity gate. Every byte-for-byte case below is meaningless if
  // the fixture renders a stub, so this asserts the shape those cases rely
  // on BEFORE any of them run: three plan levels, siblings at two of them,
  // a nested subtask, and four derived leaves.
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  auto const roots = walk_acme(conn, {});
  REQUIRE(roots.size() == 1);
  auto const& root = roots[0];

  CHECK(root.kind == "scope");
  CHECK(root.children.size() == 2);               // plan 1 and plan 2 are siblings
  CHECK(descendant_count(root) == 13);            // 5 plans + 4 tasks + 4 derived
  REQUIRE(root.children[0].children.size() == 6); // 2 child plans + 4 derived

  // Depth: scope -> plan1 -> plan3 -> plan5 -> task2 -> task3 is five
  // levels below the root. A two-level fixture cannot exercise the indent
  // extension at all.
  auto const& plan3 = root.children[0].children[0];
  REQUIRE(plan3.id == 3);
  REQUIRE(plan3.children.size() == 1);
  auto const& plan5 = plan3.children[0];
  REQUIRE(plan5.id == 5);
  REQUIRE(plan5.children.size() == 2); // task 1 and task 2 are siblings
  auto const& task2 = plan5.children[1];
  REQUIRE(task2.id == 2);
  CHECK(task2.children.size() == 1); // task 3 nested under task 2
}

TEST_CASE("tree.walk: render_text matches the oracle byte for byte", "[tree]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  auto const roots = walk_acme(conn, {});

  // Captured from `planar tree --scope acme`. Note the blank line before
  // the footer and the trailing newline; the payload is COMPLETE.
  constexpr std::string_view k_expected = "assoc:acme\n"
                                          "├── plan:1 [draft]  Alpha feature\n"
                                          "│   ├── plan:3 [draft]  Alpha M1\n"
                                          "│   │   └── plan:5 [draft]  Alpha M1 sub\n"
                                          "│   │       ├── task:1  Write the walker  [todo, pri:100]\n"
                                          "│   │       └── task:2  Write the renderer  [doing, pri:50]\n"
                                          "│   │           └── task:3  Renderer sub-step  [todo, pri:100]\n"
                                          "│   ├── plan:4 [draft]  Alpha M2\n"
                                          "│   ├── question:1  Is the glyph set stable?  [open]\n"
                                          "│   ├── decision:1  Use unicode box glyphs  [proposed]\n"
                                          "│   ├── scenario:1  renders three levels  [draft]\n"
                                          "│   └── artifact:1  tech-spec  [tech_spec, draft]\n"
                                          "└── plan:2 [active]  Beta feature\n"
                                          "    └── task:4  Beta task  [done, pri:10]\n"
                                          "\n"
                                          "5 plans, 4 tasks, 1 artifact, 1 decision, 1 scenario, 1 question\n";

  CHECK(tree::render_text(roots) == k_expected);
}

TEST_CASE("tree.walk: derived leaves follow entity_links.id, not kind order", "[tree]") {
  // The seed links question(1), decision(2), scenario(3), artifact(4) in
  // that el.id order. A walk that grouped by kind, or ordered by the
  // entity's own id, would produce the same SET in a different sequence —
  // and the set is what a careless test would check.
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  auto const  roots = walk_acme(conn, {});
  auto const& plan1 = roots[0].children[0];
  auto const& kids  = plan1.children;
  REQUIRE(kids.size() == 6);

  // The two child PLANS sort ahead of the derived leaves in the walk's own
  // output (plans are gathered first), then the four leaves in el.id order.
  CHECK(kids[0].kind == "plan");
  CHECK(kids[1].kind == "plan");
  CHECK(kids[2].kind == "question");
  CHECK(kids[3].kind == "decision");
  CHECK(kids[4].kind == "scenario");
  CHECK(kids[5].kind == "artifact");
}

TEST_CASE("tree.walk: test_scenario is stored under that name but DISPLAYS as scenario", "[tree]") {
  // Asserted against the UNFILTERED tree on purpose. The obvious spelling —
  // filter to `--kind scenario` and look for the row — cannot work here,
  // and finding that out is what the next case pins: the scaffold rule
  // collapses every derived-only filter to an empty tree, so that version
  // of this test would have "passed" its absence half against nothing at
  // all while its presence half failed.
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  auto const  roots = walk_acme(conn, {});
  auto const& plan1 = roots[0].children.at(0);

  // Presence: exactly one node carries the DISPLAY spelling.
  CHECK(std::ranges::count_if(plan1.children, [](tree::node const& n) { return n.kind == "scenario"; }) == 1);
  // Absence: and none carries the DB spelling. Meaningful only because the
  // presence check above proves the row is reachable at all.
  CHECK(std::ranges::none_of(plan1.children, [](tree::node const& n) { return n.kind == "test_scenario"; }));
  // `--kind scenario` is nonetheless the ACCEPTED flag spelling, and
  // `test_scenario` is not.
  CHECK(tree::is_valid_kind("scenario"));
  CHECK_FALSE(tree::is_valid_kind("test_scenario"));
}

TEST_CASE("tree.walk: EVERY derived-only kind filter collapses the tree", "[tree]") {
  // The general form of the artifact case, captured from the oracle for
  // all four derived kinds rather than generalised from one. A plan is
  // kept as scaffold only by a plan-or-task descendant, and each of these
  // four entities is neither — so filtering to any one of them alone
  // renders a bare scope label and a zeroed footer, at exit 0.
  //
  // This is the single most surprising thing about the verb and the
  // easiest to "fix" by accident.
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  constexpr std::string_view k_empty = "assoc:acme\n"
                                       "\n"
                                       "0 plans, 0 tasks, 0 artifacts, 0 decisions, 0 scenarios, 0 questions\n";

  for (auto const* kind : {"artifact", "decision", "scenario", "question"}) {
    INFO("derived kind: " << kind);
    auto const roots = walk_acme(conn, tree::tree_filter{.kinds = {std::string{kind}}});
    REQUIRE(roots.size() == 1);
    CHECK(roots[0].children.empty());
    CHECK(tree::render_text(roots) == k_empty);
  }

  // Non-vacuous: all four entities ARE present in the unfiltered tree, so
  // the four empty results above are the scaffold rule at work and not a
  // fixture that forgot to seed them.
  auto const  all  = walk_acme(conn, {});
  auto const& kids = all[0].children.at(0).children;
  for (auto const* kind : {"artifact", "decision", "scenario", "question"}) {
    INFO("present in unfiltered tree: " << kind);
    CHECK(std::ranges::any_of(kids, [kind](tree::node const& n) { return n.kind == kind; }));
  }
}

TEST_CASE("tree.walk: --kind artifact renders NOTHING, because of the scaffold rule", "[tree]") {
  // The counter-intuitive one, and the reason it is pinned in BOTH
  // directions. The artifact exists and is linked to plan 1 — the previous
  // cases prove that — but a plan kept only by derived leaves is dropped,
  // so filtering to artifacts alone collapses the whole tree.
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  // Presence: the artifact IS in the unfiltered tree.
  auto const  all = walk_acme(conn, {});
  auto const& p1  = all[0].children.at(0);
  REQUIRE(std::ranges::any_of(p1.children, [](tree::node const& n) { return n.kind == "artifact"; }));

  // Absence: and yet `--kind artifact` yields an empty scope root.
  auto const only_artifacts = walk_acme(conn, tree::tree_filter{.kinds = {"artifact"}});
  REQUIRE(only_artifacts.size() == 1);
  CHECK(only_artifacts[0].children.empty());

  constexpr std::string_view k_expected = "assoc:acme\n"
                                          "\n"
                                          "0 plans, 0 tasks, 0 artifacts, 0 decisions, 0 scenarios, 0 questions\n";
  CHECK(tree::render_text(only_artifacts) == k_expected);
}

TEST_CASE("tree.walk: --kind task keeps the plan spine as scaffold", "[tree]") {
  // The mirror of the artifact case: a plan filtered out on its own merits
  // SURVIVES when it holds a task descendant. plan 4 (no tasks) drops;
  // plans 1, 3, 5 and 2 stay.
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  auto const roots = walk_acme(conn, tree::tree_filter{.kinds = {"task"}});

  constexpr std::string_view k_expected = "assoc:acme\n"
                                          "├── plan:1 [draft]  Alpha feature\n"
                                          "│   └── plan:3 [draft]  Alpha M1\n"
                                          "│       └── plan:5 [draft]  Alpha M1 sub\n"
                                          "│           ├── task:1  Write the walker  [todo, pri:100]\n"
                                          "│           └── task:2  Write the renderer  [doing, pri:50]\n"
                                          "│               └── task:3  Renderer sub-step  [todo, pri:100]\n"
                                          "└── plan:2 [active]  Beta feature\n"
                                          "    └── task:4  Beta task  [done, pri:10]\n"
                                          "\n"
                                          "4 plans, 4 tasks, 0 artifacts, 0 decisions, 0 scenarios, 0 questions\n";
  CHECK(tree::render_text(roots) == k_expected);

  // Non-vacuous: plan 4 is genuinely present in the DB and genuinely
  // absent from the render. Without this the case would also pass against
  // a walk that dropped plan 4 for the wrong reason.
  auto const all = walk_acme(conn, {});
  CHECK(std::ranges::any_of(all[0].children.at(0).children, [](tree::node const& n) { return n.id == 4; }));
}

TEST_CASE("tree.walk: the summary footer pluralises per count", "[tree]") {
  // `--status done` is kept precisely because it yields exactly ONE plan
  // and ONE task: a fixture with two of everything cannot tell `1 plan`
  // from `1 plans`.
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  auto const roots = walk_acme(conn, tree::tree_filter{.statuses = {"done"}});

  constexpr std::string_view k_expected = "assoc:acme\n"
                                          "└── plan:2 [active]  Beta feature\n"
                                          "    └── task:4  Beta task  [done, pri:10]\n"
                                          "\n"
                                          "1 plan, 1 task, 0 artifacts, 0 decisions, 0 scenarios, 0 questions\n";
  CHECK(tree::render_text(roots) == k_expected);
}

TEST_CASE("tree.walk: --depth caps descent; any value <= 0 is UNBOUNDED", "[tree]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  auto const                 depth1     = walk_acme(conn, tree::tree_filter{.max_depth = 1});
  constexpr std::string_view k_expected = "assoc:acme\n"
                                          "├── plan:1 [draft]  Alpha feature\n"
                                          "│   ├── plan:3 [draft]  Alpha M1\n"
                                          "│   ├── plan:4 [draft]  Alpha M2\n"
                                          "│   ├── question:1  Is the glyph set stable?  [open]\n"
                                          "│   ├── decision:1  Use unicode box glyphs  [proposed]\n"
                                          "│   ├── scenario:1  renders three levels  [draft]\n"
                                          "│   └── artifact:1  tech-spec  [tech_spec, draft]\n"
                                          "└── plan:2 [active]  Beta feature\n"
                                          "    └── task:4  Beta task  [done, pri:10]\n"
                                          "\n"
                                          "4 plans, 1 task, 1 artifact, 1 decision, 1 scenario, 1 question\n";
  CHECK(tree::render_text(depth1) == k_expected);

  // `--depth 0` is NOT "zero levels" — it is unbounded, identical to -1.
  // The oracle's guard is `max_depth <= 0`, and reading it as "0 means
  // nothing" is the obvious wrong assumption.
  auto const unbounded = tree::render_text(walk_acme(conn, {}));
  CHECK(tree::render_text(walk_acme(conn, tree::tree_filter{.max_depth = 0})) == unbounded);
  CHECK(tree::render_text(walk_acme(conn, tree::tree_filter{.max_depth = -1})) == unbounded);
  // ...and unbounded is genuinely DEEPER than depth 1, so the equality
  // above is not two empty strings agreeing.
  CHECK(unbounded != tree::render_text(depth1));
}

TEST_CASE("tree.walk: an empty --status set means ANY, an empty status VALUE means nothing", "[tree]") {
  // The trap this verb sets three different ways. An empty `statuses`
  // vector is "no filter"; a one-element vector holding "" is a filter for
  // the literal empty string, which no row here carries.
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  auto const unfiltered = walk_acme(conn, {});
  CHECK(descendant_count(unfiltered[0]) == 13);

  auto const empty_value = walk_acme(conn, tree::tree_filter{.statuses = {""}});
  REQUIRE(empty_value.size() == 1);
  CHECK(empty_value[0].children.empty());

  // An unknown status behaves the same way — exit 0, empty tree, NOT a
  // refusal. Only `--kind` validates its argument.
  auto const unknown = walk_acme(conn, tree::tree_filter{.statuses = {"nosuchstatus"}});
  CHECK(unknown[0].children.empty());
}

TEST_CASE("tree.walk: an unknown kind REFUSES rather than matching nothing", "[tree]") {
  // The asymmetry with `--status` above is the contract: a typo'd kind is
  // an error, a typo'd status is an empty result.
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  auto const bogus = tree::build(conn, tree::tree_filter{.scope = "acme", .kinds = {"bogus"}});
  REQUIRE_FALSE(bogus.has_value());
  CHECK(bogus.error() == tree::tree_error::unknown_kind);

  // The EMPTY kind is a reachable case and refuses the same way — it does
  // NOT mean "all kinds".
  auto const empty = tree::build(conn, tree::tree_filter{.scope = "acme", .kinds = {""}});
  REQUIRE_FALSE(empty.has_value());
  CHECK(empty.error() == tree::tree_error::unknown_kind);
  CHECK(tree::render_unknown_kind("") == "unknown kind ''");
  CHECK(tree::render_unknown_kind("bogus") == "unknown kind 'bogus'");

  // Non-vacuous: a VALID kind on the same fixture succeeds, so the two
  // refusals above are not just "build always fails here".
  CHECK(tree::build(conn, tree::tree_filter{.scope = "acme", .kinds = {"plan"}}).has_value());
}

TEST_CASE("tree.walk: every kind in valid_kinds is accepted, and the set is closed", "[tree]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  REQUIRE(tree::valid_kinds().size() == 6);
  for (auto const kind : tree::valid_kinds()) {
    INFO("kind: " << kind);
    CHECK(tree::is_valid_kind(kind));
    CHECK(tree::build(conn, tree::tree_filter{.scope = "acme", .kinds = {std::string{kind}}}).has_value());
  }
  // `test_scenario` is the DB spelling and is deliberately NOT accepted on
  // the flag; `scenario` is.
  CHECK_FALSE(tree::is_valid_kind("test_scenario"));
  CHECK(tree::is_valid_kind("scenario"));
}

TEST_CASE("tree.walk: an unknown scope slug is slug_not_found, an empty scope is GLOBAL", "[tree]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  auto const missing = tree::build(conn, tree::tree_filter{.scope = "no-such-scope"});
  REQUIRE_FALSE(missing.has_value());
  CHECK(missing.error() == tree::tree_error::slug_not_found);

  // An UNSET scope is global — and global here is a real, empty scope, not
  // an error and not the association's rows.
  auto const global = tree::build(conn, tree::tree_filter{});
  REQUIRE(global.has_value());
  REQUIRE(global->size() == 1);
  CHECK((*global)[0].scope_label == "global");
  CHECK((*global)[0].children.empty());

  constexpr std::string_view k_expected = "global\n"
                                          "\n"
                                          "0 plans, 0 tasks, 0 artifacts, 0 decisions, 0 scenarios, 0 questions\n";
  CHECK(tree::render_text(*global) == k_expected);
}

TEST_CASE("tree.walk: an empty scope yields a LABELLED root, not an empty vector", "[tree]") {
  // The difference matters to the renderer: an empty vector would print no
  // label at all, where the oracle prints the scope line and a zeroed
  // footer. Exit 0 either way, which is what makes it easy to get wrong.
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);
  exec(conn, "insert into associations (id, slug, name, kind, created_at, updated_at) "
             "values (2, 'empty-org', 'Empty', 'org', '2026-01-01T00:00:00.000Z', '2026-01-01T00:00:00.000Z')");

  auto const roots = walk_acme(conn, {});
  REQUIRE(roots.size() == 1);

  auto const empty = tree::build(conn, tree::tree_filter{.scope = "empty-org"});
  REQUIRE(empty.has_value());
  REQUIRE(empty->size() == 1);
  CHECK((*empty)[0].kind == "scope");
  CHECK((*empty)[0].scope_label == "assoc:empty-org");
  CHECK((*empty)[0].children.empty());
}

TEST_CASE("tree.walk: --all-scopes emits global, then associations and projects BY SLUG", "[tree]") {
  // Ordering here is a plain SQL `order by slug` — there is no hash-map
  // iteration anywhere in this walk, so unlike task 6274's `languages`
  // field this order is reproducible and safe to pin.
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);
  // Inserted in REVERSE slug order so an id-ordered walk would diverge.
  exec(conn, "insert into associations (id, slug, name, kind, created_at, updated_at) values "
             "(2,'zeta','Zeta','org','2026-01-01T00:00:00.000Z','2026-01-01T00:00:00.000Z'),"
             "(3,'beta-org','Beta','org','2026-01-01T00:00:00.000Z','2026-01-01T00:00:00.000Z')");
  exec(conn, "insert into projects (id, slug, name, root_path, created_at, updated_at) values "
             "(1,'zed','zed','/tmp/zed','2026-01-01T00:00:00.000Z','2026-01-01T00:00:00.000Z'),"
             "(2,'apex','apex','/tmp/apex','2026-01-01T00:00:00.000Z','2026-01-01T00:00:00.000Z')");

  auto const built = tree::build(conn, tree::tree_filter{.all_scopes = true});
  REQUIRE(built.has_value());
  auto const& roots = *built;

  REQUIRE(roots.size() == 6); // global + 3 associations + 2 projects
  CHECK(roots[0].scope_label == "global");
  CHECK(roots[1].scope_label == "assoc:acme"); // acme < beta-org < zeta
  CHECK(roots[2].scope_label == "assoc:beta-org");
  CHECK(roots[3].scope_label == "assoc:zeta");
  CHECK(roots[4].scope_label == "repo:apex"); // apex < zed, though ids are 2, 1
  CHECK(roots[5].scope_label == "repo:zed");

  // The association carrying the fixture still renders its whole subtree,
  // so this is not six empty labels agreeing.
  CHECK(descendant_count(roots[1]) == 13);
}

TEST_CASE("tree.walk: render_json emits an OBJECT for one root and an ARRAY for many", "[tree]") {
  // The asymmetry is the oracle's and consumers depend on it, so it is
  // reproduced rather than normalised to an array.
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  auto const one = walk_acme(conn, {});
  REQUIRE(one.size() == 1);
  auto const single = tree::render_json(one);
  CHECK(single.starts_with("{\"kind\":\"scope\""));
  CHECK(single.ends_with("\n"));

  auto const many = tree::build(conn, tree::tree_filter{.all_scopes = true});
  REQUIRE(many.has_value());
  REQUIRE(many->size() > 1);
  auto const multi = tree::render_json(*many);
  CHECK(multi.starts_with("[{\"kind\":\"scope\""));
  CHECK(multi.ends_with("]\n"));
}

TEST_CASE("tree.walk: render_json matches the oracle byte for byte", "[tree]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  // Captured from `planar tree --scope acme --json`. A scope root emits
  // ONLY the scope fields (no id/slug/status/priority) and an entity node
  // emits only the entity fields — the split the oracle's custom
  // serializer exists to enforce (task 2377).
  constexpr std::string_view k_expected =
      R"({"kind":"scope","title":"assoc:acme","scope_kind":"association","scope_id":1,"scope_label":"assoc:acme","children":[{"kind":"plan","id":1,"title":"Alpha feature","slug":"alpha","status":"draft","priority":0,"artifact_kind":"","created_at":"2026-01-01T00:00:01.000Z","updated_at":"2026-01-01T00:00:01.000Z","children":[{"kind":"plan","id":3,"title":"Alpha M1","slug":"alpha-m1","status":"draft","priority":0,"artifact_kind":"","created_at":"2026-01-01T00:00:03.000Z","updated_at":"2026-01-01T00:00:03.000Z","children":[{"kind":"plan","id":5,"title":"Alpha M1 sub","slug":"alpha-m1-sub","status":"draft","priority":0,"artifact_kind":"","created_at":"2026-01-01T00:00:05.000Z","updated_at":"2026-01-01T00:00:05.000Z","children":[{"kind":"task","id":1,"title":"Write the walker","slug":"","status":"todo","priority":100,"artifact_kind":"","created_at":"2026-01-01T00:00:06.000Z","updated_at":"2026-01-01T00:00:06.000Z","children":[]},{"kind":"task","id":2,"title":"Write the renderer","slug":"","status":"doing","priority":50,"artifact_kind":"","created_at":"2026-01-01T00:00:07.000Z","updated_at":"2026-01-01T00:00:07.000Z","children":[{"kind":"task","id":3,"title":"Renderer sub-step","slug":"","status":"todo","priority":100,"artifact_kind":"","created_at":"2026-01-01T00:00:08.000Z","updated_at":"2026-01-01T00:00:08.000Z","children":[]}]}]}]},{"kind":"plan","id":4,"title":"Alpha M2","slug":"alpha-m2","status":"draft","priority":0,"artifact_kind":"","created_at":"2026-01-01T00:00:04.000Z","updated_at":"2026-01-01T00:00:04.000Z","children":[]},{"kind":"question","id":1,"title":"Is the glyph set stable?","slug":"","status":"open","priority":0,"artifact_kind":"","created_at":"2026-01-01T00:00:10.000Z","updated_at":"2026-01-01T00:00:10.000Z","children":[]},{"kind":"decision","id":1,"title":"Use unicode box glyphs","slug":"","status":"proposed","priority":0,"artifact_kind":"","created_at":"2026-01-01T00:00:11.000Z","updated_at":"2026-01-01T00:00:11.000Z","children":[]},{"kind":"scenario","id":1,"title":"renders three levels","slug":"","status":"draft","priority":0,"artifact_kind":"","created_at":"2026-01-01T00:00:12.000Z","updated_at":"2026-01-01T00:00:12.000Z","children":[]},{"kind":"artifact","id":1,"title":"tech-spec","slug":"","status":"draft","priority":0,"artifact_kind":"tech_spec","created_at":"2026-01-01T00:00:13.000Z","updated_at":"2026-01-01T00:00:13.000Z","children":[]}]},{"kind":"plan","id":2,"title":"Beta feature","slug":"beta","status":"active","priority":0,"artifact_kind":"","created_at":"2026-01-01T00:00:02.000Z","updated_at":"2026-01-01T00:00:02.000Z","children":[{"kind":"task","id":4,"title":"Beta task","slug":"","status":"done","priority":10,"artifact_kind":"","created_at":"2026-01-01T00:00:09.000Z","updated_at":"2026-01-01T00:00:09.000Z","children":[]}]}]})"
      "\n";

  CHECK(tree::render_json(walk_acme(conn, {})) == k_expected);
}

TEST_CASE("tree.walk: activity_summary is OMITTED when there is none, and present when there is", "[tree]") {
  // Both directions, because an "omitted" assertion alone passes for a
  // walker that never populates the field at all — which is exactly the
  // shape of defect task 6090 recorded.
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  // Absence: nothing seeded into agent_actions / agent_work_claims yet.
  auto const quiet = walk_acme(conn, {});
  CHECK_FALSE(quiet[0].children.at(0).activity.has_value());
  CHECK(tree::render_json(quiet).find("activity_summary") == std::string::npos);
  CHECK(tree::render_text(quiet).find("activity:") == std::string::npos);

  // Presence. `agent_actions.outcome` is CHECK-constrained to
  // ok|error|aborted|timeout and `entity_kind` to plan|plan_step|task —
  // a row violating either is silently rejected and the fixture then
  // proves nothing, which has already cost this milestone one cycle.
  seed_session(conn);
  exec(conn, "insert into agent_actions (id, session_id, entity_kind, entity_id, action_kind, vendor, started_at, ended_at, "
             "outcome) values (1, 1, 'task', 1, 'coder', 'claude', '2026-02-01T00:00:00.000Z', '2026-02-01T00:05:00.000Z', "
             "'ok')");

  auto const  loud  = walk_acme(conn, {});
  auto const& task1 = loud[0].children.at(0).children.at(0).children.at(0).children.at(0);
  REQUIRE(task1.id == 1);
  REQUIRE(task1.activity.has_value());
  CHECK(task1.activity->latest_action_kind == "coder");
  CHECK(task1.activity->latest_vendor == "claude");
  CHECK(task1.activity->last_event_at == "2026-02-01T00:05:00.000Z"); // ended_at wins over started_at
  CHECK(task1.activity->active_claim_count == 0);                     // an action is not a claim

  CHECK(tree::render_text(loud).find("activity: coder via claude @ 2026-02-01T00:05:00.000Z  [claims:0]\n") != std::string::npos);
  CHECK(tree::render_json(loud).find(R"("activity_summary":{"latest_action_kind":"coder","latest_vendor":"claude",)"
                                     R"("last_event_at":"2026-02-01T00:05:00.000Z","active_claim_count":0})") !=
        std::string::npos);
}

TEST_CASE("tree.walk: a claim with no action falls back to the literal word 'claim'", "[tree]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  // A claim but NO action: the rollup reports an empty action kind, which
  // the text renderer prints as `claim` so the line stays parseable.
  seed_session(conn);
  exec(conn, "insert into agent_work_claims (id, claim_token, session_id, entity_kind, entity_id, vendor, status, claimed_at, "
             "lease_expires_at) values (1, 'tok1', 1, 'task', 1, 'codex', 'active', '2026-03-01T00:00:00.000Z', "
             "'2099-01-01T00:00:00.000Z')");

  auto const  roots = walk_acme(conn, {});
  auto const& task1 = roots[0].children.at(0).children.at(0).children.at(0).children.at(0);
  REQUIRE(task1.id == 1);
  REQUIRE(task1.activity.has_value());
  CHECK(task1.activity->latest_action_kind.empty());
  CHECK(task1.activity->latest_vendor == "codex");
  CHECK(task1.activity->active_claim_count == 1);

  CHECK(tree::render_text(roots).find("activity: claim via codex @ 2026-03-01T00:00:00.000Z  [claims:1]\n") != std::string::npos);
  // The JSON keeps the EMPTY string rather than substituting `claim`; the
  // substitution is a rendering concern only.
  CHECK(tree::render_json(roots).find(R"("latest_action_kind":"","latest_vendor":"codex")") != std::string::npos);
}

TEST_CASE("tree.walk: an EXPIRED claim still counts as activity but not as an active claim", "[tree]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);
  seed_session(conn);
  exec(conn, "insert into agent_work_claims (id, claim_token, session_id, entity_kind, entity_id, vendor, status, claimed_at, "
             "lease_expires_at) values (1, 'tok1', 1, 'task', 1, 'codex', 'active', '2026-03-01T00:00:00.000Z', "
             "'2000-01-01T00:00:00.000Z')");

  auto const  roots = walk_acme(conn, {});
  auto const& task1 = roots[0].children.at(0).children.at(0).children.at(0).children.at(0);
  REQUIRE(task1.activity.has_value());            // the row still surfaces as activity
  CHECK(task1.activity->active_claim_count == 0); // but the lease has lapsed
}

TEST_CASE("tree.walk: a title longer than 80 BYTES truncates with an ellipsis", "[tree]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  // 77 bytes survive and a 3-byte `…` follows, for 80 bytes of title.
  std::string const long_title(120, 'L');
  exec(conn, std::format("insert into plans (id, scope_kind, scope_id, title, slug, status, parent_plan_id, created_at, "
                         "updated_at) values (9,'association',1,'{}','longtitle','draft',null,"
                         "'2026-01-01T00:00:20.000Z','2026-01-01T00:00:20.000Z')",
                         long_title));

  auto const rendered = tree::render_text(walk_acme(conn, {}));
  CHECK(rendered.find(std::string(77, 'L') + "…") != std::string::npos);
  // The untruncated form must NOT appear — and the 77-char prefix above
  // proves the plan is really in the render, so this absence is meaningful.
  CHECK(rendered.find(std::string(78, 'L')) == std::string::npos);

  // A title exactly at the limit is NOT truncated: the guard is `<=`.
  exec(conn, std::format("insert into plans (id, scope_kind, scope_id, title, slug, status, parent_plan_id, created_at, "
                         "updated_at) values (10,'association',1,'{}','exactly80','draft',null,"
                         "'2026-01-01T00:00:21.000Z','2026-01-01T00:00:21.000Z')",
                         std::string(80, 'X')));
  auto const with_exact = tree::render_text(walk_acme(conn, {}));
  CHECK(with_exact.find(std::string(80, 'X') + "\n") != std::string::npos);
  CHECK(with_exact.find(std::string(80, 'X') + "…") == std::string::npos);
}

TEST_CASE("tree.walk: root_plan_id narrows to one top-level plan", "[tree]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  auto const roots = walk_acme(conn, tree::tree_filter{.root_plan_id = 2});
  REQUIRE(roots.size() == 1);
  REQUIRE(roots[0].children.size() == 1);
  CHECK(roots[0].children[0].id == 2);

  // Non-vacuous: plan 1 is a real top-level sibling that the unfiltered
  // walk returns.
  auto const all = walk_acme(conn, {});
  CHECK(all[0].children.size() == 2);
}

TEST_CASE("tree.walk: a task linked by derives-from joins a plan it does not belong to", "[tree]") {
  // The `left join` arm of query_top_tasks: a plan's task set is NOT just
  // `tasks.plan_id`, it also includes tasks joined by a `derives-from`
  // edge. Without the join the task simply does not appear under plan 4.
  //
  // On the `distinct`: it cannot be discriminated by inserting the same
  // edge twice, because `entity_links` carries
  // `unique (from_kind, from_id, to_kind, to_id, relationship)` and the
  // second insert is REJECTED. A test that tried it would be asserting
  // against an unreachable state — so what is pinned instead is the
  // reachable overlap, a task matching BOTH arms of the `or` at once
  // (`plan_id = 4` AND an edge to plan 4), which is the case `distinct`
  // actually guards.
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  // Task 4 belongs to plan 2. Link it to plan 4 as well.
  exec(conn, "insert into entity_links (id, from_kind, from_id, to_kind, to_id, relationship, created_at) values "
             "(10,'task',4,'plan',4,'derives-from','2026-01-01T00:00:30.000Z')");

  auto const  roots = walk_acme(conn, {});
  auto const& plan4 = roots[0].children.at(0).children.at(1);
  REQUIRE(plan4.id == 4);
  REQUIRE(plan4.children.size() == 1);
  CHECK(plan4.children[0].id == 4);

  // And it still appears under its OWN plan — the edge adds, not moves.
  auto const& plan2 = roots[0].children.at(1);
  REQUIRE(plan2.id == 2);
  REQUIRE(plan2.children.size() == 1);
  CHECK(plan2.children[0].id == 4);

  // Both arms of the `or` satisfied at once: task 1 has plan_id = 5 AND an
  // edge to plan 5. It must appear exactly ONCE, not twice.
  exec(conn, "insert into entity_links (id, from_kind, from_id, to_kind, to_id, relationship, created_at) values "
             "(11,'task',1,'plan',5,'derives-from','2026-01-01T00:00:31.000Z')");
  auto const  again = walk_acme(conn, {});
  auto const& plan5 = again[0].children.at(0).children.at(0).children.at(0);
  REQUIRE(plan5.id == 5);
  CHECK(std::ranges::count_if(plan5.children, [](tree::node const& n) { return n.id == 1; }) == 1);
  CHECK(plan5.children.size() == 2); // tasks 1 and 2, still
}
