// Falsifies the two filesystem effects the import engine owns: a plain
// preview must not create operator state, while --interpret must atomically
// stage a request below the supplied (never real) PLANAR_HOME.
#include <catch2/catch_test_macros.hpp>

import std;
import planar.engine.importer;

namespace im = planar::engine::importer;

TEST_CASE("import staging separates no-interpret preview from pending handoff", "[engine][importer]") {
  auto const root = std::filesystem::temp_directory_path() /
                    std::format("planar-importer-{}", std::chrono::steady_clock::now().time_since_epoch().count());
  auto const home = root / "home";
  std::filesystem::create_directories(root / "repo" / "docs");
  {
    std::ofstream out(root / "repo" / "README.md");
    out << "# Imported title\n";
  }
  {
    std::ofstream out(root / "repo" / "docs" / "guide.md");
    out << "body\n";
  }

  auto preview = im::run(root / "repo", home, false);
  REQUIRE(preview.has_value());
  CHECK(preview->mode_ == im::outcome::mode::skipped);
  CHECK(preview->request_.anchor_title == "Imported title");
  CHECK(preview->request_.docs_count == 2);
  // The cache key is a full lower-hex SHA-256 over canonical content, not a
  // short path-only fingerprint: changing a document invalidates the cache.
  CHECK(preview->request_.fingerprint.size() == 64);
  {
    std::ofstream out(root / "repo" / "docs" / "guide.md", std::ios::app);
    out << "changed\n";
  }
  auto changed = im::run(root / "repo", home, false);
  REQUIRE(changed.has_value());
  CHECK(changed->request_.fingerprint != preview->request_.fingerprint);
  CHECK_FALSE(std::filesystem::exists(home));

  auto staged = im::run(root / "repo", home, true);
  REQUIRE(staged.has_value());
  CHECK(staged->mode_ == im::outcome::mode::pending);
  CHECK(std::filesystem::is_regular_file(staged->pending_path));
  auto again = im::run(root / "repo", home, true);
  REQUIRE(again.has_value());
  CHECK(again->pending_path == staged->pending_path);
  std::filesystem::remove_all(root);
}

TEST_CASE("import rejects a malformed or mismatched interpretation cache before apply", "[engine][importer]") {
  auto const root = std::filesystem::temp_directory_path() /
                    std::format("planar-importer-cache-{}", std::chrono::steady_clock::now().time_since_epoch().count());
  auto const home = root / "home";
  std::filesystem::create_directories(root / "repo");
  {
    std::ofstream out(root / "repo" / "README.md");
    out << "# Imported title\n";
  }

  auto staged = im::run(root / "repo", home, true);
  REQUIRE(staged.has_value());
  std::filesystem::create_directories(staged->cache_path.parent_path());
  {
    std::ofstream out(staged->cache_path);
    out << "{not json}";
  }
  auto malformed = im::run(root / "repo", home, true);
  REQUIRE_FALSE(malformed.has_value());
  CHECK(malformed.error() == im::error::invalid_input);

  {
    std::ofstream out(staged->cache_path);
    out << R"({"schema_version":1,"fingerprint":"other","anchor_title":"x","provenance":"p","phases":[],"forward_specs":[{"slug":"a"},{"slug":"b"},{"slug":"c"}]})";
  }
  auto mismatched = im::run(root / "repo", home, true);
  REQUIRE_FALSE(mismatched.has_value());
  CHECK(mismatched.error() == im::error::invalid_input);
  std::filesystem::remove_all(root);
}

// --- task 6406: negative-path cover for the untrusted cache envelope --------
//
// `cache_anchor` (importer.cpp:61-63) declares the interpretation cache an
// UNTRUSTED hand-off boundary: it is written out-of-process by a vendor
// skill, not by this binary. Three of its envelope checks -- the
// `forward_specs` count bound, the non-empty `anchor_title`, and the
// non-empty `provenance` -- could each be deleted outright with the suite
// still green (measured by permissive mutation, task 6106 blind review).
// The logic was right; nothing asserted the rejection.
//
// Each case below pairs a rejection with the POSITIVE CONTROL that differs
// only in the field under test, so a rejection for some unrelated reason
// cannot masquerade as cover.

namespace {

/// @brief A cache envelope with a caller-supplied `phases` / `decisions` /
/// `deferred_items` body, for the task-6405 validation classes.
///
/// The three forward specs and the non-empty title/provenance are fixed at
/// values the envelope accepts, so any rejection below is attributable to the
/// member under test rather than to the frame around it.
auto envelope_with(std::string_view fingerprint, std::string_view phases, std::string_view extra) -> std::string {
  return std::format(R"({{"schema_version":1,"fingerprint":"{}","anchor_title":"t","provenance":"p",)"
                     R"("phases":[{}]{},"forward_specs":[{{"slug":"a"}},{{"slug":"b"}},{{"slug":"c"}}]}})",
                     fingerprint, phases, extra);
}

/// @brief A cache envelope carrying the real fingerprint, parameterised on
/// exactly the three fields task 6406 leaves uncovered.
auto envelope(std::string_view fingerprint, std::size_t spec_count, std::string_view title, std::string_view provenance)
    -> std::string {
  std::string specs;
  for (std::size_t i = 0; i < spec_count; ++i) {
    if (i > 0)
      specs += ",";
    specs += std::format(R"({{"slug":"spec-{}"}})", i);
  }
  return std::format(R"({{"schema_version":1,"fingerprint":"{}","anchor_title":"{}","provenance":"{}",)"
                     R"("phases":[],"forward_specs":[{}]}})",
                     fingerprint, title, provenance, specs);
}

/// @brief Overwrite the staged cache and re-run staging over it.
auto stage_and_run(const std::filesystem::path& repo, const std::filesystem::path& home, const std::filesystem::path& cache,
                   std::string_view body) -> std::expected<im::outcome, im::error> {
  std::ofstream out(cache);
  out << body;
  out.close();
  return im::run(repo, home, true);
}

} // namespace

TEST_CASE("the untrusted cache envelope is rejected outside its documented bounds", "[engine][importer][6406]") {
  auto const root = std::filesystem::temp_directory_path() /
                    std::format("planar-importer-envelope-{}", std::chrono::steady_clock::now().time_since_epoch().count());
  auto const home = root / "home";
  std::filesystem::create_directories(root / "repo");
  {
    std::ofstream out(root / "repo" / "README.md");
    out << "# Imported title\n";
  }

  auto staged = im::run(root / "repo", home, true);
  REQUIRE(staged.has_value());
  auto const repo        = root / "repo";
  auto const cache       = staged->cache_path;
  auto const fingerprint = staged->request_.fingerprint;
  std::filesystem::create_directories(cache.parent_path());

  SECTION("forward_specs count is bounded at BOTH ends") {
    // The positive control first: three specs is the low bound and must be
    // ACCEPTED, so a rejection below cannot be blamed on the envelope shape.
    auto const three = stage_and_run(repo, home, cache, envelope(fingerprint, 3, "x", "p"));
    REQUIRE(three.has_value());
    CHECK(three->mode_ == im::outcome::mode::cache_hit);
    auto const five = stage_and_run(repo, home, cache, envelope(fingerprint, 5, "x", "p"));
    REQUIRE(five.has_value());
    CHECK(five->mode_ == im::outcome::mode::cache_hit);

    // Both ends, not one: a bound written as a single comparison passes a
    // one-sided fixture.
    auto const two = stage_and_run(repo, home, cache, envelope(fingerprint, 2, "x", "p"));
    REQUIRE_FALSE(two.has_value());
    CHECK(two.error() == im::error::invalid_input);
    auto const six = stage_and_run(repo, home, cache, envelope(fingerprint, 6, "x", "p"));
    REQUIRE_FALSE(six.has_value());
    CHECK(six.error() == im::error::invalid_input);
  }

  SECTION("anchor_title must be non-empty") {
    auto const present = stage_and_run(repo, home, cache, envelope(fingerprint, 3, "a title", "p"));
    REQUIRE(present.has_value());
    CHECK(present->mode_ == im::outcome::mode::cache_hit);

    auto const empty = stage_and_run(repo, home, cache, envelope(fingerprint, 3, "", "p"));
    REQUIRE_FALSE(empty.has_value());
    CHECK(empty.error() == im::error::invalid_input);
  }

  SECTION("provenance must be non-empty") {
    auto const present = stage_and_run(repo, home, cache, envelope(fingerprint, 3, "x", "vendor-skill"));
    REQUIRE(present.has_value());
    CHECK(present->mode_ == im::outcome::mode::cache_hit);

    auto const empty = stage_and_run(repo, home, cache, envelope(fingerprint, 3, "x", ""));
    REQUIRE_FALSE(empty.has_value());
    CHECK(empty.error() == im::error::invalid_input);
  }

  std::filesystem::remove_all(root);
}

// --- task 6405: the validation classes the port silently accepted ----------
//
// `cache_anchor` validated the envelope's shape but not its CONTENT, so the
// C++ `import` accepted a broader class of malformed cache JSON than the
// contract allows -- at both preview (cache-hit) and apply time, on a verb
// that writes planning entities.
//
// The contract is `agents/planar-importer.md` § "Sequencing" (the Result rules), NOT the
// sibling `synthesize` validator. Transcribing synthesize verbatim demanded a
// `source` and `citation` on EVERY decision and rejected the vendor skill's
// own documented output; the fixtures below pin the documented rule instead.
//
// Each case pairs the rejection with the positive control that differs only
// in the member under test.

TEST_CASE("the import cache is rejected on content the contract forbids", "[engine][importer][6405]") {
  auto const root = std::filesystem::temp_directory_path() /
                    std::format("planar-importer-content-{}", std::chrono::steady_clock::now().time_since_epoch().count());
  auto const home = root / "home";
  std::filesystem::create_directories(root / "repo" / "docs");
  {
    std::ofstream out(root / "repo" / "README.md");
    out << "# Imported title\n";
  }
  {
    std::ofstream out(root / "repo" / "docs" / "tech-spec.md");
    out << "# Tech spec\n";
  }

  auto staged = im::run(root / "repo", home, true);
  REQUIRE(staged.has_value());
  auto const repo  = root / "repo";
  auto const cache = staged->cache_path;
  auto const fp    = staged->request_.fingerprint;
  std::filesystem::create_directories(cache.parent_path());

  auto run_with = [&](std::string_view phases, std::string_view extra) {
    return stage_and_run(repo, home, cache, envelope_with(fp, phases, extra));
  };
  constexpr std::string_view one_todo = R"({"slug":"p1","status":"active","tasks":[{"slug":"t1","status":"todo"}]})";

  SECTION("task status must be one Planar recognises") {
    REQUIRE(run_with(one_todo, "").has_value());
    auto const bad = run_with(R"({"slug":"p1","status":"active","tasks":[{"slug":"t1","status":"not-a-status"}]})", "");
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error() == im::error::invalid_input);
  }

  SECTION("phase status must be one Planar recognises") {
    REQUIRE(run_with(R"({"slug":"p1","status":"paused","tasks":[]})", "").has_value());
    auto const bad = run_with(R"({"slug":"p1","status":"in-progress","tasks":[]})", "");
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error() == im::error::invalid_input);
  }

  SECTION("at most one task per phase may be doing") {
    // One `doing` is legal, and a SECOND `doing` in a DIFFERENT phase is too:
    // the rule is per-phase, and a fixture that only ever tried two in one
    // phase could not tell the two readings apart.
    REQUIRE(run_with(R"({"slug":"p1","status":"active","tasks":[{"slug":"t1","status":"doing"}]})", "").has_value());
    REQUIRE(run_with(R"({"slug":"p1","status":"active","tasks":[{"slug":"t1","status":"doing"}]},)"
                     R"({"slug":"p2","status":"active","tasks":[{"slug":"t2","status":"doing"}]})",
                     "")
                .has_value());
    auto const bad = run_with(R"({"slug":"p1","status":"active","tasks":[{"slug":"t1","status":"doing"},)"
                              R"({"slug":"t2","status":"doing"}]})",
                              "");
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error() == im::error::invalid_input);
  }

  SECTION("task priority is optional but bounded at BOTH ends when present") {
    REQUIRE(run_with(R"({"slug":"p1","status":"active","tasks":[{"slug":"t1","status":"todo","priority":0}]})", "").has_value());
    REQUIRE(
        run_with(R"({"slug":"p1","status":"active","tasks":[{"slug":"t1","status":"todo","priority":1000}]})", "").has_value());
    // Absent entirely is still fine -- the bound must not become a
    // requirement.
    REQUIRE(run_with(one_todo, "").has_value());

    auto const low = run_with(R"({"slug":"p1","status":"active","tasks":[{"slug":"t1","status":"todo","priority":-1}]})", "");
    REQUIRE_FALSE(low.has_value());
    CHECK(low.error() == im::error::invalid_input);
    auto const high = run_with(R"({"slug":"p1","status":"active","tasks":[{"slug":"t1","status":"todo","priority":1001}]})", "");
    REQUIRE_FALSE(high.has_value());
    CHECK(high.error() == im::error::invalid_input);
  }

  SECTION("a decision names a source Planar understands, or none at all") {
    // No `source` is the deterministic-import case and stays legal: the
    // contract requires a citation only OF an `llm-inferred` decision.
    REQUIRE(run_with(one_todo, R"(,"decisions":[{"title":"d","body":"b"}])").has_value());
    REQUIRE(run_with(one_todo, R"(,"decisions":[{"title":"d","source":"tech-spec"}])").has_value());

    auto const bad = run_with(one_todo, R"(,"decisions":[{"title":"d","source":"vibes"}])");
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error() == im::error::invalid_input);
  }

  SECTION("an llm-inferred decision must cite a path, and the path must exist") {
    REQUIRE(
        run_with(one_todo, R"(,"decisions":[{"source":"llm-inferred","citation":{"path":"docs/tech-spec.md"}}])").has_value());

    auto const no_citation = run_with(one_todo, R"(,"decisions":[{"source":"llm-inferred"}])");
    REQUIRE_FALSE(no_citation.has_value());
    auto const empty_path = run_with(one_todo, R"(,"decisions":[{"source":"llm-inferred","citation":{"path":""}}])");
    REQUIRE_FALSE(empty_path.has_value());
    // A named path is a provenance claim the apply pass records as fact, so a
    // path that is not there is a claim the cache cannot support.
    auto const missing = run_with(one_todo, R"(,"decisions":[{"source":"llm-inferred","citation":{"path":"docs/nope.md"}}])");
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error() == im::error::invalid_input);
  }

  SECTION("deferred items are validate-only, and still validated") {
    REQUIRE(run_with(one_todo, R"(,"deferred_items":[{"priority":150,"phase_slug":"p1"}])").has_value());

    auto const low = run_with(one_todo, R"(,"deferred_items":[{"priority":149,"phase_slug":"p1"}])");
    REQUIRE_FALSE(low.has_value());
    // A deferral must point at a phase THIS cache proposes; anywhere else
    // means the two halves disagree about the plan shape.
    auto const dangling = run_with(one_todo, R"(,"deferred_items":[{"priority":150,"phase_slug":"nonexistent"}])");
    REQUIRE_FALSE(dangling.has_value());
    CHECK(dangling.error() == im::error::invalid_input);
    auto const not_array = run_with(one_todo, R"(,"deferred_items":{"priority":150})");
    REQUIRE_FALSE(not_array.has_value());
  }

  std::filesystem::remove_all(root);
}
