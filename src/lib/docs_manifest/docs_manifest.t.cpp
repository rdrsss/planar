// @file docs_manifest.t.cpp
// @brief Tests for `planar.docs_manifest` (plan 996, task 6364).
//
// ============================================================================
// ORACLE PROVENANCE
// ============================================================================
// Every hash constant below was captured by RUNNING zig/zig-out/bin/planar
// end to end (`workspace init` -> routing build -> regenerate) in a scratch
// arena with HOME / PLANAR_HOME / PLANAR_DB redirected into it, then reading
// the written `.manifest-docs` bytes directly off disk:
//
//   $ Z=zig/zig-out/bin/planar
//   $ HOME=$SCRATCH/home PLANAR_HOME=$SCRATCH/home/.planar \
//     PLANAR_DB=$SCRATCH/home/.planar/planar.db $Z workspace init --name acme --slug acme
//   $ cat $SCRATCH/home/.planar/workspaces/1/.manifest-docs
//   {"version":1,"algo":"xxh64","root":"393e2cc927894d57","generated_at":"now",
//    "entries":{"<path>/AGENTS.md":{"doc_hash":"4bc747c07bfb3bfe","sources":{},
//    "sources_hash":"2e1472b57af294d1","entry_hash":"0530963b3e3967de"}}}
//
// `oracle_fixture_agents.md` beside this file is the exact byte-for-byte
// `AGENTS.md` the oracle wrote in that run (captured via a raw file copy, not
// retyped). The three content-derived digests (`doc_hash`, `sources_hash`,
// `entry_hash`) depend only on FILE CONTENT, not on the filesystem path the
// oracle happened to use — so they are asserted here as fixed constants,
// portable across machines and CI. `root` additionally folds in the file's
// PATH, which is not portable; the root-aggregation test below instead
// verifies the documented formula (`hash_hex(path + '\0' + entry_hash +
// '\n')`, derived from the same oracle capture) against whatever path THIS
// run's own scratch fixture happens to use.
//
// `generated_at` being the literal string `"now"` (not a timestamp) is
// reproduced from the SAME capture and is itself the finding recorded in
// docs_manifest.cppm's header — not a normalization bug in this port.
//
// ============================================================================
// REFERENCE xxh64 VECTORS
// ============================================================================
// `hash_hex("")` and `hash_hex("abc")` are the standard published xxh64 test
// vectors (seed 0), independently reproduced here by compiling the vendored
// xxHash release archive standalone and calling `XXH64` directly — this
// confirms the VENDORED library, not just this module's use of it, matches
// the algorithm zig's `std.hash.XxHash64` implements (both are the same
// well-defined xxh64 algorithm; the oracle-fixture chain above additionally
// proves it end to end through this module's own code path).
//
// ============================================================================
// HOME SAFETY
// ============================================================================
// This module never reads the environment. Every test here builds its own
// scratch directory under `std::filesystem::temp_directory_path()` and reads
// only the checked-in fixture file; nothing reaches a real `~/.planar`.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.docs_manifest;

namespace manifest = planar::docs_manifest;

namespace {

constexpr unsigned char k_oracle_fixture_bytes[] = {
#embed "oracle_fixture_agents.md"
};
const std::string_view k_oracle_fixture(reinterpret_cast<const char*>(k_oracle_fixture_bytes), sizeof(k_oracle_fixture_bytes));

// The oracle's own captured digests — see this file's header.
constexpr std::string_view k_oracle_doc_hash     = "4bc747c07bfb3bfe";
constexpr std::string_view k_oracle_sources_hash = "2e1472b57af294d1";
constexpr std::string_view k_oracle_entry_hash   = "0530963b3e3967de";

struct scratch_dir {
  std::filesystem::path path;

  explicit scratch_dir(std::string_view name) : path(std::filesystem::temp_directory_path() / name) {
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
  }
  ~scratch_dir() {
    std::filesystem::remove_all(path);
  }
};

} // namespace

TEST_CASE("hash_hex reproduces the published xxh64 seed-0 test vectors", "[docs_manifest][hash]") {
  CHECK(manifest::hash_hex("") == "ef46db3751d8e999");
  CHECK(manifest::hash_hex("abc") == "44bc2cf5ad770999");
}

TEST_CASE("normalize folds CRLF and lone CR to LF", "[docs_manifest][normalize]") {
  CHECK(manifest::normalize("a\r\nb\rc\n") == "a\nb\nc\n");
}

TEST_CASE("normalize right-trims trailing whitespace on every line", "[docs_manifest][normalize]") {
  CHECK(manifest::normalize("a  \t\nb\t \n") == "a\nb\n");
}

TEST_CASE("normalize collapses trailing blank lines to exactly one newline", "[docs_manifest][normalize]") {
  CHECK(manifest::normalize("a\n\n\n\n") == "a\n");
}

TEST_CASE("normalize guarantees a trailing newline on content that lacks one", "[docs_manifest][normalize]") {
  CHECK(manifest::normalize("a") == "a\n");
}

TEST_CASE("normalize strips regenerated_at and source_versions lines from front matter, "
          "and nothing else in the block",
          "[docs_manifest][normalize]") {
  const auto got = manifest::normalize("---\n"
                                       "title: x\n"
                                       "regenerated_at: \"2026-01-01\"\n"
                                       "source_versions: {}\n"
                                       "keep: yes\n"
                                       "---\n"
                                       "body\n");
  CHECK(got == "---\ntitle: x\nkeep: yes\n---\nbody\n");
}

TEST_CASE("normalize leaves content with no --- front matter untouched apart from the usual folding",
          "[docs_manifest][normalize]") {
  CHECK(manifest::normalize("plain\ntext\n") == "plain\ntext\n");
}

TEST_CASE("normalize(oracle AGENTS.md fixture) reproduces the oracle's own doc_hash", "[docs_manifest][oracle]") {
  // The single strongest end-to-end proof this suite carries: feed the
  // EXACT bytes the oracle itself wrote through THIS port's normalize(),
  // and the digest must match what the oracle computed over the same bytes.
  CHECK(manifest::hash_hex(manifest::normalize(k_oracle_fixture)) == k_oracle_doc_hash);
}

TEST_CASE("hash_hex(\"{}\") is the oracle's sources_hash for a source-less entry", "[docs_manifest][oracle]") {
  // Every entry in this port has empty `sources` (see docs_manifest.cppm's
  // header), so `sources_hash` is this one constant for every file — pinned
  // to the oracle's own captured value.
  CHECK(manifest::hash_hex("{}") == k_oracle_sources_hash);
}

TEST_CASE("entry_hash combines doc_hash and sources_hash as \"<doc>|<sources>\"", "[docs_manifest][oracle]") {
  const auto combined = std::format("{}|{}", k_oracle_doc_hash, k_oracle_sources_hash);
  CHECK(manifest::hash_hex(combined) == k_oracle_entry_hash);
}

TEST_CASE("build() over a directory holding the oracle's own AGENTS.md reproduces every oracle digest",
          "[docs_manifest][build][oracle]") {
  scratch_dir dir("planar_docs_manifest_oracle_fixture");
  {
    std::ofstream out(dir.path / "AGENTS.md", std::ios::binary);
    out.write(k_oracle_fixture.data(), static_cast<std::streamsize>(k_oracle_fixture.size()));
  }

  auto built = manifest::build(dir.path);
  REQUIRE(built.has_value());
  REQUIRE(built->entries.size() == 1);

  const auto& row = built->entries.front();
  CHECK(row.path == (dir.path / "AGENTS.md").string());
  CHECK(row.value.doc_hash == k_oracle_doc_hash);
  CHECK(row.value.sources_hash == k_oracle_sources_hash);
  CHECK(row.value.entry_hash == k_oracle_entry_hash);
  CHECK(row.value.sources.empty());

  CHECK(built->version == manifest::version);
  CHECK(built->algo == manifest::algo);
  // The literal string, not a real timestamp — see docs_manifest.cppm's header.
  CHECK(built->generated_at == "now");

  // The root-aggregation FORMULA, derived from the same oracle capture this
  // file's header quotes (`hash_hex(path + '\0' + entry_hash + '\n')`),
  // applied here to THIS run's own (non-portable) fixture path.
  const auto expected_root = manifest::hash_hex(std::format("{}{}{}{}", row.path, '\0', row.value.entry_hash, '\n'));
  CHECK(built->root == expected_root);
}

TEST_CASE("build() ignores non-.md files and recurses into subdirectories", "[docs_manifest][build]") {
  scratch_dir dir("planar_docs_manifest_walk_fixture");
  std::filesystem::create_directories(dir.path / "sub");
  {
    std::ofstream a(dir.path / "a.md", std::ios::binary);
    a << "# A\n";
  }
  {
    std::ofstream ignored(dir.path / "ignored.txt", std::ios::binary);
    ignored << "not markdown\n";
  }
  {
    std::ofstream nested(dir.path / "sub" / "b.md", std::ios::binary);
    nested << "# B\n";
  }
  {
    // Uppercase extension: the oracle's `endsWithIgnoreCase` matches this.
    std::ofstream upper(dir.path / "c.MD", std::ios::binary);
    upper << "# C\n";
  }

  auto built = manifest::build(dir.path);
  REQUIRE(built.has_value());
  REQUIRE(built->entries.size() == 3);

  // Sorted ascending by path, byte-wise — matches `manifest.zig`'s
  // `std.mem.lessThan` sort key.
  std::vector<std::string> paths;
  paths.reserve(built->entries.size());
  for (const auto& row : built->entries) {
    paths.push_back(row.path);
  }
  auto sorted = paths;
  std::ranges::sort(sorted);
  CHECK(paths == sorted);
}

TEST_CASE("build() over an empty directory yields zero entries and a stable empty root", "[docs_manifest][build]") {
  scratch_dir dir("planar_docs_manifest_empty_fixture");
  auto        built = manifest::build(dir.path);
  REQUIRE(built.has_value());
  CHECK(built->entries.empty());
  CHECK(built->root == manifest::hash_hex(""));
}

TEST_CASE("write() emits the hand-rolled JSON shape with fields in the oracle's field order", "[docs_manifest][write]") {
  scratch_dir              dir("planar_docs_manifest_write_fixture");
  const manifest::manifest value{
      .version      = manifest::version,
      .algo         = std::string{manifest::algo},
      .root         = "deadbeefdeadbeef",
      .generated_at = "now",
      .entries      = {manifest::entry_row{.path  = "docs/a.md",
                                           .value = manifest::entry{.doc_hash     = "1111111111111111",
                                                                    .sources      = {},
                                                                    .sources_hash = "2222222222222222",
                                                                    .entry_hash   = "3333333333333333"}}},
  };

  const auto out_path = dir.path / manifest::file_name;
  REQUIRE(manifest::write(out_path, value));

  std::ifstream     input(out_path, std::ios::binary);
  const std::string got{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};

  CHECK(got == "{\"version\":1,\"algo\":\"xxh64\",\"root\":\"deadbeefdeadbeef\",\"generated_at\":\"now\","
               "\"entries\":{\"docs/a.md\":{\"doc_hash\":\"1111111111111111\",\"sources\":{},"
               "\"sources_hash\":\"2222222222222222\",\"entry_hash\":\"3333333333333333\"}}\n}\n");
}
