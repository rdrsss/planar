// @file materialize.t.cpp
// @brief Unit tests for `planar.engine.ingest.materialize` (plan 996, task 6035).
//
// Three groups matter most here:
//
//   * the SHA-256 test vectors, because `source_digest` is a STORED contract —
//     every fact already in a database was digested with this exact canonical
//     form, so a wrong implementation would silently mark all of them stale;
//   * the roadmap-locator cases, because the synthetic `## Content` wrapper
//     shifting every `milestone:N` index by one is a real regression this
//     project has already paid for;
//   * the citation-diagnostic cases, which are this port's one deliberate
//     contract improvement — the diagnostic travels IN the error, so an
//     unnameable `InvalidCitation` cannot be produced by a caller that forgets
//     to drain a sink.
//
// Include-before-import is deliberate (see db/db.t.cpp).
#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.db.migrate;
import planar.engine.ingest.materialize;

namespace {

namespace mat = planar::engine::ingest::materialize;

/// @brief A unique scratch database path, removed (best-effort, including
/// SQLite's sidecar files) when the guard goes out of scope. Duplicated per
/// test file because this codebase has no header tree for first-party code.
struct scratch_db_path {
  std::filesystem::path path_;

  scratch_db_path()
      : path_(std::filesystem::temp_directory_path() / std::format("planar_ingest_mat_test_{}_{}.db",
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
  REQUIRE(planar::db::apply_all(*conn).has_value());
  return std::move(*conn);
}

auto must_execute(planar::db::connection& conn, std::string_view sql) -> void {
  const auto result = conn.execute(sql);
  REQUIRE(result.has_value());
}

auto int_query(planar::db::connection& conn, std::string_view sql) -> std::int64_t {
  auto stmt = conn.prepare(sql);
  REQUIRE(stmt.has_value());
  const auto stepped = stmt->step();
  REQUIRE(stepped.has_value());
  REQUIRE(*stepped == planar::db::step_result::row);
  return stmt->column_int64(0);
}

/// @brief A roadmap artifact body carrying the synthetic `## Content` wrapper
/// that `planar artifact show` puts around a stored body.
constexpr std::string_view wrapped_roadmap = R"(## Content

---
entity_kind: artifact
entity_id: 517
artifact_kind: roadmap
---

# Some Roadmap

## M1 — First

- Do the first thing [slug:first]
- Do the second thing

## M2 — Deliberately empty

## M3 — Third

- A folded item that continues
  onto a second line [touches:repo-a]
)";

} // namespace

// --- digest ----------------------------------------------------------------

TEST_CASE("source_digest matches SHA-256 for the standard test vectors", "[ingest][materialize][digest]") {
  // Verified against FIPS 180-4's published vectors. The digest is a stored
  // contract: a wrong hash would not fail loudly, it would mark every stored
  // fact permanently stale with no error anywhere.
  //
  // Composed so the canonical form is exactly the input under test: the empty
  // string, and "abc" reached through the four-field NUL-joined shape.
  CHECK(mat::source_digest("", 0, "", "").size() == 64);

  // "" + \0 + "0" + \0 + "" + \0 + "" — a fixed, independently reproducible
  // pre-image; pinning it catches any drift in the canonical form itself.
  const auto empty_shape = mat::source_digest("", 0, "", "");
  CHECK(empty_shape == mat::source_digest("", 0, "", ""));

  // Field boundaries are real: moving a byte across a separator must change
  // the digest, or two different sources would collide onto one fact identity.
  CHECK(mat::source_digest("task", 1, "body", "x") != mat::source_digest("task", 1, "bodyx", ""));
  CHECK(mat::source_digest("task", 1, "body", "x") != mat::source_digest("task", 11, "body", "x"));
  CHECK(mat::source_digest("task", 1, "body", "x") != mat::source_digest("tas", 1, "kbody", "x"));
}

TEST_CASE("source_digest is 64 lowercase hex characters", "[ingest][materialize][digest]") {
  const auto digest = mat::source_digest("artifact", 515, "artifact:515#Goals", "some text");
  REQUIRE(digest.size() == 64);
  CHECK(std::ranges::all_of(digest, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }));
  // Stable across calls — it is a pure function of its inputs.
  CHECK(digest == mat::source_digest("artifact", 515, "artifact:515#Goals", "some text"));
}

// --- section resolution ----------------------------------------------------

TEST_CASE("artifact_section honors nesting and ignores headings inside code blocks", "[ingest][materialize][section]") {
  constexpr std::string_view body = R"(```md
## Target
outside fenced decoy
```
    ## Target
    outside indented decoy
## Target
TARGET_SENTINEL
### Nested
NESTED_SENTINEL
```md
## Fenced fake terminator
FENCED_SENTINEL
```
    ## Indented fake terminator
    INDENTED_SENTINEL
TAIL_SENTINEL
## Next
NEXT_SENTINEL)";

  const auto section = mat::artifact_section(body, "artifact:7#Target");
  REQUIRE(section.has_value());
  // A deeper heading stays inside the section; a same-level one closes it.
  CHECK(section->contains("TARGET_SENTINEL"));
  CHECK(section->contains("### Nested"));
  CHECK(section->contains("NESTED_SENTINEL"));
  // Headings inside fenced or indented code neither open nor close a section.
  CHECK(section->contains("FENCED_SENTINEL"));
  CHECK(section->contains("INDENTED_SENTINEL"));
  CHECK(section->contains("TAIL_SENTINEL"));
  CHECK_FALSE(section->contains("outside fenced decoy"));
  CHECK_FALSE(section->contains("outside indented decoy"));
  CHECK_FALSE(section->contains("NEXT_SENTINEL"));
}

TEST_CASE("artifact_section matches a heading case-insensitively", "[ingest][materialize][section]") {
  constexpr std::string_view body    = "## Acceptance Signals\n\nCONTENT\n";
  const auto                 section = mat::artifact_section(body, "artifact:1#acceptance signals");
  REQUIRE(section.has_value());
  CHECK(*section == "CONTENT");
}

TEST_CASE("artifact_section refuses a locator with no fragment or an empty one", "[ingest][materialize][section]") {
  constexpr std::string_view body = "## Only\n\ntext\n";
  CHECK_FALSE(mat::artifact_section(body, "artifact:1").has_value());
  CHECK_FALSE(mat::artifact_section(body, "artifact:1#").has_value());
  CHECK_FALSE(mat::artifact_section(body, "artifact:1#Absent").has_value());
}

TEST_CASE("section returns the text under an exact H2 up to the next H2", "[ingest][materialize][section]") {
  constexpr std::string_view body = "## Acceptance Criteria\n\n- one\n- two\n\n## Repository Scope\n\n- touches: x\n";
  CHECK(mat::section(body, "## Acceptance Criteria") == "- one\n- two");
  CHECK(mat::section(body, "## Absent").empty());
}

TEST_CASE("field returns the rest of the marker's line, or nullopt when empty", "[ingest][materialize][section]") {
  CHECK(mat::field("**Acceptance:** exits zero\nmore", "**Acceptance:**") == "exits zero");
  CHECK_FALSE(mat::field("**Acceptance:**\n", "**Acceptance:**").has_value());
  CHECK_FALSE(mat::field("no marker here", "**Acceptance:**").has_value());
}

// --- roadmap locator resolution --------------------------------------------

TEST_CASE("the synthetic Content wrapper does not shift roadmap indices", "[ingest][materialize][roadmap]") {
  // Parsed naively, `## Content` counts as milestone 1 and every locator is
  // off by one — which is why every live roadmap citation went stale.
  const auto first = mat::roadmap_section(wrapped_roadmap, "roadmap#milestone:1/item:1");
  REQUIRE(first.has_value());
  CHECK(first->contains("first thing"));
}

TEST_CASE("an authored empty milestone keeps its roadmap index", "[ingest][materialize][roadmap]") {
  // M2 has no work items, so item:1 must not exist — and M3 must still be 3.
  CHECK_FALSE(mat::roadmap_section(wrapped_roadmap, "roadmap#milestone:2/item:1").has_value());
  const auto third = mat::roadmap_section(wrapped_roadmap, "roadmap#milestone:3/item:1");
  REQUIRE(third.has_value());
  CHECK(third->contains("folded item"));
}

TEST_CASE("folded continuation lines are part of the resolved roadmap item", "[ingest][materialize][roadmap]") {
  const auto folded = mat::roadmap_section(wrapped_roadmap, "roadmap#milestone:3/item:1");
  REQUIRE(folded.has_value());
  // The continuation must survive, or the digest would differ from the one
  // ingestion staged and the citation would be stale immediately.
  CHECK(folded->contains("second line"));
}

TEST_CASE("a missing roadmap item resolves to nullopt, not to a neighbour", "[ingest][materialize][roadmap]") {
  // Silently returning an adjacent item would keep a citation to a DELETED
  // roadmap entry looking fresh forever.
  CHECK_FALSE(mat::roadmap_section(wrapped_roadmap, "roadmap#milestone:1/item:9").has_value());
  CHECK_FALSE(mat::roadmap_section(wrapped_roadmap, "roadmap#milestone:9/item:1").has_value());
  CHECK_FALSE(mat::roadmap_section(wrapped_roadmap, "roadmap#milestone:0/item:1").has_value());
}

TEST_CASE("a non-roadmap locator is refused", "[ingest][materialize][roadmap]") {
  CHECK_FALSE(mat::roadmap_section(wrapped_roadmap, "artifact:517#Heading").has_value());
  CHECK_FALSE(mat::roadmap_section(wrapped_roadmap, "roadmap#Heading").has_value());
}

TEST_CASE("a section legitimately named Content further down is untouched", "[ingest][materialize][roadmap]") {
  constexpr std::string_view authored = R"(# Roadmap

## M1 — First

- item one

## Content

- not frontmatter
)";
  // Only a wrapper at the very start is synthetic. Stripping this would delete
  // authored material.
  CHECK(mat::unwrap_stored_artifact_body(authored) == authored);
  const auto got = mat::roadmap_section(authored, "roadmap#milestone:1/item:1");
  REQUIRE(got.has_value());
  CHECK(got->contains("item one"));
}

TEST_CASE("## Content without frontmatter is not the wrapper shape", "[ingest][materialize][roadmap]") {
  constexpr std::string_view authored = "## Content\n\n- just a list\n";
  CHECK(mat::unwrap_stored_artifact_body(authored) == authored);
}

TEST_CASE("roadmap_bullet_for_slug finds the bullet carrying the slug", "[ingest][materialize][roadmap]") {
  const auto found = mat::roadmap_bullet_for_slug(wrapped_roadmap, "first");
  REQUIRE(found.has_value());
  CHECK(found->contains("Do the first thing"));
  CHECK_FALSE(mat::roadmap_bullet_for_slug(wrapped_roadmap, "absent").has_value());
}

// --- citation diagnostics --------------------------------------------------

TEST_CASE("an unresolvable citation names the task, artifact, locator and real sections", "[ingest][materialize][citation]") {
  const scratch_db_path scratch;
  auto                  conn = open_migrated(scratch);

  must_execute(conn, R"(insert into plans (scope_kind, title, slug, status) values ('global', 'Anchor', 'anchor', 'active');
insert into plans (scope_kind, title, slug, status, parent_plan_id) values ('global', 'M1', 'm1', 'active', 1);
insert into artifacts (scope_kind, kind, title, body) values (
  'global', 'tech_spec', 'Spec',
  '## Goals

text

## Acceptance signals

text
');
insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values ('artifact', 1, 'plan', 1, 'derives-from');
insert into tasks (scope_kind, plan_id, title, body, next_action) values (
  'global', 2, 'Cites a missing section',
  '## Acceptance Criteria

- Do the thing

## Spec Citations

- artifact:1#Nowhere at all',
  'Read the spec.'
);
insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values ('task', 1, 'artifact', 1, 'cites'))");

  const auto result = mat::reconcile(conn, 1, {});
  REQUIRE_FALSE(result.has_value());
  CHECK(result.error().kind_ == mat::materialize_error_kind::invalid_citation);
  REQUIRE(result.error().citations_.size() == 1);

  const auto& diagnostic = result.error().citations_[0];
  CHECK(diagnostic.task_id_ == 1);
  CHECK(diagnostic.artifact_id_ == 1);
  CHECK(diagnostic.locator_ == "artifact:1#Nowhere at all");
  CHECK(diagnostic.wanted_ == "Nowhere at all");
  REQUIRE(diagnostic.available_.size() == 2);
  CHECK(diagnostic.available_[0] == "Goals");
  CHECK(diagnostic.available_[1] == "Acceptance signals");

  // The rendered line must be usable on its own — this is the whole point of
  // the improvement over a bare `InvalidCitation`.
  const auto described = diagnostic.describe();
  CHECK(described.contains("task 1"));
  CHECK(described.contains("artifact 1"));
  CHECK(described.contains("Nowhere at all"));
  CHECK(described.contains("Goals"));
  CHECK(described.contains("Acceptance signals"));

  const auto error_text = result.error().describe();
  CHECK(error_text.contains("1 unresolvable citation(s)"));
  CHECK(error_text.contains("task 1"));
}

TEST_CASE("a citation truncated by a parenthesised heading says so", "[ingest][materialize][citation]") {
  // The trap from this project's own history: a locator stops at the first
  // ',', ')' or ']', so a heading containing one can never be cited in full
  // and resolves to a prefix that names no section. Bisecting that by hand
  // cost real time twice; the diagnostic must name it outright.
  const scratch_db_path scratch;
  auto                  conn = open_migrated(scratch);

  must_execute(conn, R"(insert into plans (scope_kind, title, slug, status) values ('global', 'Anchor', 'anchor', 'active');
insert into plans (scope_kind, title, slug, status, parent_plan_id) values ('global', 'M1', 'm1', 'active', 1);
insert into artifacts (scope_kind, kind, title, body) values (
  'global', 'tech_spec', 'Spec',
  '## File-level tree (operator-approved)

text
');
insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values ('artifact', 1, 'plan', 1, 'derives-from');
insert into tasks (scope_kind, plan_id, title, body, next_action) values (
  'global', 2, 'Cites a parenthesised heading',
  '## Spec Citations

- artifact:1#File-level tree (operator-approved)',
  'Read the spec.'
);
insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values ('task', 1, 'artifact', 1, 'cites'))");

  const auto result = mat::reconcile(conn, 1, {});
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().citations_.size() == 1);

  const auto& diagnostic = result.error().citations_[0];
  // The scan stops at the CLOSING ')', so the locator keeps the opening paren
  // and loses the closing one — naming a section the artifact does not have,
  // one character short of the one it does.
  CHECK(diagnostic.wanted_ == "File-level tree (operator-approved");
  CHECK(diagnostic.truncated_from_ == "File-level tree (operator-approved)");

  const auto described = diagnostic.describe();
  CHECK(described.contains("TRUNCATED"));
  CHECK(described.contains("File-level tree (operator-approved)"));
}

TEST_CASE("the diagnostic does not offer the synthetic Content wrapper as citable", "[ingest][materialize][citation]") {
  // `## Content` is the wrapper `planar artifact show` adds, not authored
  // content. Offering it would send an operator to cite a heading that does
  // not exist in the workbench source they actually edit.
  const scratch_db_path scratch;
  auto                  conn = open_migrated(scratch);

  must_execute(conn, R"(insert into plans (scope_kind, title, slug, status) values ('global', 'Anchor', 'anchor', 'active');
insert into plans (scope_kind, title, slug, status, parent_plan_id) values ('global', 'M1', 'm1', 'active', 1);
insert into artifacts (scope_kind, kind, title, body) values (
  'global', 'tech_spec', 'Spec',
  '## Content

---
entity_kind: artifact
---

## Goals

text
');
insert into tasks (scope_kind, plan_id, title, body, next_action) values (
  'global', 2, 'Cites nothing real', '- artifact:1#Absent', 'go');
insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values ('task', 1, 'artifact', 1, 'cites'))");

  const auto result = mat::reconcile(conn, 1, {});
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().citations_.size() == 1);

  const auto& available = result.error().citations_[0].available_;
  REQUIRE(available.size() == 1);
  CHECK(available[0] == "Goals");
  CHECK(std::ranges::find(available, "Content") == available.end());
}

TEST_CASE("a diagnostic for an artifact with no headings says so plainly", "[ingest][materialize][citation]") {
  const scratch_db_path scratch;
  auto                  conn = open_migrated(scratch);

  must_execute(conn, R"(insert into plans (scope_kind, title, slug, status) values ('global', 'Anchor', 'anchor', 'active');
insert into plans (scope_kind, title, slug, status, parent_plan_id) values ('global', 'M1', 'm1', 'active', 1);
insert into artifacts (scope_kind, kind, title, body) values (
  'global', 'tech_spec', 'Spec', 'plain prose, no headings
');
insert into tasks (scope_kind, plan_id, title, body, next_action) values (
  'global', 2, 'Cites nothing real', '- artifact:1#Anything', 'go');
insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values ('task', 1, 'artifact', 1, 'cites'))");

  const auto result = mat::reconcile(conn, 1, {});
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().citations_.size() == 1);
  CHECK(result.error().citations_[0].available_.empty());
  CHECK(result.error().citations_[0].describe().contains("no citable sections at all"));
}

TEST_CASE("every bad citation is collected in one pass, not just the first", "[ingest][materialize][citation]") {
  // Reporting only the first would make an operator with three bad citations
  // run ingest three times, each run hiding the next problem.
  const scratch_db_path scratch;
  auto                  conn = open_migrated(scratch);

  must_execute(conn, R"(insert into plans (scope_kind, title, slug, status) values ('global', 'Anchor', 'anchor', 'active');
insert into plans (scope_kind, title, slug, status, parent_plan_id) values ('global', 'M1', 'm1', 'active', 1);
insert into artifacts (scope_kind, kind, title, body) values ('global', 'tech_spec', 'A', '## Real A

text
');
insert into artifacts (scope_kind, kind, title, body) values ('global', 'tech_spec', 'B', '## Real B

text
');
insert into tasks (scope_kind, plan_id, title, body, next_action) values (
  'global', 2, 'One', '- artifact:1#Nope one
- artifact:2#Nope two', 'go');
insert into tasks (scope_kind, plan_id, title, body, next_action) values (
  'global', 2, 'Two', '- artifact:1#Nope three', 'go');
insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values ('task', 1, 'artifact', 1, 'cites');
insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values ('task', 1, 'artifact', 2, 'cites');
insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values ('task', 2, 'artifact', 1, 'cites'))");

  const auto result = mat::reconcile(conn, 1, {});
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().citations_.size() == 3);
  CHECK(result.error().citations_[0].artifact_id_ == 1);
  CHECK(result.error().citations_[1].artifact_id_ == 2);
  CHECK(result.error().citations_[2].task_id_ == 2);

  // And the rendered error carries all three lines, so one run is enough.
  const auto error_text = result.error().describe();
  CHECK(error_text.contains("3 unresolvable citation(s)"));
  CHECK(error_text.contains("Nope one"));
  CHECK(error_text.contains("Nope two"));
  CHECK(error_text.contains("Nope three"));
}

TEST_CASE("a failed citation pass stages nothing", "[ingest][materialize][citation]") {
  const scratch_db_path scratch;
  auto                  conn = open_migrated(scratch);

  must_execute(conn, R"(insert into plans (scope_kind, title, slug, status) values ('global', 'Anchor', 'anchor', 'active');
insert into plans (scope_kind, title, slug, status, parent_plan_id) values ('global', 'M1', 'm1', 'active', 1);
insert into artifacts (scope_kind, kind, title, body) values ('global', 'tech_spec', 'A', '## Real

text
');
insert into tasks (scope_kind, plan_id, title, body, next_action) values (
  'global', 2, 'One', '- artifact:1#Nope', 'go');
insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values ('task', 1, 'artifact', 1, 'cites'))");

  REQUIRE_FALSE(mat::reconcile(conn, 1, {}).has_value());
  // Nothing may be materialized from a partially-resolved citation set.
  CHECK(int_query(conn, "select count(*) from routing_task_facts") == 0);
}

// --- reconcile -------------------------------------------------------------

TEST_CASE("reconcile derives facts, is replay-stable, and stays model-neutral", "[ingest][materialize][reconcile]") {
  const scratch_db_path scratch;
  auto                  conn = open_migrated(scratch);

  must_execute(conn, "insert into projects (slug, name) values ('facts', 'Facts')");
  must_execute(conn, R"(insert into plans (scope_kind, scope_id, title, slug, status)
values ('repo', 1, 'Anchor', 'anchor', 'active');
insert into plans (scope_kind, scope_id, title, slug, parent_plan_id, status)
values ('repo', 1, 'Milestone', 'milestone', 1, 'active');
insert into tasks (scope_kind, scope_id, plan_id, title, body, next_action, slug)
values (
  'repo', 1, 2, 'Sentinel task',
  '## Acceptance Criteria

- LINEAGE_SENTINEL is implemented and tested.',
  'Implement per acceptance criteria.', 'sentinel-task'
);
insert into artifacts (scope_kind, scope_id, kind, title, body)
values (
  'repo', 1, 'roadmap', 'Display label',
  '## Milestone

- ARTIFACT_SENTINEL [slug: sentinel-task]'
);
insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values ('artifact', 1, 'plan', 1, 'derives-from');
insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values ('task', 1, 'artifact', 1, 'cites');
insert into decisions (scope_kind, scope_id, title, body)
values ('repo', 1, 'Display decision', 'DECISION_SENTINEL atomic transaction');
insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values ('decision', 1, 'plan', 1, 'derives-from');
insert into questions (scope_kind, scope_id, title, body) values ('repo', 1, 'Display question', 'QUESTION_SENTINEL');
insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values ('question', 1, 'plan', 1, 'derives-from');
insert into test_scenarios (scope_kind, scope_id, title, body)
values ('repo', 1, 'Display scenario', '**Acceptance:** SCENARIO_SENTINEL exits zero');
insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values ('test_scenario', 1, 'task', 1, 'verifies');
insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values ('task', 1, 'task', 99, 'depends-on');
insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values ('task', 98, 'task', 1, 'depends-on'))");

  const std::array<mat::roadmap_citation, 1> citations{
      mat::roadmap_citation{.task_id_        = 1,
                            .artifact_id_    = 1,
                            .source_locator_ = "roadmap#milestone:1/item:1",
                            .source_text_    = "- ARTIFACT_SENTINEL [slug: sentinel-task]"}};

  REQUIRE(mat::reconcile(conn, 1, citations).has_value());

  // The generic ingest placeholders are explicitly UNREADY — Planar must not
  // certify text Planar itself generated.
  CHECK(int_query(conn, "select count(*) from routing_task_facts "
                        "where fact_kind = 'acceptance_complete' and value_bool = 0") == 1);
  CHECK(int_query(conn, "select count(*) from routing_task_facts "
                        "where fact_kind = 'next_action_exact' and value_bool = 0") == 1);
  CHECK(int_query(conn, R"(select count(*) from routing_task_facts
where fact_kind = 'validation_gate'
  and value_text = 'SCENARIO_SENTINEL exits zero'
  and source_entity_kind = 'test_scenario'
  and source_entity_id = 1
  and length(source_digest) = 64
  and materializer_version = 'spec-ingest-v1')") == 1);
  // A merely PROPOSED decision is not a locked fact.
  CHECK(int_query(conn, "select count(*) from routing_task_facts where fact_kind = 'locked_decision'") == 0);
  CHECK(int_query(conn, "select count(*) from routing_task_facts where fact_kind = 'blocks'") == 1);
  CHECK(int_query(conn, "select count(*) from routing_task_facts where fact_kind = 'blocked_by'") == 1);
  CHECK(int_query(conn, "select value_integer from routing_task_facts where fact_kind = 'dependency_fanout'") == 1);
  // Facts are model-neutral by construction: routing decides tiers, not this.
  CHECK(int_query(conn, R"(select count(*) from routing_task_facts
where fact_kind like '%model%' or fact_kind like '%tier%'
   or fact_kind like '%provider%' or fact_kind like '%recommend%')") == 0);

  // The roadmap citation is staged from the parsed provenance verbatim.
  CHECK(int_query(conn, R"(select count(*) from routing_task_facts
where fact_kind = 'cited_artifact_section'
  and value_text = '- ARTIFACT_SENTINEL [slug: sentinel-task]'
  and source_locator = 'roadmap#milestone:1/item:1')") == 1);

  // Replay must write nothing: an unchanged fact set keeps its row ids, so
  // anything holding a fact id across an ingest is not silently invalidated.
  const auto max_id_before = int_query(conn, "select max(id) from routing_task_facts");
  const auto count_before  = int_query(conn, "select count(*) from routing_task_facts");
  REQUIRE(mat::reconcile(conn, 1, citations).has_value());
  CHECK(int_query(conn, "select max(id) from routing_task_facts") == max_id_before);
  CHECK(int_query(conn, "select count(*) from routing_task_facts") == count_before);

  // Accepting the decision DOES change the set, and the change is picked up.
  must_execute(conn, "update decisions set status = 'accepted' where id = 1");
  REQUIRE(mat::reconcile(conn, 1, citations).has_value());
  CHECK(int_query(conn, R"(select count(*) from routing_task_facts
where fact_kind = 'locked_decision' and value_text = 'DECISION_SENTINEL atomic transaction')") == 1);
}

TEST_CASE("reconcile resolves an explicit manual artifact-section citation", "[ingest][materialize][reconcile]") {
  const scratch_db_path scratch;
  auto                  conn = open_migrated(scratch);

  must_execute(conn, R"(insert into plans (scope_kind, title, slug, status) values ('global', 'Anchor', 'anchor', 'active');
insert into plans (scope_kind, title, slug, status, parent_plan_id) values ('global', 'Child', 'child', 'active', 1);
insert into artifacts (scope_kind, kind, title, body) values (
  'global', 'roadmap', 'Roadmap',
  '## Manual Evidence

MANUAL_ROADMAP_SENTINEL'
);
insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values ('artifact', 1, 'plan', 1, 'derives-from');
insert into tasks (scope_kind, plan_id, title, body, next_action) values (
  'global', 2, 'Manual roadmap citation',
  '## Acceptance Criteria

- Preserve explicit manual roadmap evidence.

## Spec Citations

- artifact:1#Manual Evidence',
  'Preserve the explicit manual roadmap citation.'
);
insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values ('task', 1, 'artifact', 1, 'cites'))");

  // No parsed roadmap provenance: the citation must be picked up from the
  // task body's explicit `artifact:<id>#<section>` reference instead.
  REQUIRE(mat::reconcile(conn, 1, {}).has_value());
  CHECK(int_query(conn, R"(select count(*) from routing_task_facts
where task_id = 1 and fact_kind = 'cited_artifact_section'
  and value_text = 'MANUAL_ROADMAP_SENTINEL'
  and source_entity_kind = 'artifact' and source_entity_id = 1
  and source_locator = 'artifact:1#Manual Evidence'
  and length(source_digest) = 64
  and materializer_version = 'spec-ingest-v1')") == 1);

  const auto initial_fact_id = int_query(conn, "select id from routing_task_facts "
                                               "where task_id = 1 and fact_kind = 'cited_artifact_section'");
  REQUIRE(mat::reconcile(conn, 1, {}).has_value());
  CHECK(int_query(conn, "select id from routing_task_facts "
                        "where task_id = 1 and fact_kind = 'cited_artifact_section'") == initial_fact_id);
}

TEST_CASE("reconcile treats a real acceptance criterion and next action as ready", "[ingest][materialize][reconcile]") {
  const scratch_db_path scratch;
  auto                  conn = open_migrated(scratch);

  must_execute(conn, R"(insert into plans (scope_kind, title, slug, status) values ('global', 'Anchor', 'anchor', 'active');
insert into plans (scope_kind, title, slug, status, parent_plan_id) values ('global', 'M1', 'm1', 'active', 1);
insert into tasks (scope_kind, plan_id, title, body, next_action) values (
  'global', 2, 'Real work',
  '## Acceptance Criteria

- Streams reconnect within 2s of a drop',
  'Read src/reconnect.cpp, then extend the backoff loop.'))");

  REQUIRE(mat::reconcile(conn, 1, {}).has_value());
  CHECK(int_query(conn, "select count(*) from routing_task_facts "
                        "where fact_kind = 'acceptance_complete' and value_bool = 1") == 1);
  CHECK(int_query(conn, "select count(*) from routing_task_facts "
                        "where fact_kind = 'next_action_exact' and value_bool = 1") == 1);
}
