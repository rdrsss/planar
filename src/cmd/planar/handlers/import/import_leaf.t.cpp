// @file import_leaf.t.cpp
// @brief Transactional, end-to-end evidence for the interpreted `import` core.
//
// The cache is deliberately seeded only after the real handler has staged its
// pending request.  That keeps the fixture tied to the production fingerprint
// and makes the preview meaningful: it contains two phase/task proposals, an
// imported decision, and three forward-spec proposals, yet leaves SQLite
// unopened.  The same cache then drives apply twice (idempotency) and a
// malformed mid-reconciliation variant (atomic rollback).

#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.cmd.planar.context;
import planar.cmd.planar.dispatch;
import planar.cmd.planar.main;

#include "json_envelope_test_support.hpp"

namespace {

using planar::cmd::context;

struct invocation {
  int         code;
  std::string out;
  std::string err;
  bool        db_open;
};
struct fixture {
  std::filesystem::path                           root;
  std::map<std::string, std::string, std::less<>> vars;
  std::filesystem::path                           db_path;
};

auto make_fixture(std::string_view tag) -> fixture {
  auto            root = std::filesystem::temp_directory_path() /
                         std::format("planar_import_{}_{}", tag, std::chrono::steady_clock::now().time_since_epoch().count());
  std::error_code ec;
  for (auto const& part : {"home", "proj", "repo", "fakehome"})
    std::filesystem::create_directories(root / part, ec);
  return {.root    = root,
          .vars    = {{"PLANAR_HOME", (root / "home").string()},
                      {"HOME", (root / "fakehome").string()},
                      {"PWD", (root / "proj").string()}},
          .db_path = root / "planar.db"};
}

auto dispatch(const fixture& fx, std::vector<std::string> args) -> invocation {
  std::vector<std::string> argv{"planar"};
  argv.insert(argv.end(), args.begin(), args.end());
  std::ostringstream out, err;
  context            ctx{std::move(argv),
                         planar::cmd::map_env(fx.vars),
                         fx.root / "proj",
                         std::make_shared<planar::cmd::database>(fx.db_path, err),
                         out,
                         err};
  auto               tree  = planar::cmd::root_app();
  auto               table = planar::cmd::make_handler_table(*tree);
  int                code  = planar::cmd::run(ctx, *tree, table);
  return {.code = code, .out = out.str(), .err = err.str(), .db_open = ctx.db().opened()};
}

auto read(const std::filesystem::path& path) -> std::string {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}
auto write(const std::filesystem::path& path, std::string_view body) -> void {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  REQUIRE(out.good());
  out << body;
  REQUIRE(out.good());
}
auto field(std::string_view json, std::string_view name) -> std::string {
  auto const key   = std::format("\"{}\":\"", name);
  auto const start = json.find(key);
  REQUIRE(start != std::string_view::npos);
  auto const first = start + key.size();
  auto const last  = json.find('"', first);
  REQUIRE(last != std::string_view::npos);
  return std::string{json.substr(first, last - first)};
}
auto query_count(planar::db::connection& conn, std::string_view sql) -> std::int64_t {
  auto stmt = conn.prepare(sql);
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->step().has_value());
  return stmt->column_int64(0);
}
auto query_text(planar::db::connection& conn, std::string_view sql) -> std::string {
  auto stmt = conn.prepare(sql);
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->step().has_value());
  return stmt->column_text(0);
}
auto inventory(const fixture& fx) -> std::string {
  auto conn = planar::db::connection::open(fx.db_path.string());
  REQUIRE(conn.has_value());
  return std::format("plans={} tasks={} artifacts={} decisions={} links={}", query_count(*conn, "select count(*) from plans"),
                     query_count(*conn, "select count(*) from tasks"), query_count(*conn, "select count(*) from artifacts"),
                     query_count(*conn, "select count(*) from decisions"),
                     query_count(*conn, "select count(*) from entity_links"));
}
/// @brief The fixture cache.
///
/// `bad_decision` drops the decision's `body`, which is what makes
/// reconciliation fail PART WAY THROUGH — the property the rollback case
/// exists to prove.
///
/// It used to be an invalid task STATUS instead. That stopped working at task
/// 6405: envelope validation now rejects an unknown task status, so the cache
/// never reached reconciliation and the rollback path went uncovered while the
/// case still passed for the wrong reason. A missing decision body is checked
/// by `reconcile_cache` and deliberately NOT by the envelope (the contract in
/// `agents/planar-importer.md` says nothing about it), and decisions reconcile
/// AFTER plans and tasks — so there are real prior writes to roll back.
auto cache_body(std::string_view fingerprint, bool bad_decision = false) -> std::string {
  return std::format(
      R"({{"schema_version":1,"fingerprint":"{}","anchor_title":"Imported Anchor","provenance":"fixture","phases":[{{"slug":"phase-one","title":"Phase One","status":"active","tasks":[{{"slug":"task-one","title":"Task One","status":"todo"}}]}},{{"slug":"phase-two","title":"Phase Two","status":"draft","tasks":[{{"slug":"task-two","title":"Task Two","status":"doing"}}]}}],"decisions":[{{"title":"Keep transaction"{}}}],"forward_specs":[{{"slug":"forward-a","title":"Forward A"}},{{"slug":"forward-b","title":"Forward B"}},{{"slug":"forward-c","title":"Forward C"}}]}})",
      fingerprint, bad_decision ? "" : R"(,"body":"Every reconciliation write is atomic.")");
}

auto stage_cache(const fixture& fx, bool bad_decision = false) -> std::filesystem::path {
  write(fx.root / "repo" / "README.md", "# Imported fixture\n");
  write(fx.root / "repo" / "docs" / "tech-spec.md", "# Imported tech spec\n");
  auto preview = dispatch(fx, {"import", (fx.root / "repo").string(), "--interpret", "--json"});
  REQUIRE(preview.code == 0);
  REQUIRE(preview.err.empty());
  REQUIRE_FALSE(preview.db_open);
  CHECK(preview.out.contains("\"mode\":\"pending\""));
  CHECK_FALSE(preview.out.contains("\"plans_created\"")); // cache has not been applied.
  // Pin the oracle's own cache layout (`planar.engine.llm.client.cachePath`):
  // `<planar_home>/cache/import-interpretation/<repo_slug>/<fingerprint>.json`,
  // the same scheme `bootstrap-synthesis` uses. An earlier
  // `llm/import-interpretation/...` layout would write the real
  // `pl-import` vendor-skill handoff somewhere this binary never reads back.
  CHECK(
      preview.out.contains(std::format("\"cache_path\":\"{}", (fx.root / "home" / "cache" / "import-interpretation").string())));
  auto const fingerprint = field(preview.out, "fingerprint");
  // Read the cache path back off the JSON contract rather than
  // reconstructing the layout by hand: the fixture must exercise wherever
  // the handler actually looks, not a copy of that logic that can drift
  // from it silently.
  auto const cache = std::filesystem::path{field(preview.out, "cache_path")};
  write(cache, cache_body(fingerprint, bad_decision));
  // This second preview contains a non-empty proposal cache but must still
  // perform no database write. It falsifies a handler that treats cache-hit
  // as implicit apply.
  auto cached_preview = dispatch(fx, {"import", (fx.root / "repo").string(), "--interpret", "--json"});
  REQUIRE(cached_preview.code == 0);
  REQUIRE_FALSE(cached_preview.db_open);
  CHECK(cached_preview.out.contains("\"mode\":\"cache_hit\""));
  CHECK(read(cache).contains("phase-one"));
  CHECK_FALSE(std::filesystem::exists(fx.db_path));
  return cache;
}

} // namespace

TEST_CASE("interpreted import applies its cache atomically and is idempotent", "[cmd][import][transaction][idempotent]") {
  auto const fx = make_fixture("apply");
  static_cast<void>(stage_cache(fx));

  auto const first = dispatch(fx, {"import", (fx.root / "repo").string(), "--interpret", "--apply", "--json"});
  REQUIRE(first.code == 0);
  CHECK(first.err.empty());
  CHECK(first.out.contains("\"mode\":\"cache_hit\""));
  CHECK(first.out.contains("\"applied\":"));

  auto const after_first = inventory(fx);
  CHECK(after_first == "plans=3 tasks=2 artifacts=2 decisions=1 links=3");
  {
    auto conn = planar::db::connection::open(fx.db_path.string());
    REQUIRE(conn.has_value());
    CHECK(query_count(*conn, "select count(*) from entity_links where relationship='derives-from' and from_kind='artifact'") ==
          2);
    CHECK(query_count(*conn, "select count(*) from entity_links where relationship='derives-from' and from_kind='decision'") ==
          1);
    CHECK(query_count(*conn, "select count(*) from plans where slug like 'forward-%' and status='draft'") == 0);
  }
  auto const second = dispatch(fx, {"import", (fx.root / "repo").string(), "--interpret", "--apply", "--json"});
  REQUIRE(second.code == 0);
  CHECK(second.err.empty());
  CHECK(inventory(fx) == after_first);
}

TEST_CASE("interpreted import rolls every prior write back when reconciliation fails mid-cache",
          "[cmd][import][transaction][rollback]") {
  auto const fx = make_fixture("rollback");
  static_cast<void>(stage_cache(fx, true));
  auto const rejected = dispatch(fx, {"import", (fx.root / "repo").string(), "--interpret", "--apply", "--json"});
  CHECK(rejected.code == 1);
  CHECK(rejected.out == planar::cmd::testsupport::json_error_envelope_line("import", "generic_failure"));
  CHECK(rejected.err == "error: invalid import arguments\n");
  CHECK(inventory(fx) == "plans=0 tasks=0 artifacts=0 decisions=0 links=0");
}

TEST_CASE("interpreted import applies proposed removals only when explicitly enabled", "[cmd][import][removals][survivors]") {
  auto const fx    = make_fixture("removals");
  auto const cache = stage_cache(fx);
  REQUIRE(dispatch(fx, {"import", (fx.root / "repo").string(), "--interpret", "--apply", "--json"}).code == 0);

  // The second interpretation retains phase/task one and its decision, while
  // phase/task two, the second decision, and tech-spec.md are genuinely
  // absent.  This fixture falsifies both an implicit-removal implementation
  // and one that cancels every row rather than subtracting survivors.
  auto       body   = read(cache);
  auto const needle = R"(}],"forward_specs")";
  auto const at     = body.find(needle);
  REQUIRE(at != std::string::npos);
  // `needle` starts on the retained decision's closing `}`; append inside
  // the array, after that object but before the array-closing `]`.
  body.insert(at + 1, R"(,{"title":"Drop decision","body":"removed by replacement"})");
  write(cache, body);
  REQUIRE(dispatch(fx, {"import", (fx.root / "repo").string(), "--interpret", "--apply", "--json"}).code == 0);
  std::error_code ec;
  std::filesystem::remove(fx.root / "repo" / "docs" / "tech-spec.md", ec);

  // The source tree is an input to the cache key.  Deleting the stale
  // artifact must therefore stage a new request and seed its *new*
  // fingerprinted cache, rather than attempting to smuggle an old cache
  // across the production freshness guard.
  auto const staged = dispatch(fx, {"import", (fx.root / "repo").string(), "--interpret", "--json"});
  REQUIRE(staged.code == 0);
  REQUIRE(staged.out.contains("\"mode\":\"pending\""));
  auto const next_fingerprint = field(staged.out, "fingerprint");
  auto       next             = cache_body(next_fingerprint);
  auto const phase_two        = next.find(R"(,{"slug":"phase-two")");
  REQUIRE(phase_two != std::string::npos);
  auto const phase_end = next.find(R"(}]}],"decisions")", phase_two);
  REQUIRE(phase_end != std::string::npos);
  // Erase the second phase through its task/object closers, retaining only
  // the final `]` that closes the phases array.
  next.erase(phase_two, phase_end + 3 - phase_two);
  auto const next_cache = std::filesystem::path{field(staged.out, "cache_path")};
  write(next_cache, next);

  auto const preview_apply = dispatch(fx, {"import", (fx.root / "repo").string(), "--interpret", "--apply", "--json"});
  REQUIRE(preview_apply.code == 0);
  {
    auto conn = planar::db::connection::open(fx.db_path.string());
    REQUIRE(conn.has_value());
    CHECK(query_text(*conn, "select status from tasks where slug='task-two'") == "doing");
    CHECK(query_text(*conn, "select status from plans where slug='phase-two'") == "draft");
    CHECK(query_text(*conn, "select status from artifacts where source_path='docs/tech-spec.md'") == "active");
    CHECK(query_text(*conn, "select status from decisions where title='Drop decision'") == "proposed");
    CHECK(query_text(*conn, "select status from tasks where slug='task-one'") == "todo");
  }
  REQUIRE(dispatch(fx, {"import", (fx.root / "repo").string(), "--interpret", "--apply", "--apply-removals", "--json"}).code ==
          0);
  {
    auto conn = planar::db::connection::open(fx.db_path.string());
    REQUIRE(conn.has_value());
    CHECK(query_text(*conn, "select status from tasks where slug='task-two'") == "cancelled");
    CHECK(query_text(*conn, "select status from plans where slug='phase-two'") == "abandoned");
    CHECK(query_text(*conn, "select status from artifacts where source_path='docs/tech-spec.md'") == "retired");
    CHECK(query_text(*conn, "select status from decisions where title='Drop decision'") == "superseded");
    CHECK(query_text(*conn, "select status from tasks where slug='task-one'") == "todo");
  }
  auto const after = inventory(fx);
  REQUIRE(dispatch(fx, {"import", (fx.root / "repo").string(), "--interpret", "--apply", "--apply-removals", "--json"}).code ==
          0);
  CHECK(inventory(fx) == after);
}

TEST_CASE("interpreted import forward-spec selection supports all CSV and none", "[cmd][import][forward_specs][selection]") {
  auto count_forwards = [](const fixture& fx) {
    auto conn = planar::db::connection::open(fx.db_path.string());
    REQUIRE(conn.has_value());
    return query_count(*conn, "select count(*) from plans where slug like 'forward-%'");
  };
  auto const all = make_fixture("forward_all");
  static_cast<void>(stage_cache(all));
  REQUIRE(dispatch(all, {"import", (all.root / "repo").string(), "--interpret", "--apply", "--accept-spec", "all"}).code == 0);
  CHECK(count_forwards(all) == 3);
  {
    auto conn = planar::db::connection::open(all.db_path.string());
    REQUIRE(conn.has_value());
    CHECK(query_count(*conn, "select count(*) from artifacts where source_path like 'pl-forward-spec://%'") == 9);
  }
  auto const csv = make_fixture("forward_csv");
  static_cast<void>(stage_cache(csv));
  REQUIRE(
      dispatch(csv, {"import", (csv.root / "repo").string(), "--interpret", "--apply", "--accept-spec", "forward-a,forward-c"})
          .code == 0);
  CHECK(count_forwards(csv) == 2);
  auto const none = make_fixture("forward_none");
  static_cast<void>(stage_cache(none));
  REQUIRE(dispatch(none, {"import", (none.root / "repo").string(), "--interpret", "--apply", "--no-forward-specs"}).code == 0);
  CHECK(count_forwards(none) == 0);
}

// --- task 6406: the `decisions` array-shape guard ---------------------------
//
// `reconcile_cache` requires `decisions` to be an ARRAY before iterating
// `decisions->array` downstream, so deleting that clause is null-deref-
// adjacent rather than merely permissive. It had no test: the guard could be
// removed outright with the suite green (task 6106 blind review).
//
// The envelope validator upstream (`cache_anchor`) does NOT inspect
// `decisions` at all, so a non-array value reaches this guard and nothing
// else -- which is what makes it the discriminating fixture.
TEST_CASE("interpreted import rejects a cache whose decisions field is not an array", "[cmd][import][6406]") {
  auto const fx    = make_fixture("decisions_shape");
  auto const cache = stage_cache(fx);

  // Swap ONLY the decisions value, array -> object, leaving every other
  // field of the known-good envelope byte-identical.
  auto const good = read(cache);
  auto const key  = std::string{R"("decisions":[)"};
  REQUIRE(good.contains(key));
  auto const open_at  = good.find(key) + key.size() - 1;
  auto const close_at = good.find(']', open_at);
  REQUIRE(close_at != std::string::npos);
  auto malformed = good;
  malformed.replace(open_at, close_at - open_at + 1, R"({"title":"Keep transaction"})");
  REQUIRE(malformed.contains(R"("decisions":{)"));
  write(cache, malformed);

  auto const rejected = dispatch(fx, {"import", (fx.root / "repo").string(), "--interpret", "--apply", "--json"});
  CHECK(rejected.code == 1);
  // Rejection must be total: the guard runs before reconciliation writes, so
  // nothing from this cache may reach the database.
  if (std::filesystem::exists(fx.db_path))
    CHECK(inventory(fx) == "plans=0 tasks=0 artifacts=0 decisions=0 links=0");

  // Positive control: the identical envelope with `decisions` restored to an
  // array IS accepted, so the rejection above is attributable to the shape
  // and not to some unrelated staleness in the fixture.
  write(cache, good);
  auto const accepted = dispatch(fx, {"import", (fx.root / "repo").string(), "--interpret", "--apply", "--json"});
  CHECK(accepted.code == 0);
  CHECK(inventory(fx) == "plans=3 tasks=2 artifacts=2 decisions=1 links=3");
}
