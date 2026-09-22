// @file synthesize_leaf.t.cpp
// @brief End-to-end staging, validation, transaction, and literal-delegation evidence.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.cmd.planar.context;
import planar.cmd.planar.dispatch;
import planar.cmd.planar.main;

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
                         std::format("planar_synthesize_{}_{}", tag, std::chrono::steady_clock::now().time_since_epoch().count());
  std::error_code ec;
  for (auto const& part : {"home", "proj", "repo", "fakehome", "workbench"})
    std::filesystem::create_directories(root / part, ec);
  return {.root    = root,
          .vars    = {{"PLANAR_HOME", (root / "home").string()},
                      {"PLANAR_WORKBENCH_ROOT", (root / "workbench").string()},
                      {"PLANAR_LLM_PROVIDER", "shell"},
                      {"HOME", (root / "fakehome").string()},
                      {"PWD", (root / "proj").string()}},
          .db_path = root / "planar.db"};
}

auto dispatch(const fixture& fx, std::vector<std::string> args) -> invocation {
  std::vector<std::string> argv{"planar"};
  argv.insert(argv.end(), args.begin(), args.end());
  std::ostringstream out, err;
  context            ctx{std::move(argv), planar::cmd::map_env(fx.vars), fx.root / "proj", fx.db_path, out, err};
  auto               tree  = planar::cmd::root_app();
  auto               table = planar::cmd::handlers(*tree);
  int                code  = planar::cmd::run(ctx, *tree, table);
  return {.code = code, .out = out.str(), .err = err.str(), .db_open = ctx.db_opened()};
}

auto write(const std::filesystem::path& path, std::string_view body) -> void {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  REQUIRE_FALSE(ec);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  REQUIRE(out.good());
  out << body;
  REQUIRE(out.good());
}

auto read(const std::filesystem::path& path) -> std::string {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
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
  auto step = stmt->step();
  REQUIRE(step.has_value());
  REQUIRE(*step == planar::db::step_result::row);
  return stmt->column_int64(0);
}

auto query_text(planar::db::connection& conn, std::string_view sql) -> std::string {
  auto stmt = conn.prepare(sql);
  REQUIRE(stmt.has_value());
  auto step = stmt->step();
  REQUIRE(step.has_value());
  REQUIRE(*step == planar::db::step_result::row);
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

auto seed_repo(const fixture& fx) -> void {
  write(fx.root / "repo" / "README.md", "# Synthesized Fixture\n");
  write(fx.root / "repo" / "docs" / "tech-spec.md", "# Transactional design\n");
  write(fx.root / "repo" / "src" / "main.cpp", "int main() { return 0; }\n");
  write(fx.root / "repo" / "tests" / "unit" / "main.cpp", "// test\n");
}

auto cache_body(std::string_view fingerprint, bool slim = false) -> std::string {
  if (slim)
    return std::format(
        R"({{"schema_version":1,"fingerprint":"{}","synthesized":true,"anchor_title":"Synthesized Anchor","provenance":"fixture","phases":[{{"slug":"phase-one","title":"Phase One","status":"active","tasks":[{{"slug":"task-one","title":"Task One","status":"todo"}}]}}],"decisions":[],"deferred_items":[],"forward_specs":[{{"slug":"forward-a","title":"Forward A","goal":"A"}},{{"slug":"forward-b","title":"Forward B","goal":"B"}},{{"slug":"forward-c","title":"Forward C","goal":"C"}}],"reference_artifacts":[]}})",
        fingerprint);
  return std::format(
      R"({{"schema_version":1,"fingerprint":"{}","synthesized":true,"anchor_title":"Synthesized Anchor","provenance":"fixture","phases":[{{"slug":"phase-one","title":"Phase One","status":"active","tasks":[{{"slug":"task-one","title":"Task One","status":"todo"}}]}},{{"slug":"phase-two","title":"Phase Two","status":"draft","tasks":[{{"slug":"task-two","title":"Task Two","status":"doing","priority":200,"citations":[{{"path":"docs/tech-spec.md"}}],"code_evidence":[{{"path":"src/main.cpp"}}]}}]}}],"decisions":[{{"title":"Keep transactions","body":"Apply atomically.","source":"tech-spec","citation":{{"path":"docs/tech-spec.md"}}}}],"deferred_items":[{{"slug":"later","phase_slug":"phase-two","priority":200}}],"forward_specs":[{{"slug":"forward-a","title":"Forward A","goal":"A"}},{{"slug":"forward-b","title":"Forward B","goal":"B"}},{{"slug":"forward-c","title":"Forward C","goal":"C"}}],"reference_artifacts":[{{"path":"README.md","kind":"readme","title":"Source README"}}]}})",
      fingerprint);
}

auto stage_cache(const fixture& fx) -> std::filesystem::path {
  seed_repo(fx);
  auto pending = dispatch(fx, {"synthesize", (fx.root / "repo").string(), "--json"});
  REQUIRE(pending.code == 0);
  REQUIRE(pending.err.empty());
  REQUIRE_FALSE(pending.db_open);
  CHECK(pending.out.contains("\"mode\":\"pending\""));
  CHECK(pending.out.contains("\"provider\":\"shell\""));
  CHECK(pending.out.contains("\"docs_count\":2"));
  CHECK(pending.out.contains("\"guide_files_count\":0"));
  CHECK(pending.out.contains("\"tree_entry_count\":4"));
  CHECK(pending.out.contains("\"greenfield\":false"));
  auto const fingerprint  = field(pending.out, "fingerprint");
  auto const cache        = std::filesystem::path{field(pending.out, "cache_path")};
  auto const pending_path = std::filesystem::path{field(pending.out, "pending_path")};
  REQUIRE(std::filesystem::exists(pending_path));
  CHECK(read(pending_path).contains(std::format("\"fingerprint\":\"{}\"", fingerprint)));
  write(cache, cache_body(fingerprint));

  auto preview = dispatch(fx, {"synthesize", (fx.root / "repo").string(), "--json"});
  REQUIRE(preview.code == 0);
  REQUIRE_FALSE(preview.db_open);
  CHECK(preview.out.contains("\"mode\":\"cache_hit\""));
  CHECK(preview.out.contains("\"applied\":null"));
  CHECK_FALSE(std::filesystem::exists(fx.db_path));
  return cache;
}

} // namespace

TEST_CASE("synthesize stages and previews without opening SQLite", "[cmd][synthesize][preview]") {
  auto const fx = make_fixture("preview");
  static_cast<void>(stage_cache(fx));
  CHECK_FALSE(std::filesystem::exists(fx.db_path));
}

TEST_CASE("synthesize applies a validated cache transactionally and idempotently", "[cmd][synthesize][apply][idempotent]") {
  auto const fx = make_fixture("apply");
  static_cast<void>(stage_cache(fx));

  auto first = dispatch(fx, {"synthesize", (fx.root / "repo").string(), "--apply", "--json"});
  REQUIRE(first.code == 0);
  REQUIRE(first.err.empty());
  CHECK(first.out.contains("\"plans_created\":3"));
  CHECK(first.out.contains("\"tasks_created\":2"));
  CHECK(first.out.contains("\"artifacts_created\":4"));
  CHECK(first.out.contains("\"decisions_created\":1"));
  auto const after_first = inventory(fx);
  CHECK(after_first == "plans=3 tasks=2 artifacts=4 decisions=1 links=5");
  {
    auto conn = planar::db::connection::open(fx.db_path.string());
    REQUIRE(conn.has_value());
    CHECK(query_text(*conn, "select status from tasks where slug='task-two'") == "doing");
    CHECK(query_text(*conn, "select kind from artifacts where source_path='README.md'") == "readme");
    CHECK(query_count(*conn, "select count(*) from plans where slug like 'forward-%'") == 0);
  }

  auto second = dispatch(fx, {"synthesize", (fx.root / "repo").string(), "--apply", "--json"});
  REQUIRE(second.code == 0);
  CHECK(second.out.contains("\"plans_created\":0"));
  CHECK(second.out.contains("\"plans_updated\":3"));
  CHECK(second.out.contains("\"tasks_updated\":2"));
  CHECK(inventory(fx) == after_first);
}

TEST_CASE("synthesize removals preserve survivors and require the explicit gate", "[cmd][synthesize][removals]") {
  auto const fx    = make_fixture("removals");
  auto const cache = stage_cache(fx);
  REQUIRE(dispatch(fx, {"synthesize", (fx.root / "repo").string(), "--apply", "--json"}).code == 0);
  auto const fingerprint = cache.stem().string();
  write(cache, cache_body(fingerprint, true));

  auto preview_removal = dispatch(fx, {"synthesize", (fx.root / "repo").string(), "--apply", "--json"});
  REQUIRE(preview_removal.code == 0);
  {
    auto conn = planar::db::connection::open(fx.db_path.string());
    REQUIRE(conn.has_value());
    CHECK(query_text(*conn, "select status from plans where slug='phase-two'") == "active");
    CHECK(query_text(*conn, "select status from tasks where slug='task-two'") == "doing");
    CHECK(query_text(*conn, "select status from artifacts where source_path='README.md'") == "active");
    CHECK(query_text(*conn, "select status from decisions where title='Keep transactions'") == "proposed");
  }

  auto removed = dispatch(fx, {"synthesize", (fx.root / "repo").string(), "--apply", "--apply-removals", "--json"});
  REQUIRE(removed.code == 0);
  CHECK(removed.out.contains("\"plans_abandoned\":1"));
  CHECK(removed.out.contains("\"tasks_cancelled\":1"));
  CHECK(removed.out.contains("\"artifacts_retired\":1"));
  CHECK(removed.out.contains("\"decisions_superseded\":1"));
  {
    auto conn = planar::db::connection::open(fx.db_path.string());
    REQUIRE(conn.has_value());
    CHECK(query_text(*conn, "select status from plans where slug='phase-one'") == "active");
    CHECK(query_text(*conn, "select status from tasks where slug='task-one'") == "todo");
    CHECK(query_text(*conn, "select status from plans where slug='phase-two'") == "abandoned");
    CHECK(query_text(*conn, "select status from tasks where slug='task-two'") == "cancelled");
    CHECK(query_text(*conn, "select status from artifacts where source_path='README.md'") == "retired");
    CHECK(query_text(*conn, "select status from decisions where title='Keep transactions'") == "superseded");
  }
}

TEST_CASE("synthesize materializes only explicitly selected forward specs and pushes workbench state",
          "[cmd][synthesize][forward_specs][workbench]") {
  auto const fx = make_fixture("forward_specs");
  static_cast<void>(stage_cache(fx));
  auto applied =
      dispatch(fx, {"synthesize", (fx.root / "repo").string(), "--apply", "--accept-spec", "forward-a,forward-c", "--json"});
  REQUIRE(applied.code == 0);
  auto conn = planar::db::connection::open(fx.db_path.string());
  REQUIRE(conn.has_value());
  CHECK(query_count(*conn, "select count(*) from plans where slug like 'forward-%'") == 2);
  CHECK(query_count(*conn, "select count(*) from artifacts where source_path like 'pl-forward-spec://%'") == 6);
  CHECK(query_count(*conn, "select count(*) from plans where slug='forward-b'") == 0);
  std::size_t files = 0;
  for (auto const& entry : std::filesystem::recursive_directory_iterator(fx.root / "workbench"))
    files += entry.is_regular_file();
  CHECK(files >= 6);
}

TEST_CASE("synthesize rejects missing cache and invalid selection without observable writes",
          "[cmd][synthesize][refusal][rollback]") {
  auto const no_cache = make_fixture("no_cache");
  seed_repo(no_cache);
  auto refused = dispatch(no_cache, {"synthesize", (no_cache.root / "repo").string(), "--apply", "--json"});
  REQUIRE(refused.code == 1);
  CHECK(refused.err == std::format("error: repo-root not found or not a directory: {}\n", (no_cache.root / "repo").string()));
  CHECK_FALSE(std::filesystem::exists(no_cache.db_path));
  CHECK_FALSE(std::filesystem::exists(no_cache.root / "home" / "cache" / "bootstrap-synthesis" / "repo" / "_pending.json"));

  auto const rollback = make_fixture("rollback");
  static_cast<void>(stage_cache(rollback));
  auto bad = dispatch(
      rollback, {"synthesize", (rollback.root / "repo").string(), "--apply", "--accept-spec", "forward-a,missing", "--json"});
  REQUIRE(bad.code == 2);
  CHECK(bad.err == "error: invalid synthesize arguments\n");
  CHECK(inventory(rollback) == "plans=0 tasks=0 artifacts=0 decisions=0 links=0");
  CHECK(std::filesystem::is_empty(rollback.root / "workbench"));
}

TEST_CASE("synthesize literal delegates to deterministic import state", "[cmd][synthesize][literal]") {
  auto const fx = make_fixture("literal");
  write(fx.root / "repo" / "README.md", "# Literal Anchor\n");
  write(fx.root / "repo" / "docs" / "roadmap.md", "# Roadmap\n");

  auto preview = dispatch(fx, {"synthesize", (fx.root / "repo").string(), "--literal", "--json"});
  REQUIRE(preview.code == 0);
  REQUIRE_FALSE(preview.db_open);
  CHECK(preview.err == "synthesize: --literal mode; delegating to import.\n");
  CHECK(preview.out.contains("\"mode\":\"skipped\""));
  CHECK(preview.out.contains("\"provider\":\"shell\""));
  CHECK(preview.out.contains("\"cache_path\":null"));
  CHECK(preview.out.contains("\"applied\":null"));

  auto applied = dispatch(fx, {"synthesize", (fx.root / "repo").string(), "--literal", "--apply", "--json"});
  REQUIRE(applied.code == 0);
  CHECK(applied.out.contains("\"plans_created\":1"));
  CHECK(applied.out.contains("\"artifacts_created\":2"));
  CHECK(inventory(fx) == "plans=1 tasks=0 artifacts=2 decisions=0 links=2");
  auto conn = planar::db::connection::open(fx.db_path.string());
  REQUIRE(conn.has_value());
  CHECK(query_text(*conn, "select slug from plans limit 1") == "literal-anchor");
  CHECK(query_text(*conn, "select kind from artifacts where source_path='README.md'") == "readme");
  CHECK(query_text(*conn, "select kind from artifacts where source_path='docs/roadmap.md'") == "roadmap");
}

TEST_CASE("synthesize semantic flags are strict", "[cmd][synthesize][flags]") {
  auto const fx = make_fixture("flags");
  seed_repo(fx);
  for (auto args : std::array{
           std::vector<std::string>{"synthesize", (fx.root / "repo").string(), "--apply-removals"},
           std::vector<std::string>{"synthesize", (fx.root / "repo").string(), "--treat-as-greenfield",
                                    "--treat-as-nongreenfield"},
           std::vector<std::string>{"synthesize", (fx.root / "repo").string(), "--code-layout", "rust"},
           std::vector<std::string>{"synthesize", (fx.root / "repo").string(), "--accept-spec", "all", "--no-forward-specs"},
       }) {
    auto invalid = dispatch(fx, std::move(args));
    CHECK(invalid.code == 2);
    CHECK(invalid.err == "error: invalid synthesize arguments\n");
    CHECK_FALSE(invalid.db_open);
  }
}

TEST_CASE("synthesize --dry-run stages nothing through the CLI", "[cmd][synthesize][dry-run][6273]") {
  // The flag was DECLARED and never read, so `planar synthesize <root>
  // --dry-run` did exactly what the bare invocation does. Wiring it in the
  // engine is not enough on its own: the handler has to pass it through, and
  // only a CLI-level case can observe that it does.
  auto const fx = make_fixture("dry-run");
  seed_repo(fx);

  auto const dry = dispatch(fx, {"synthesize", (fx.root / "repo").string(), "--dry-run", "--json"});
  REQUIRE(dry.code == 0);
  REQUIRE_FALSE(dry.db_open);
  auto const pending_path = std::filesystem::path{field(dry.out, "pending_path")};
  CHECK_FALSE(pending_path.empty());
  CHECK_FALSE(std::filesystem::exists(pending_path));
  CHECK_FALSE(std::filesystem::exists(fx.root / "home" / "cache"));
  CHECK(dry.out.contains("dry run: nothing staged"));

  // Same invocation WITHOUT the flag stages for real -- the two runs differ
  // in exactly one argument, so the flag is what made the difference.
  auto const wet = dispatch(fx, {"synthesize", (fx.root / "repo").string(), "--json"});
  REQUIRE(wet.code == 0);
  CHECK(std::filesystem::exists(std::filesystem::path{field(wet.out, "pending_path")}));
}
