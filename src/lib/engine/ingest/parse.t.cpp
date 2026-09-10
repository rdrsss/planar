// @file parse.t.cpp
// @brief Unit tests for `planar.engine.ingest.parse` (plan 996, task 6035).
//
// Every case below is a port of a `zig/src/engine/ingestor/parse.zig` test
// block, so a divergence in extraction behavior between the two
// implementations shows up here rather than as a silent difference in the
// task graph ingest produces. The adversarial `**Verifies:**` case and the
// bucket-H3 cases are the ones the coverage gate depends on most directly.
//
// Include-before-import is deliberate (see db/db.t.cpp): MSVC's supported
// direction for mixing textual std headers with IFC imports is
// include-then-import, not the reverse.
#include <catch2/catch_test_macros.hpp>

import std;
import planar.engine.ingest.parse;

namespace {

namespace parse = planar::engine::ingest::parse;

} // namespace

TEST_CASE("sanitize_slug trims, lowercases, and collapses separators", "[ingest][parse]") {
  CHECK(parse::sanitize_slug("  Hello World  ") == "hello-world");
  CHECK(parse::sanitize_slug("foo--bar") == "foo-bar");
  CHECK(parse::sanitize_slug("FOO_BAR") == "foo-bar");
  CHECK(parse::sanitize_slug("-leading-") == "leading");
  CHECK(parse::sanitize_slug("").empty());
  CHECK(parse::sanitize_slug("   ").empty());
}

TEST_CASE("parse_tech_spec_decisions extracts H3-rooted decisions", "[ingest][parse]") {
  constexpr std::string_view body      = R"(## Decisions

### Use SQLite

Chosen for embedded simplicity.

### Avoid cgo

Pure-Go driver.

## Open Questions

### Some question
)";
  const auto                 decisions = parse::parse_tech_spec_decisions(body);
  REQUIRE(decisions.size() == 2);
  CHECK(decisions[0].title_ == "Use SQLite");
  CHECK(decisions[0].body_ == "Chosen for embedded simplicity.");
  CHECK(decisions[1].title_ == "Avoid cgo");
  CHECK(decisions[1].body_ == "Pure-Go driver.");
}

TEST_CASE("parse_tech_spec_decisions returns empty when the section is absent", "[ingest][parse]") {
  CHECK(parse::parse_tech_spec_decisions("no decisions here\n").empty());
}

TEST_CASE("parse_tech_spec_decisions falls back to bullets when no H3 heading exists", "[ingest][parse]") {
  constexpr std::string_view body      = R"(## Decisions

- **Use SQLite.** Chosen for embedded simplicity and zero deps.
- **Avoid cgo.** Pure-Go driver keeps cross-compilation easy.

## Open Questions

### Some question
)";
  const auto                 decisions = parse::parse_tech_spec_decisions(body);
  REQUIRE(decisions.size() == 2);
  CHECK(decisions[0].title_ == "Use SQLite.");
  CHECK(decisions[1].title_ == "Avoid cgo.");
}

TEST_CASE("parse_tech_spec_decisions prefers H3 over bullets when both are present", "[ingest][parse]") {
  constexpr std::string_view body      = R"(## Decisions

- **Bullet decision.** Should be ignored when H3 present.

### Proper H3 Decision

Body text here.
)";
  const auto                 decisions = parse::parse_tech_spec_decisions(body);
  REQUIRE(decisions.size() == 1);
  CHECK(decisions[0].title_ == "Proper H3 Decision");
}

TEST_CASE("parse_tech_spec_open_questions extracts H3 titles and resolutions", "[ingest][parse]") {
  constexpr std::string_view body      = R"(## Open Questions

### Q1: how to handle X?

Some body text.

### Q2: resolved already

Resolution: use option A
with extra context.
)";
  const auto                 questions = parse::parse_tech_spec_open_questions(body);
  REQUIRE(questions.size() == 2);
  CHECK(questions[0].title_ == "Q1: how to handle X?");
  CHECK(questions[0].body_ == "Some body text.");
  CHECK(questions[0].resolution_.empty());
  CHECK(questions[1].title_ == "Q2: resolved already");
  CHECK(questions[1].resolution_ == "use option A\nwith extra context.");
}

TEST_CASE("parse_roadmap extracts H2 milestones with intent and bullets", "[ingest][parse]") {
  constexpr std::string_view body       = R"(# Roadmap

## M1

First milestone intent paragraph.

- Add foo
- Add bar [touches: a, b]
- Add baz [slug: add-baz]

## M2

Second milestone.

- Do qux
)";
  const auto                 milestones = parse::parse_roadmap(body);
  REQUIRE(milestones.size() == 2);
  CHECK(milestones[0].name_ == "M1");
  CHECK(milestones[0].intent_ == "First milestone intent paragraph.");
  REQUIRE(milestones[0].work_items_.size() == 3);
  CHECK(milestones[0].work_items_[0].title_ == "Add foo");
  CHECK(milestones[0].work_items_[1].title_ == "Add bar");
  REQUIRE(milestones[0].work_items_[1].touches_.size() == 2);
  CHECK(milestones[0].work_items_[1].touches_[0] == "a");
  CHECK(milestones[0].work_items_[1].touches_[1] == "b");
  CHECK(milestones[0].work_items_[2].title_ == "Add baz");
  CHECK(milestones[0].work_items_[2].slug_ == "add-baz");
  CHECK(milestones[1].name_ == "M2");
  CHECK(milestones[1].work_items_.size() == 1);
}

TEST_CASE("parse_roadmap folds continuation lines so wrapped annotations survive", "[ingest][parse]") {
  // Real operator markdown wraps bullets. Without folding, a `[slug:]` on the
  // continuation line is silently dropped and the task is born slugless.
  constexpr std::string_view body       = R"(## M1

- First bullet runs across two
  lines and the slug sits on the second. [slug: m1-first]
- Second bullet has the touches
  on a continuation. [touches: alpha, beta]
- Third bullet single-line only [slug: m1-third]
)";
  const auto                 milestones = parse::parse_roadmap(body);
  REQUIRE(milestones.size() == 1);
  REQUIRE(milestones[0].work_items_.size() == 3);

  CHECK(milestones[0].work_items_[0].title_ == "First bullet runs across two lines and the slug sits on the second.");
  CHECK(milestones[0].work_items_[0].slug_ == "m1-first");
  CHECK(milestones[0].work_items_[0].source_text_ ==
        "- First bullet runs across two lines and the slug sits on the second. [slug: m1-first]");

  CHECK(milestones[0].work_items_[1].title_ == "Second bullet has the touches on a continuation.");
  REQUIRE(milestones[0].work_items_[1].touches_.size() == 2);
  CHECK(milestones[0].work_items_[1].touches_[0] == "alpha");
  CHECK(milestones[0].work_items_[1].touches_[1] == "beta");
  CHECK(milestones[0].work_items_[1].source_text_ == "- Second bullet has the touches on a continuation. [touches: alpha, beta]");

  CHECK(milestones[0].work_items_[2].title_ == "Third bullet single-line only");
  CHECK(milestones[0].work_items_[2].slug_ == "m1-third");
}

TEST_CASE("parse_roadmap lifts a [depends:] annotation off the title", "[ingest][parse]") {
  constexpr std::string_view body       = R"(## M1

- Wire the reader [depends: build-writer, seed-fixture] [slug: wire-reader]
)";
  const auto                 milestones = parse::parse_roadmap(body);
  REQUIRE(milestones.size() == 1);
  REQUIRE(milestones[0].work_items_.size() == 1);
  const auto& item = milestones[0].work_items_[0];
  CHECK(item.title_ == "Wire the reader");
  CHECK(item.slug_ == "wire-reader");
  REQUIRE(item.depends_.size() == 2);
  CHECK(item.depends_[0] == "build-writer");
  CHECK(item.depends_[1] == "seed-fixture");
}

TEST_CASE("parse_test_spec extracts verifies, kind, acceptance, and prose", "[ingest][parse]") {
  constexpr std::string_view body      = R"(## Scenarios

### Scenario: happy path

**Verifies:** task:add-foo, task:42
**Kind:** integration
**Acceptance:** binary exits 0

Prose body line one.
Prose body line two.

### plain title (no prefix)

**Verifies:** task:add-bar
)";
  const auto                 scenarios = parse::parse_test_spec(body);
  REQUIRE(scenarios.size() == 2);
  CHECK(scenarios[0].title_ == "happy path");
  CHECK(scenarios[0].kind_ == "integration");
  CHECK(scenarios[0].acceptance_ == "binary exits 0");
  REQUIRE(scenarios[0].verifies_.size() == 2);
  CHECK(scenarios[0].verifies_[0].kind_ == "task");
  CHECK(scenarios[0].verifies_[0].slug_ == "add-foo");
  CHECK(scenarios[0].verifies_[0].id_ == 0);
  CHECK(scenarios[0].verifies_[1].kind_ == "task");
  CHECK(scenarios[0].verifies_[1].slug_.empty());
  CHECK(scenarios[0].verifies_[1].id_ == 42);
  CHECK(scenarios[0].body_ == "Prose body line one.\nProse body line two.");

  CHECK(scenarios[1].title_ == "plain title (no prefix)");
  REQUIRE(scenarios[1].verifies_.size() == 1);
  CHECK(scenarios[1].verifies_[0].slug_ == "add-bar");
  CHECK(scenarios[1].body_.empty());
}

TEST_CASE("parse_test_spec leaves verifies empty when the field line is absent", "[ingest][parse]") {
  constexpr std::string_view body      = R"(## Scenarios

### No verifies here

**Kind:** unit

Body only.
)";
  const auto                 scenarios = parse::parse_test_spec(body);
  REQUIRE(scenarios.size() == 1);
  CHECK(scenarios[0].verifies_.empty());
  CHECK(scenarios[0].kind_ == "unit");
  CHECK(scenarios[0].body_ == "Body only.");
}

TEST_CASE("parse_test_spec drops malformed verifies entries silently", "[ingest][parse]") {
  // The coverage gate reports the resulting shortfall; the parser must not
  // reject the whole document over one typo'd ref.
  constexpr std::string_view body      = R"(## Scenarios

### Adversarial

**Verifies:** task:valid, , task:, :bad, task:good-slug, task:-1, task:0, task:5
)";
  const auto                 scenarios = parse::parse_test_spec(body);
  REQUIRE(scenarios.size() == 1);
  // Survivors: task:valid, task:good-slug (slugs), task:5 (id). `-1` parses
  // numerically but is dropped by the id <= 0 rule; `0` likewise.
  REQUIRE(scenarios[0].verifies_.size() == 3);
  CHECK(scenarios[0].verifies_[0].slug_ == "valid");
  CHECK(scenarios[0].verifies_[1].slug_ == "good-slug");
  CHECK(scenarios[0].verifies_[2].id_ == 5);
}

TEST_CASE("parse_test_spec extracts H4 scenarios under a bucket H3 group header", "[ingest][parse]") {
  constexpr std::string_view body      = R"(## Scenarios

### Happy paths

#### Scenario: add item succeeds

**Verifies:** task:add-item
**Kind:** integration
**Acceptance:** exit 0

#### Scenario: list items returns all

**Verifies:** task:list-items
**Kind:** integration
**Acceptance:** JSON array with expected count

### Errors

#### Scenario: invalid input rejected

**Verifies:** task:validate-input
**Kind:** unit
**Acceptance:** exit 1
)";
  const auto                 scenarios = parse::parse_test_spec(body);
  REQUIRE(scenarios.size() == 3);
  for (const auto& s : scenarios) {
    CHECK(s.title_ != "Happy paths");
    CHECK(s.title_ != "Errors");
  }
  CHECK(scenarios[0].title_ == "add item succeeds");
  REQUIRE(scenarios[0].verifies_.size() == 1);
  CHECK(scenarios[0].verifies_[0].slug_ == "add-item");
  CHECK(scenarios[1].title_ == "list items returns all");
  CHECK(scenarios[2].title_ == "invalid input rejected");
}

TEST_CASE("parse_test_spec still extracts canonical flat H3 scenarios", "[ingest][parse]") {
  constexpr std::string_view body      = R"(## Scenarios

### Scenario: happy path

**Verifies:** task:do-thing
**Kind:** integration
**Acceptance:** exit 0

### Scenario: error path

**Verifies:** task:do-thing
**Kind:** unit
**Acceptance:** exit 1
)";
  const auto                 scenarios = parse::parse_test_spec(body);
  REQUIRE(scenarios.size() == 2);
  CHECK(scenarios[0].title_ == "happy path");
  CHECK(scenarios[1].title_ == "error path");
}

TEST_CASE("parse_test_spec treats a prefix-less H3 carrying Verifies as a scenario", "[ingest][parse]") {
  constexpr std::string_view body      = R"(## Scenarios

### plain title no prefix

**Verifies:** task:foo-bar
**Kind:** unit
**Acceptance:** passes
)";
  const auto                 scenarios = parse::parse_test_spec(body);
  REQUIRE(scenarios.size() == 1);
  CHECK(scenarios[0].title_ == "plain title no prefix");
  CHECK(scenarios[0].verifies_.size() == 1);
}

TEST_CASE("parse_test_spec skips a bucket H3 with neither prefix nor Verifies", "[ingest][parse]") {
  constexpr std::string_view body      = R"(## Scenarios

### Error cases

Some descriptive text about this group.

#### Scenario: network failure

**Verifies:** task:handle-network-error
**Kind:** integration
**Acceptance:** graceful degradation
)";
  const auto                 scenarios = parse::parse_test_spec(body);
  REQUIRE(scenarios.size() == 1);
  CHECK(scenarios[0].title_ != "Error cases");
  CHECK(scenarios[0].title_ == "network failure");
}

TEST_CASE("section_has_content distinguishes an absent section from an empty one", "[ingest][parse]") {
  constexpr std::string_view populated = "## Scenarios\n\n- something\n";
  constexpr std::string_view empty     = "## Scenarios\n\n## Next\n\n- content here\n";
  constexpr std::string_view absent    = "## Other\n\n- something\n";

  CHECK(parse::section_has_content(populated, "## Scenarios"));
  CHECK_FALSE(parse::section_has_content(empty, "## Scenarios"));
  CHECK_FALSE(parse::section_has_content(absent, "## Scenarios"));
}
