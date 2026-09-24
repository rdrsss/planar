// @file parity.t.cpp
// @brief Cross-implementation parity gate for `planar.engine.ingest`
// (plan 996, task 6035, M4).
//
// The M9 cutover depends on the C++ preview object matching the Zig
// implementation BYTE FOR BYTE against the same input — most of all its
// `coverage` block, which the `--strict` gate reads. Nothing else in this
// bucket's test suite proves that: the render tests pin the C++ output against
// itself, which would stay green through a shared misreading of the original.
//
// This file closes that hole. `oracle_json` below is the VERBATIM stdout of
//
//     planar spec ingest 1 --strict --json
//
// run against the Zig binary (zig/zig-out/bin/planar) over a scratch database
// seeded from `roadmap_md`, `tech_spec_md` and `test_spec_md` exactly as they
// appear here. The test drives the C++ pipeline over the same three documents
// and the same database shape, and asserts the whole rendered object equals
// that captured output.
//
// The fixture is chosen to exercise what the gate actually cares about:
//   * two milestones, so plan-level derives_from labels differ;
//   * a task carrying `[touches:]`, so the optional array is emitted;
//   * a folded multi-line bullet, so continuation handling is compared;
//   * two covered and two uncovered slugs, so `uncovered_task_slugs` is
//     non-empty AND its bytewise ordering is compared;
//   * two scenarios citing nothing, so `orphan_scenarios` is non-empty and its
//     ordering is compared;
//   * a scenario citing a numeric ref alongside a slug, so the numeric-refs-
//     do-not-count rule is compared;
//   * a resolved open question, so the derived-decision rule is compared.
//
// When this test fails, the C++ pipeline has diverged from the parity oracle.
// Do not adjust `oracle_json` to match new C++ output — re-run the Zig binary
// and only update it if the ORACLE itself changed.
//
// Include-before-import is deliberate (see db/db.t.cpp).
#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.db.migrate;
import planar.engine.ingest.parse;
import planar.engine.ingest.diff;
import planar.engine.ingest.render;
import planar.engine.ingest.coverage;

namespace {

namespace parse    = planar::engine::ingest::parse;
namespace diff     = planar::engine::ingest::diff;
namespace render   = planar::engine::ingest::render;
namespace coverage = planar::engine::ingest::coverage;

/// @brief A unique scratch database path, removed (best-effort, including
/// SQLite's sidecar files) when the guard goes out of scope. Duplicated per
/// test file because this codebase has no header tree for first-party code.
struct scratch_db_path {
  std::filesystem::path path_;

  scratch_db_path()
      : path_(std::filesystem::temp_directory_path() / std::format("planar_ingest_parity_test_{}_{}.db",
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

// --- the fixture documents, verbatim ---------------------------------------

constexpr std::string_view roadmap_md = R"(# Roadmap

## M1 — Foundations

Lay the groundwork.

- Build the reader [slug: build-reader] [touches: repo]
- Build the writer [slug: build-writer]
- Wire them together
  across a folded line [slug: wire-together]

## M2 — Hardening

- Add retries [slug: add-retries]
)";

constexpr std::string_view tech_spec_md = R"(# Tech Spec

## Decisions

### Use a ring buffer

Bounded memory, predictable latency.

### Avoid a background thread

Keeps the ownership story simple.

## Open Questions

### How large should the buffer be?

Unclear; depends on throughput.

### Which serialization format?

Resolution: JSON, for tooling compatibility.
)";

constexpr std::string_view test_spec_md = R"(# Test Spec

## Scenarios

### Scenario: reader round-trips

**Verifies:** task:build-reader
**Kind:** unit
**Acceptance:** exit 0

### Scenario: writer round-trips

**Verifies:** task:build-writer, task:1234
**Kind:** integration
**Acceptance:** exit 0

### Scenario: cites nothing at all

**Kind:** unit
**Acceptance:** unclear

### Scenario: also cites nothing

**Kind:** unit
)";

/// @brief Verbatim stdout of the Zig `planar spec ingest 1 --strict --json`
/// run over the three documents above. See this file's header.
constexpr std::string_view oracle_json = R"({
  "anchor_plan_id": 1,
  "assoc_slug": "project:repo",
  "anchor_slug": "oracle-anchor",
  "entities": [
    {"op": "add", "kind": "plan", "title": "M1 — Foundations", "scope": "assoc:project:repo", "derives_from": "plan:1"},
    {"op": "add", "kind": "task", "title": "Build the reader", "scope": "assoc:project:repo", "derives_from": "plan:M1 — Foundations", "touches": ["repo"]},
    {"op": "add", "kind": "task", "title": "Build the writer", "scope": "assoc:project:repo", "derives_from": "plan:M1 — Foundations"},
    {"op": "add", "kind": "task", "title": "Wire them together across a folded line", "scope": "assoc:project:repo", "derives_from": "plan:M1 — Foundations"},
    {"op": "add", "kind": "plan", "title": "M2 — Hardening", "scope": "assoc:project:repo", "derives_from": "plan:1"},
    {"op": "add", "kind": "task", "title": "Add retries", "scope": "assoc:project:repo", "derives_from": "plan:M2 — Hardening"},
    {"op": "add", "kind": "decision", "title": "Use a ring buffer", "scope": "assoc:project:repo", "derives_from": "plan:1"},
    {"op": "add", "kind": "decision", "title": "Avoid a background thread", "scope": "assoc:project:repo", "derives_from": "plan:1"},
    {"op": "add", "kind": "decision", "title": "Which serialization format?", "scope": "assoc:project:repo", "derives_from": "plan:1"},
    {"op": "add", "kind": "question", "title": "How large should the buffer be?", "scope": "assoc:project:repo"},
    {"op": "add", "kind": "question", "title": "Which serialization format?", "scope": "assoc:project:repo"}
  ],
  "summary": {
    "additions": 11,
    "updates": 0,
    "removals": 0
  },
  "coverage": {
    "total_tasks": 4,
    "tasks_with_slug": 4,
    "tasks_without_slug": 0,
    "uncovered_task_slugs": ["add-retries", "wire-together"],
    "orphan_scenarios": ["also cites nothing", "cites nothing at all"]
  },
  "slug_collisions": []
}
)";

auto must_execute(planar::db::connection& conn, std::string_view sql) -> void {
  const auto result = conn.execute(sql);
  REQUIRE(result.has_value());
}

/// @brief Reproduces the database shape the oracle run was performed against:
/// an association-scoped root plan `oracle-anchor` in status `draft`, with no
/// child plans, tasks, decisions, questions or scenarios yet.
auto seed_oracle_database(const scratch_db_path& scratch) -> planar::db::connection {
  auto conn = planar::db::connection::open(scratch.path_.string());
  REQUIRE(conn.has_value());
  REQUIRE(planar::db::apply_all(*conn).has_value());

  must_execute(*conn, "insert into associations (slug, name, kind) values ('project:repo', 'repo', 'project')");
  must_execute(*conn, "insert into plans (scope_kind, scope_id, title, slug, status) "
                      "values ('association', 1, 'Oracle Anchor', 'oracle-anchor', 'draft')");
  return std::move(*conn);
}

} // namespace

TEST_CASE("the preview object matches the Zig parity oracle byte for byte", "[ingest][parity][m9]") {
  const scratch_db_path scratch;
  auto                  conn = seed_oracle_database(scratch);

  const auto milestones = parse::parse_roadmap(roadmap_md);
  const auto decisions  = parse::parse_tech_spec_decisions(tech_spec_md);
  const auto questions  = parse::parse_tech_spec_open_questions(tech_spec_md);
  const auto scenarios  = parse::parse_test_spec(test_spec_md);

  const auto computed = diff::compute(conn, 1, milestones, decisions, questions, scenarios);
  REQUIRE(computed.has_value());

  CHECK(render::render_json(*computed) == oracle_json);
}

TEST_CASE("the strict gate refuses the oracle fixture for the same reasons", "[ingest][parity][m9]") {
  const scratch_db_path scratch;
  auto                  conn = seed_oracle_database(scratch);

  const auto computed = diff::compute(conn, 1, parse::parse_roadmap(roadmap_md), parse::parse_tech_spec_decisions(tech_spec_md),
                                      parse::parse_tech_spec_open_questions(tech_spec_md), parse::parse_test_spec(test_spec_md));
  REQUIRE(computed.has_value());

  // The Zig run exited 2 with:
  //   plan 1: --strict refused: 2 uncovered task slug(s): add-retries,
  //   wire-together; 2 orphan scenario(s): also cites nothing; cites nothing
  //   at all
  const auto cov = coverage::compute(*computed);
  CHECK(cov.has_gaps());
  REQUIRE(cov.uncovered_task_slugs_.size() == 2);
  CHECK(cov.uncovered_task_slugs_[0] == "add-retries");
  CHECK(cov.uncovered_task_slugs_[1] == "wire-together");
  REQUIRE(cov.orphan_scenarios_.size() == 2);
  CHECK(cov.orphan_scenarios_[0] == "also cites nothing");
  CHECK(cov.orphan_scenarios_[1] == "cites nothing at all");
}

TEST_CASE("the parity fixture exercises the rules the gate depends on", "[ingest][parity][m9]") {
  // Guards the fixture itself: if a future edit flattened it into something
  // trivial, the byte-comparison above would still pass while proving much
  // less. Each assertion names one rule the fixture is here to compare.
  const auto milestones = parse::parse_roadmap(roadmap_md);
  REQUIRE(milestones.size() == 2);

  // A folded continuation line, carrying its slug on the second line.
  const auto& folded = milestones[0].work_items_[2];
  CHECK(folded.title_ == "Wire them together across a folded line");
  CHECK(folded.slug_ == "wire-together");

  // A `[touches:]` annotation, so the optional JSON array is emitted.
  CHECK(milestones[0].work_items_[0].touches_ == std::vector<std::string>{"repo"});

  // A scenario mixing a slug ref with a numeric one.
  const auto scenarios = parse::parse_test_spec(test_spec_md);
  REQUIRE(scenarios.size() == 4);
  REQUIRE(scenarios[1].verifies_.size() == 2);
  CHECK(scenarios[1].verifies_[0].slug_ == "build-writer");
  CHECK(scenarios[1].verifies_[1].id_ == 1234);

  // A resolved open question, which also derives a decision.
  const auto questions = parse::parse_tech_spec_open_questions(tech_spec_md);
  REQUIRE(questions.size() == 2);
  CHECK(questions[0].resolution_.empty());
  CHECK(questions[1].resolution_ == "JSON, for tooling compatibility.");
}
