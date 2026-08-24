// @file manifest.t.cpp
// @brief Unit tests for `planar.engine.workbench.manifest` (plan 996, task
// 6037): the bucket-local SHA-256, the `workbench_sync_state` CRUD, and the
// `.sync` mirror's exact bytes.
//
// ORACLE PROVENANCE. The digest and the `.sync` line shape were taken from a
// REAL `workbench push` against a scratch root, not from the Zig source:
//
//   $Z workbench push 1
//   cat <root>/project_demo/p1-demo-feature/.sync
//     README.md\tplan:1\t95ab1aa1...74387\t2026-08-23T15:29:16.323Z
//   shasum -a 256 <root>/project_demo/p1-demo-feature/README.md
//     95ab1aa190d6dc5092fafe2878d3c4a5902e4e5829776128c04507c186e74387
//
// The whole `.sync` file was then diffed between the two binaries: paths and
// digests identical on all eight rows, differing only in `last_synced_at`
// (which SQLite stamps at write time).

#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.db.migrate;
import planar.engine.workbench.manifest;

namespace {

namespace wm = planar::engine::workbench::manifest;

struct scratch_db_path {
  std::filesystem::path path_;

  scratch_db_path()
      : path_(std::filesystem::temp_directory_path() / std::format("planar_wb_manifest_test_{}_{}.db",
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
  REQUIRE(planar::db::apply_all(*conn).has_value());
  return std::move(*conn);
}

/// @brief A plan row, so the manifest's FK-free rows still sit next to real
/// data the way they do in production.
auto insert_plan(planar::db::connection& conn, std::string_view slug) -> std::int64_t {
  auto stmt = conn.prepare("insert into plans (scope_kind, title, slug) values ('global', 'P', ?) returning id");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->bind_text(1, slug).has_value());
  auto step = stmt->step();
  REQUIRE(step.has_value());
  REQUIRE(*step == planar::db::step_result::row);
  return stmt->column_int64(0);
}

/// @brief The exact bytes the oracle wrote for the anchor plan's README.md.
constexpr std::string_view k_oracle_readme = "---\n"
                                             "entity_kind: plan\n"
                                             "entity_id: 1\n"
                                             "anchor_plan_id: 1\n"
                                             "title: Demo Feature\n"
                                             "status: draft\n"
                                             "---\n"
                                             "\n"
                                             "# Plan 1: Demo Feature\n"
                                             "\n"
                                             "**Status:** draft  \n"
                                             "**Created:** 2026-08-23T15:29:15.630Z  \n"
                                             "**Updated:** 2026-08-23T15:29:15.630Z\n"
                                             "\n"
                                             "A demo.\n";

} // namespace

// --- SHA-256 --------------------------------------------------------------

TEST_CASE("hash_content matches the FIPS 180-4 vectors", "[workbench][manifest][hash]") {
  CHECK(wm::hash_content("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(wm::hash_content("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK(wm::hash_content("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST_CASE("hash_content matches a digest the ORACLE actually wrote", "[workbench][manifest][hash][oracle]") {
  // This is the case that would catch a transcription slip in a way the
  // textbook vectors cannot: it proves the digest is computed over the same
  // bytes the renderer produces, terminator and trailing newline included.
  CHECK(wm::hash_content(k_oracle_readme) == "95ab1aa190d6dc5092fafe2878d3c4a5902e4e5829776128c04507c186e74387");
}

TEST_CASE("hash_content is lowercase hex and exactly 64 characters", "[workbench][manifest][hash]") {
  auto const digest = wm::hash_content("anything at all");
  CHECK(digest.size() == 64);
  CHECK(std::ranges::all_of(digest, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }));
}

TEST_CASE("hash_content spans the message-block boundary correctly", "[workbench][manifest][hash]") {
  // 55 / 56 / 64 / 119 / 120 bytes exercise every padding branch: the last
  // block that still fits its length field, the one that does not and needs
  // a whole extra block, and an exact multiple.
  auto const digest_of = [](std::size_t n) { return wm::hash_content(std::string(n, 'a')); };
  CHECK(digest_of(55) == "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
  CHECK(digest_of(56) == "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
  CHECK(digest_of(64) == "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
  CHECK(digest_of(119) == "31eba51c313a5c08226adf18d4a359cfdfd8d2e816b13f4af952f7ea6584dcfb");
  CHECK(digest_of(120) == "2f3d335432c70b580af0e8e1b3674a7c020d683aa5f73aaaedfdc55af904c21c");
}

// --- the `.sync` mirror ---------------------------------------------------

TEST_CASE("feature_rel_path strips the assoc and feature directory levels", "[workbench][manifest]") {
  CHECK(wm::feature_rel_path("project_demo/p1-demo-feature/README.md") == "README.md");
  CHECK(wm::feature_rel_path("project_demo/p1-demo-feature/tasks/cross/1-a.md") == "tasks/cross/1-a.md");
  CHECK(wm::feature_rel_path("/project_demo/p1-demo-feature/README.md/") == "README.md");
}

TEST_CASE("feature_rel_path returns a path with FEWER than two separators WHOLE", "[workbench][manifest]") {
  // The global-scope case: with no association level the stored path has one
  // separator, so `.sync` lists `p1-slug/README.md` rather than `README.md`.
  // Reproduced rather than corrected (D2) -- `.sync` is a mirror nothing
  // reads back. Pinned so a later "tidy-up" has to argue with a test.
  CHECK(wm::feature_rel_path("p1-slug/README.md") == "p1-slug/README.md");
  CHECK(wm::feature_rel_path("README.md") == "README.md");
}

TEST_CASE("render_sync_file emits one tab-separated line per row", "[workbench][manifest]") {
  std::vector<wm::sync_state> rows{
      wm::sync_state{.entity_kind    = "plan",
                     .entity_id      = 1,
                     .file_path      = "project_demo/p1-demo-feature/README.md",
                     .content_hash   = "abc123",
                     .last_synced_at = "2026-08-23T15:29:16.323Z"},
      wm::sync_state{.entity_kind    = "task",
                     .entity_id      = 2,
                     .file_path      = "project_demo/p1-demo-feature/tasks/cross/2-t.md",
                     .content_hash   = "def456",
                     .last_synced_at = "2026-08-23T15:29:16.324Z"},
  };
  CHECK(wm::render_sync_file(rows) == "README.md\tplan:1\tabc123\t2026-08-23T15:29:16.323Z\n"
                                      "tasks/cross/2-t.md\ttask:2\tdef456\t2026-08-23T15:29:16.324Z\n");
}

TEST_CASE("render_sync_file on no rows is ZERO BYTES, not a bare newline", "[workbench][manifest]") {
  CHECK(wm::render_sync_file({}).empty());
}

// --- CRUD -----------------------------------------------------------------

TEST_CASE("upsert then load round-trips a row", "[workbench][manifest][db]") {
  scratch_db_path scratch;
  auto            conn    = open_migrated(scratch);
  auto const      plan_id = insert_plan(conn, "p");

  REQUIRE(wm::upsert(conn, wm::sync_state{.anchor_plan_id = plan_id,
                                          .entity_kind    = "plan",
                                          .entity_id      = plan_id,
                                          .file_path      = "a/b/README.md",
                                          .content_hash   = "hash1",
                                          .db_updated_at  = "2026-01-01T00:00:00.000Z"})
              .has_value());
  auto rows = wm::load(conn, plan_id);
  REQUIRE(rows.has_value());
  REQUIRE(rows->size() == 1);
  CHECK((*rows)[0].entity_kind == "plan");
  CHECK((*rows)[0].content_hash == "hash1");
  CHECK((*rows)[0].db_updated_at == "2026-01-01T00:00:00.000Z");
  // SQLite stamps this, not the caller.
  CHECK_FALSE((*rows)[0].last_synced_at.empty());
  // `fs_mtime` is written as the EMPTY STRING on every upsert -- the Zig
  // original's `fileMtime` returns "" unconditionally. If `bind_text` bound
  // SQL NULL for an empty view (task 6097) the NOT NULL constraint would
  // have refused the insert above, so this assertion is what proves the
  // normalisation in `db::statement::bind_text` is doing its job. It was
  // written against a local `nn()` guard in manifest.cpp; task 6097 moved
  // that fix to its root and deleted the guard, and this case still fails
  // if `bind_text` is reverted.
  CHECK((*rows)[0].fs_mtime.empty());
}

TEST_CASE("upsert on the same file_path REPLACES rather than duplicating", "[workbench][manifest][db]") {
  scratch_db_path scratch;
  auto            conn    = open_migrated(scratch);
  auto const      plan_id = insert_plan(conn, "p");
  auto const      row     = [&](std::string_view hash) {
    return wm::sync_state{.anchor_plan_id = plan_id,
                          .entity_kind    = "plan",
                          .entity_id      = plan_id,
                          .file_path      = "a/b/README.md",
                          .content_hash   = std::string{hash}};
  };
  REQUIRE(wm::upsert(conn, row("first")).has_value());
  REQUIRE(wm::upsert(conn, row("second")).has_value());
  auto rows = wm::load(conn, plan_id);
  REQUIRE(rows.has_value());
  REQUIRE(rows->size() == 1);
  CHECK((*rows)[0].content_hash == "second");
}

TEST_CASE("load orders by file_path and scopes to the anchor plan", "[workbench][manifest][db]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  auto const      one  = insert_plan(conn, "one");
  auto const      two  = insert_plan(conn, "two");
  // Distinct (entity_kind, entity_id) per row: the table carries
  // `unique(entity_kind, entity_id)` ALONGSIDE `unique(file_path)`, and the
  // upsert's ON CONFLICT clause names only the latter. Two rows for the same
  // entity under different paths therefore FAIL rather than replace -- found
  // by writing this test the naive way first.
  auto const write = [&](std::int64_t plan_id, std::int64_t entity_id, std::string_view path) {
    REQUIRE(wm::upsert(conn, wm::sync_state{.anchor_plan_id = plan_id,
                                            .entity_kind    = "task",
                                            .entity_id      = entity_id,
                                            .file_path      = std::string{path},
                                            .content_hash   = "h"})
                .has_value());
  };
  write(one, 10, "z/last.md");
  write(one, 11, "a/first.md");
  write(two, 12, "m/other.md");

  auto rows = wm::load(conn, one);
  REQUIRE(rows.has_value());
  REQUIRE(rows->size() == 2);
  CHECK((*rows)[0].file_path == "a/first.md");
  CHECK((*rows)[1].file_path == "z/last.md");
}

TEST_CASE("the three delete surfaces each remove the right rows", "[workbench][manifest][db]") {
  scratch_db_path scratch;
  auto            conn    = open_migrated(scratch);
  auto const      plan_id = insert_plan(conn, "p");
  auto const      write   = [&](std::string_view kind, std::int64_t id, std::string_view path) {
    REQUIRE(wm::upsert(conn, wm::sync_state{.anchor_plan_id = plan_id,
                                            .entity_kind    = std::string{kind},
                                            .entity_id      = id,
                                            .file_path      = std::string{path},
                                            .content_hash   = "h"})
                .has_value());
  };
  auto const count = [&] {
    auto rows = wm::load(conn, plan_id);
    REQUIRE(rows.has_value());
    return rows->size();
  };

  write("plan", plan_id, "a.md");
  write("task", 7, "b.md");
  write("task", 8, "c.md");
  CHECK(count() == 3);

  REQUIRE(wm::delete_by_file_path(conn, "a.md").has_value());
  CHECK(count() == 2);
  REQUIRE(wm::delete_by_entity(conn, plan_id, "task", 7).has_value());
  CHECK(count() == 1);
  REQUIRE(wm::delete_for_plan(conn, plan_id).has_value());
  CHECK(count() == 0);
}

TEST_CASE("write_sync_file lands the mirror beside the feature directory", "[workbench][manifest][fs]") {
  auto const      dir = std::filesystem::temp_directory_path() /
                        std::format("planar_wb_sync_{}", std::chrono::steady_clock::now().time_since_epoch().count());
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  std::vector<wm::sync_state> const rows{wm::sync_state{
      .entity_kind = "plan", .entity_id = 1, .file_path = "a/b/README.md", .content_hash = "h", .last_synced_at = "T"}};
  CHECK(wm::write_sync_file(dir, rows));
  std::ifstream      file(dir / ".sync", std::ios::binary);
  std::ostringstream buffer;
  buffer << file.rdbuf();
  CHECK(buffer.str() == "README.md\tplan:1\th\tT\n");
  // No `.sync.tmp` left behind: the write is tmp-then-rename.
  CHECK_FALSE(std::filesystem::exists(dir / ".sync.tmp", ec));
  std::filesystem::remove_all(dir, ec);
}
