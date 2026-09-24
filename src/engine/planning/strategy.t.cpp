// @file strategy.t.cpp
// @brief Engine-level tests for `planar.engine.planning.strategy` (plan 996,
// task 6310).
//
// The CLI-visible bytes for both leaves are pinned in
// `src/cmd/planar/plan_strategy_leaves.t.cpp`. This file covers what the
// leaf tests cannot reach cleanly: the pure classifiers, and the engine
// entry points driven directly against a scratch database so a rule can be
// isolated without a whole CLI arena around it.
//
// The two entry points are exercised SEPARATELY on the same fixtures. They
// share a loader and disagree about nearly everything downstream, so a test
// that checked `recommend` and inferred `compute_divergence` would be
// asserting the assumption this cycle exists to disprove.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.db.migrate;
import planar.engine.planning.strategy;

namespace {

namespace st = planar::engine::planning::strategy;

/// @brief A scratch database path, removed with its sidecars on destruction.
struct scratch_db_path {
  std::filesystem::path path_;
  scratch_db_path()
      : path_(std::filesystem::temp_directory_path() / std::format("planar_strategy_eng_{}_{}",
                                                                   std::chrono::steady_clock::now().time_since_epoch().count(),
                                                                   reinterpret_cast<std::uintptr_t>(this))) {
  }
  scratch_db_path(const scratch_db_path&)            = delete;
  scratch_db_path& operator=(const scratch_db_path&) = delete;
  ~scratch_db_path() {
    std::error_code ec;
    for (auto const* suffix : {"", "-journal", "-wal", "-shm"}) {
      std::filesystem::remove(path_.string() + suffix, ec);
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

/// @brief Seed a plan and return its id.
auto seed_plan(planar::db::connection& conn) -> std::int64_t {
  exec(conn, "insert into plans (scope_kind, title, slug, status) values ('global','P','p','active')");
  auto stmt = conn.prepare("select id from plans where slug='p'");
  REQUIRE(stmt.has_value());
  auto step = stmt->step();
  REQUIRE(step.has_value());
  REQUIRE(*step == planar::db::step_result::row);
  return stmt->column_int64(0);
}

/// @brief Seed a `todo` task and return its id.
auto seed_task(planar::db::connection& conn, std::int64_t plan_id, std::string_view title) -> std::int64_t {
  exec(conn, std::format("insert into tasks (scope_kind, plan_id, title, status, priority) "
                         "values ('global',{},'{}','todo',100)",
                         plan_id, title));
  auto stmt = conn.prepare(std::format("select id from tasks where title='{}'", title));
  REQUIRE(stmt.has_value());
  auto step = stmt->step();
  REQUIRE(step.has_value());
  REQUIRE(*step == planar::db::step_result::row);
  return stmt->column_int64(0);
}

auto seed_repo(planar::db::connection& conn, std::string_view slug) -> std::int64_t {
  exec(conn, std::format("insert into projects (slug,name,root_path) values ('{0}','{0}','/tmp/{0}')", slug));
  auto stmt = conn.prepare(std::format("select id from projects where slug='{}'", slug));
  REQUIRE(stmt.has_value());
  auto step = stmt->step();
  REQUIRE(step.has_value());
  REQUIRE(*step == planar::db::step_result::row);
  return stmt->column_int64(0);
}

auto touch_path(planar::db::connection& conn, std::int64_t task, std::int64_t repo, std::string_view path) -> void {
  exec(conn, std::format("insert into task_touch_paths (task_id,repo_id,path) values ({},{},'{}')", task, repo, path));
}

auto touch_repo(planar::db::connection& conn, std::int64_t task, std::int64_t repo) -> void {
  exec(conn, std::format("insert into entity_links (from_kind,from_id,to_kind,to_id,relationship) "
                         "values ('task',{},'repo',{},'touches')",
                         task, repo));
}

auto seed_closure(planar::db::connection& conn, std::int64_t task, std::int64_t repo, std::string_view symbol,
                  std::string_view role) -> void {
  exec(conn, std::format("insert into closures (task_id,repo_id,path,symbol,role,token_weight,extractor_version) "
                         "values ({},{},'p.zig','{}','{}',0,'t')",
                         task, repo, symbol, role));
}

/// @brief Does `refs` contain a task carrying an exclusion for `rule`?
auto has_rule(const std::vector<st::task_ref>& refs, std::int64_t id, std::uint8_t rule) -> bool {
  for (auto const& r : refs) {
    if (r.id != id) {
      continue;
    }
    for (auto const& e : r.excluded_by) {
      if (e.rule == rule) {
        return true;
      }
    }
  }
  return false;
}

} // namespace

TEST_CASE("strategy: the two path classifiers match the oracle's prefix/suffix rules", "[planning]") {
  // Rule 3 needs BOTH a `migrations/` prefix and a `.sql` suffix.
  CHECK(st::is_migration_path("migrations/00016_foo.sql"));
  CHECK_FALSE(st::is_migration_path("migrations/README.md")); // wrong suffix
  CHECK_FALSE(st::is_migration_path("src/migrations/x.sql")); // not a prefix
  CHECK_FALSE(st::is_migration_path("migrations/"));
  CHECK_FALSE(st::is_migration_path(""));

  // Rule 4 is exact-match against a fixed list, not a prefix test.
  CHECK(st::is_singleton_file("CLAUDE.md"));
  CHECK(st::is_singleton_file("AGENTS.md"));
  CHECK(st::is_singleton_file("docs/cli-reference.md"));
  CHECK(st::is_singleton_file("docs/architecture.md"));
  CHECK_FALSE(st::is_singleton_file("docs/concepts.md"));
  CHECK_FALSE(st::is_singleton_file("sub/CLAUDE.md"));
  CHECK(st::singleton_files().size() == 4);
}

TEST_CASE("strategy: closure_source parses exactly two tokens", "[planning]") {
  CHECK(st::parse_closure_source("declared") == st::closure_source::declared);
  CHECK(st::parse_closure_source("derived") == st::closure_source::derived);
  CHECK_FALSE(st::parse_closure_source("bogus").has_value());
  CHECK_FALSE(st::parse_closure_source("").has_value());
  CHECK_FALSE(st::parse_closure_source("Declared").has_value()); // case-sensitive
  CHECK(st::closure_source_name(st::closure_source::declared) == "declared");
  CHECK(st::closure_source_name(st::closure_source::derived) == "derived");
}

TEST_CASE("strategy: both entry points report not_found for a plan that does not exist", "[planning]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto const rec = st::recommend(conn, 9999);
  REQUIRE_FALSE(rec.has_value());
  CHECK(rec.error() == st::strategy_error::not_found);

  auto const div = st::compute_divergence(conn, 9999);
  REQUIRE_FALSE(div.has_value());
  CHECK(div.error() == st::strategy_error::not_found);
}

TEST_CASE("strategy: disjoint repo touches leave both tasks eligible and enable fan-out", "[planning]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto const      plan = seed_plan(conn);
  auto const      t1   = seed_task(conn, plan, "A");
  auto const      t2   = seed_task(conn, plan, "B");
  touch_repo(conn, t1, seed_repo(conn, "ra"));
  touch_repo(conn, t2, seed_repo(conn, "rb"));

  auto const rec = st::recommend(conn, plan);
  REQUIRE(rec.has_value());
  CHECK(rec->parallel_eligible.size() == 2);
  CHECK(rec->serialized.empty());
  CHECK(rec->fan_out_available);
  CHECK(rec->open_tasks == 2);
  CHECK(st::recommended_note(*rec) == "parallel-fanout available: 2 eligible tasks");
}

TEST_CASE("strategy: fan_out_available needs TWO eligible tasks, and the note says which", "[planning]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto const      plan = seed_plan(conn);
  auto const      t1   = seed_task(conn, plan, "solo");
  touch_repo(conn, t1, seed_repo(conn, "ra"));

  auto const rec = st::recommend(conn, plan);
  REQUIRE(rec.has_value());
  CHECK(rec->parallel_eligible.size() == 1);
  // One eligible task is NOT fan-out, and its note differs from the zero case.
  CHECK_FALSE(rec->fan_out_available);
  CHECK(st::recommended_note(*rec) == "no fan-out: only 1 eligible task; run sequentially");
}

TEST_CASE("strategy: two paths in one repo are disjoint but the same path is not", "[planning]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto const      plan = seed_plan(conn);
  auto const      t1   = seed_task(conn, plan, "A");
  auto const      t2   = seed_task(conn, plan, "B");
  auto const      repo = seed_repo(conn, "r");

  SECTION("different paths in the same repo") {
    touch_path(conn, t1, repo, "src/foo.zig");
    touch_path(conn, t2, repo, "src/bar.zig");
    auto const rec = st::recommend(conn, plan);
    REQUIRE(rec.has_value());
    CHECK(rec->parallel_eligible.size() == 2);
  }

  SECTION("the same path in the same repo drops BOTH") {
    touch_path(conn, t1, repo, "src/foo.zig");
    touch_path(conn, t2, repo, "src/foo.zig");
    auto const rec = st::recommend(conn, plan);
    REQUIRE(rec.has_value());
    CHECK(rec->parallel_eligible.empty());
    CHECK(rec->serialized.size() == 2);
    CHECK(has_rule(rec->serialized, t1, 2));
    CHECK(has_rule(rec->serialized, t2, 2));
  }

  SECTION("a whole-repo claim subsumes a same-repo path touch") {
    touch_path(conn, t1, repo, "src/foo.zig");
    touch_repo(conn, t2, repo);
    auto const rec = st::recommend(conn, plan);
    REQUIRE(rec.has_value());
    CHECK(rec->parallel_eligible.empty());
    CHECK(has_rule(rec->serialized, t1, 2));
    CHECK(has_rule(rec->serialized, t2, 2));
  }
}

TEST_CASE("strategy: a repo with any path declaration contributes no whole-repo touch", "[planning]") {
  // Task A declares BOTH a coarse repo edge and a path on that repo. The path
  // detail wins, so A's touch set is the path alone -- which is why B's
  // different path in the same repo does NOT collide.
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto const      plan = seed_plan(conn);
  auto const      t1   = seed_task(conn, plan, "A");
  auto const      t2   = seed_task(conn, plan, "B");
  auto const      repo = seed_repo(conn, "r");
  touch_repo(conn, t1, repo);
  touch_path(conn, t1, repo, "src/foo.zig");
  touch_path(conn, t2, repo, "src/bar.zig");

  auto const rec = st::recommend(conn, plan);
  REQUIRE(rec.has_value());
  CHECK(rec->parallel_eligible.size() == 2);
}

TEST_CASE("strategy: rule 2 does not cascade through a unilaterally dropped peer", "[planning]") {
  // A touches a migration (rule 3, unilateral). B's ONLY overlap is with A.
  // The pairwise pass runs over survivors only, so B stays eligible and A
  // carries rule 3 WITHOUT a rule-2 cascade.
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto const      plan = seed_plan(conn);
  auto const      t_a  = seed_task(conn, plan, "migrator");
  auto const      t_b  = seed_task(conn, plan, "B");
  auto const      repo = seed_repo(conn, "r");
  touch_repo(conn, t_a, repo);
  touch_path(conn, t_a, repo, "migrations/00099_x.sql");
  touch_repo(conn, t_b, repo);

  auto const rec = st::recommend(conn, plan);
  REQUIRE(rec.has_value());
  REQUIRE(rec->parallel_eligible.size() == 1);
  CHECK(rec->parallel_eligible[0].id == t_b);
  REQUIRE(rec->serialized.size() == 1);
  CHECK(rec->serialized[0].id == t_a);
  CHECK(has_rule(rec->serialized, t_a, 3));
  CHECK_FALSE(has_rule(rec->serialized, t_a, 2));
}

TEST_CASE("strategy: rule 1 reads the not-done set, which is wider than the todo candidate set", "[planning]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto const      plan = seed_plan(conn);
  auto const      dep  = seed_task(conn, plan, "dependant");
  auto const      blk  = seed_task(conn, plan, "blocker");
  auto const      repo = seed_repo(conn, "r");
  touch_path(conn, dep, repo, "a.zig");
  touch_path(conn, blk, repo, "b.zig");
  exec(conn, std::format("insert into entity_links (from_kind,from_id,to_kind,to_id,relationship) "
                         "values ('task',{},'task',{},'depends-on')",
                         dep, blk));

  SECTION("a todo blocker fires rule 1") {
    auto const rec = st::recommend(conn, plan);
    REQUIRE(rec.has_value());
    CHECK(has_rule(rec->serialized, dep, 1));
  }

  SECTION("a doing blocker still fires rule 1 though it is no longer a candidate") {
    exec(conn, std::format("update tasks set status='doing' where id={}", blk));
    auto const rec = st::recommend(conn, plan);
    REQUIRE(rec.has_value());
    CHECK(rec->open_tasks == 1); // the blocker dropped out of the candidates
    CHECK(has_rule(rec->serialized, dep, 1));
  }

  SECTION("a done blocker does NOT fire rule 1") {
    exec(conn, std::format("update tasks set status='done' where id={}", blk));
    auto const rec = st::recommend(conn, plan);
    REQUIRE(rec.has_value());
    CHECK(rec->open_tasks == 1);
    CHECK_FALSE(has_rule(rec->serialized, dep, 1));
    CHECK(rec->parallel_eligible.size() == 1); // freed, and now eligible
  }

  SECTION("a cancelled blocker does NOT fire rule 1 either") {
    exec(conn, std::format("update tasks set status='cancelled' where id={}", blk));
    auto const rec = st::recommend(conn, plan);
    REQUIRE(rec.has_value());
    CHECK_FALSE(has_rule(rec->serialized, dep, 1));
  }
}

TEST_CASE("strategy: rules 5 and 6 fire only on unresolved linked entities", "[planning]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto const      plan = seed_plan(conn);
  auto const      t1   = seed_task(conn, plan, "A");
  auto const      repo = seed_repo(conn, "r");
  touch_path(conn, t1, repo, "a.zig");

  SECTION("an OPEN question drops the task, an answered one does not") {
    exec(conn, "insert into questions (scope_kind,title,status) values ('global','Q','open')");
    exec(conn, std::format("insert into entity_links (from_kind,from_id,to_kind,to_id,relationship) "
                           "values ('task',{},'question',(select id from questions where title='Q'),'addresses')",
                           t1));
    auto const open_rec = st::recommend(conn, plan);
    REQUIRE(open_rec.has_value());
    CHECK(has_rule(open_rec->serialized, t1, 5));

    // The paired presence/absence: resolving the SAME question frees the task,
    // which proves the assertion above was not passing on a missing link.
    //
    // `answer_body` and `answered_at` are set alongside the status because
    // the table CHECKs them together -- `update ... set status='answered'`
    // alone is rejected, which is how this seed first failed.
    exec(conn, "update questions set status='answered', answer_body='a', "
               "answered_at='2026-01-01T00:00:00.000Z' where title='Q'");
    auto const closed_rec = st::recommend(conn, plan);
    REQUIRE(closed_rec.has_value());
    CHECK_FALSE(has_rule(closed_rec->serialized, t1, 5));
    CHECK(closed_rec->parallel_eligible.size() == 1);
  }

  SECTION("a PROPOSED decision drops the task, an accepted one does not") {
    // Note the edge points decision -> task, the opposite direction from the
    // question case: the rule matches EITHER orientation.
    exec(conn, "insert into decisions (scope_kind,title,body,status) values ('global','D','b','proposed')");
    exec(conn, std::format("insert into entity_links (from_kind,from_id,to_kind,to_id,relationship) "
                           "values ('decision',(select id from decisions where title='D'),'task',{},'depends-on')",
                           t1));
    auto const proposed_rec = st::recommend(conn, plan);
    REQUIRE(proposed_rec.has_value());
    CHECK(has_rule(proposed_rec->serialized, t1, 6));

    exec(conn, "update decisions set status='accepted' where title='D'");
    auto const accepted_rec = st::recommend(conn, plan);
    REQUIRE(accepted_rec.has_value());
    CHECK_FALSE(has_rule(accepted_rec->serialized, t1, 6));
    CHECK(accepted_rec->parallel_eligible.size() == 1);
  }
}

TEST_CASE("strategy: divergence measures rule 2 alone and ignores every unilateral rule", "[planning]") {
  // The sharpest disagreement between the two entry points. Task A trips
  // rules 3, 4, 5 and 6; `recommend` serializes it, and `compute_divergence`
  // does not notice any of that -- it only ever asks whether pairs overlap.
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto const      plan = seed_plan(conn);
  auto const      t_a  = seed_task(conn, plan, "loaded");
  auto const      t_b  = seed_task(conn, plan, "clean");
  auto const      repo = seed_repo(conn, "r");
  touch_path(conn, t_a, repo, "migrations/00001_a.sql");
  touch_path(conn, t_a, repo, "CLAUDE.md");
  touch_path(conn, t_b, repo, "b.zig");
  exec(conn, "insert into questions (scope_kind,title,status) values ('global','Q','open')");
  exec(conn, std::format("insert into entity_links (from_kind,from_id,to_kind,to_id,relationship) "
                         "values ('task',{},'question',(select id from questions where title='Q'),'addresses')",
                         t_a));
  exec(conn, "insert into decisions (scope_kind,title,body,status) values ('global','D','b','proposed')");
  exec(conn, std::format("insert into entity_links (from_kind,from_id,to_kind,to_id,relationship) "
                         "values ('decision',(select id from decisions where title='D'),'task',{},'depends-on')",
                         t_a));

  auto const rec = st::recommend(conn, plan);
  REQUIRE(rec.has_value());
  CHECK(has_rule(rec->serialized, t_a, 3));
  CHECK(has_rule(rec->serialized, t_a, 4));
  CHECK(has_rule(rec->serialized, t_a, 5));
  CHECK(has_rule(rec->serialized, t_a, 6));

  auto const div = st::compute_divergence(conn, plan);
  REQUIRE(div.has_value());
  CHECK(div->open_tasks == 2); // nothing was dropped
  CHECK(div->pairs == 1);
  CHECK(div->declared_overlaps == 0); // distinct paths
  CHECK(div->derived_overlaps == 0);  // no closures at all
  CHECK(div->flips == 0);
  CHECK(div->jaccard == 0.0); // empty union -> 0.0 by definition, not by division
}

TEST_CASE("strategy: an empty touch set drops a task in recommend but is inert in divergence", "[planning]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto const      plan = seed_plan(conn);
  auto const      t1   = seed_task(conn, plan, "A");
  auto const      t2   = seed_task(conn, plan, "B");

  auto const rec = st::recommend(conn, plan);
  REQUIRE(rec.has_value());
  CHECK(rec->parallel_eligible.empty());
  CHECK(has_rule(rec->serialized, t1, 2));
  CHECK(has_rule(rec->serialized, t2, 2));

  auto const div = st::compute_divergence(conn, plan);
  REQUIRE(div.has_value());
  CHECK(div->pairs == 1);
  CHECK(div->declared_overlaps == 0); // an empty set conflicts with nothing
  CHECK(div->flips == 0);
}

TEST_CASE("strategy: a shared derived symbol flips rule 2, and transitive rows do not", "[planning]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto const      plan = seed_plan(conn);
  auto const      t1   = seed_task(conn, plan, "A");
  auto const      t2   = seed_task(conn, plan, "B");
  auto const      repo = seed_repo(conn, "r");
  touch_path(conn, t1, repo, "src/foo.zig");
  touch_path(conn, t2, repo, "src/bar.zig");

  SECTION("a shared reference symbol overlaps under derived and flips") {
    seed_closure(conn, t1, repo, "shared.helper", "reference");
    seed_closure(conn, t2, repo, "shared.helper", "reference");

    auto const declared = st::recommend_with(conn, plan, st::closure_source::declared);
    REQUIRE(declared.has_value());
    CHECK(declared->parallel_eligible.size() == 2);

    auto const derived = st::recommend_with(conn, plan, st::closure_source::derived);
    REQUIRE(derived.has_value());
    CHECK(derived->parallel_eligible.empty());
    CHECK(has_rule(derived->serialized, t1, 2));

    auto const div = st::compute_divergence(conn, plan);
    REQUIRE(div.has_value());
    CHECK(div->declared_overlaps == 0);
    CHECK(div->derived_overlaps == 1);
    CHECK(div->flips == 1);
    CHECK(div->jaccard == 1.0);
  }

  SECTION("a shared TRANSITIVE symbol is excluded from the effective closure") {
    // Paired with the section above so the zero cannot be a missing fixture.
    seed_closure(conn, t1, repo, "deep.thing", "transitive");
    seed_closure(conn, t2, repo, "deep.thing", "transitive");

    auto const derived = st::recommend_with(conn, plan, st::closure_source::derived);
    REQUIRE(derived.has_value());
    CHECK(derived->parallel_eligible.size() == 2);

    auto const div = st::compute_divergence(conn, plan);
    REQUIRE(div.has_value());
    CHECK(div->derived_overlaps == 0);
    CHECK(div->flips == 0);
    CHECK(div->jaccard == 0.0);
  }
}

TEST_CASE("strategy: recommend is the declared wrapper and rule 2 is the only branch that switches", "[planning]") {
  // Under `derived`, rules 3 and 4 still read the DECLARED paths -- a
  // migration touch is a declared-path property, not a closure property. A
  // port that swapped the whole touch set would lose these two rules entirely
  // whenever a task had closures.
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto const      plan = seed_plan(conn);
  auto const      t1   = seed_task(conn, plan, "migrator");
  auto const      repo = seed_repo(conn, "r");
  touch_path(conn, t1, repo, "migrations/00001_a.sql");
  seed_closure(conn, t1, repo, "some.symbol", "modify");

  auto const derived = st::recommend_with(conn, plan, st::closure_source::derived);
  REQUIRE(derived.has_value());
  CHECK(has_rule(derived->serialized, t1, 3));

  auto const declared = st::recommend_with(conn, plan, st::closure_source::declared);
  auto const plain    = st::recommend(conn, plan);
  REQUIRE(declared.has_value());
  REQUIRE(plain.has_value());
  CHECK(plain->parallel_eligible.size() == declared->parallel_eligible.size());
  CHECK(plain->serialized.size() == declared->serialized.size());
}

TEST_CASE("strategy: the renderers carry the trailing newline and the two jaccard formats", "[planning]") {
  st::divergence_result div{
      .open_tasks = 4, .pairs = 6, .declared_overlaps = 2, .derived_overlaps = 3, .flips = 1, .jaccard = 1.0 / 3.0};

  auto const json = st::render_divergence_json(6, div);
  CHECK(json == R"({"plan_id":6,"open_tasks":4,"pairs":6,"declared_overlaps":2,)"
                R"("derived_overlaps":3,"flips":1,"jaccard":0.3333333333333333})"
                "\n");

  auto const text = st::render_divergence_text(6, div);
  CHECK(text == "plan:6  open:4  pairs:6  declared_overlaps:2  derived_overlaps:3  flips:1  jaccard:0.3333\n");

  // The integral cases are where the two formats differ most visibly.
  st::divergence_result zero{};
  CHECK(st::render_divergence_json(1, zero).contains(R"("jaccard":0})"));
  CHECK(st::render_divergence_text(1, zero).contains("jaccard:0.0000\n"));
}

TEST_CASE("strategy: the recommendation JSON escapes titles and renders a null slug", "[planning]") {
  st::recommendation rec{
      .plan_id           = 7,
      .parallel_eligible = {st::task_ref{.id = 1, .slug = std::nullopt, .title = "He said \"hi\"\tand \\ that"}},
      .serialized        = {},
      .open_tasks        = 1,
      .fan_out_available = false,
  };
  auto const json = st::render_recommendation_json(rec, st::closure_source::declared);
  CHECK(json.contains(R"({"id":1,"slug":null,"title":"He said \"hi\"\tand \\ that"})"));
  CHECK(json.ends_with("\n"));
  CHECK(json.contains(R"("recommended_note":"no fan-out: only 1 eligible task; run sequentially")"));
}
