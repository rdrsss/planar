// @file terminal.t.cpp
// @brief Unit tests for `planar.engine.workbench.terminal` (plan 996, task
// 6037): the (kind, status) -> filtered? predicate every write-set decision
// runs through.
//
// ORACLE PROVENANCE. The filter's effect was observed end to end rather than
// asserted from the table: with one cancelled task in a pushed tree,
//
//   $Z workbench push 1
//     workbench push: plan 1 (demo-feature) - 0 applied, 0 pending,
//                     1 filtered (mode=failures), 0 conflict(s)
//   $Z workbench push 1 --filter-mode all
//     ... 1 filtered (mode=all) ...
//   $Z workbench pull 1
//     ... no `filtered` count at all -- pull does NOT filter
//   $Z workbench gc 1 --dry-run
//     workbench gc (--dry-run): would remove 1, keep 7, ...
//
// The status VALUES come from the migration CHECK constraints
// (migrations/*.up.sql), which is why `statuses_of` exists: the
// exhaustiveness test below walks it, so a status added to one place and not
// the other fails here rather than silently classifying as "not filtered".

#include <catch2/catch_test_macros.hpp>

import std;
import planar.engine.workbench.terminal;

namespace {

namespace wt = planar::engine::workbench::terminal;

using wt::classification;
using wt::kind;
using wt::mode;

constexpr std::array<kind, 6> k_all_kinds{kind::plan,     kind::task,          kind::decision,
                                          kind::question, kind::test_scenario, kind::artifact};

} // namespace

TEST_CASE("mode parses and prints its two operator-visible spellings", "[workbench][terminal]") {
  CHECK(wt::mode_from_string("failures") == mode::failures);
  CHECK(wt::mode_from_string("all") == mode::all);
  CHECK_FALSE(wt::mode_from_string("other").has_value());
  CHECK_FALSE(wt::mode_from_string("").has_value());
  // These bytes appear verbatim in `(mode=failures)` and in `filter_mode`.
  CHECK(wt::mode_to_string(mode::failures) == "failures");
  CHECK(wt::mode_to_string(mode::all) == "all");
}

TEST_CASE("kind_from_string accepts BOTH scenario spellings", "[workbench][terminal]") {
  // `test_scenario` is the table's name; `scenario` is what the sync layer's
  // entity stream carries. Both reach this function, from different callers.
  CHECK(wt::kind_from_string("test_scenario") == kind::test_scenario);
  CHECK(wt::kind_from_string("scenario") == kind::test_scenario);
  CHECK(wt::kind_from_string("plan") == kind::plan);
  CHECK(wt::kind_from_string("task") == kind::task);
  CHECK(wt::kind_from_string("decision") == kind::decision);
  CHECK(wt::kind_from_string("question") == kind::question);
  CHECK(wt::kind_from_string("artifact") == kind::artifact);
  CHECK_FALSE(wt::kind_from_string("session").has_value());
  CHECK_FALSE(wt::kind_from_string("").has_value());
}

TEST_CASE("every declared status of every kind classifies", "[workbench][terminal][exhaustive]") {
  // The guard against a migration adding a status nobody classified. Walking
  // `statuses_of` rather than a hand-written list is the point: both come
  // from the same table, so they cannot drift apart, and a status added to
  // the table without a classification cannot exist.
  for (auto const entity_kind : k_all_kinds) {
    auto const statuses = wt::statuses_of(entity_kind);
    CHECK_FALSE(statuses.empty());
    for (auto const status : statuses) {
      INFO(std::format("kind {} status {}", static_cast<int>(entity_kind), status));
      CHECK(wt::classify(entity_kind, status).has_value());
      CHECK(wt::is_filtered_str(status.empty() ? "plan" : "plan", "draft", mode::failures).has_value());
    }
  }
}

TEST_CASE("the failure terminals are filtered under BOTH modes", "[workbench][terminal]") {
  struct row {
    std::string_view kind_text;
    std::string_view status;
  };
  constexpr std::array<row, 8> failures{{
      {"plan", "abandoned"},
      {"task", "cancelled"},
      {"decision", "superseded"},
      {"decision", "withdrawn"},
      {"question", "wontfix"},
      {"scenario", "retired"},
      {"artifact", "superseded"},
      {"artifact", "retired"},
  }};
  for (auto const& item : failures) {
    INFO(std::format("{} {}", item.kind_text, item.status));
    CHECK(wt::is_filtered_str(item.kind_text, item.status, mode::failures) == true);
    CHECK(wt::is_filtered_str(item.kind_text, item.status, mode::all) == true);
  }
}

TEST_CASE("the success terminals are filtered ONLY under `all`", "[workbench][terminal]") {
  struct row {
    std::string_view kind_text;
    std::string_view status;
  };
  constexpr std::array<row, 3> successes{{
      {"plan", "done"},
      {"task", "done"},
      {"question", "answered"},
  }};
  for (auto const& item : successes) {
    INFO(std::format("{} {}", item.kind_text, item.status));
    CHECK(wt::is_filtered_str(item.kind_text, item.status, mode::failures) == false);
    CHECK(wt::is_filtered_str(item.kind_text, item.status, mode::all) == true);
  }
  // `verified` is the scenario's success terminal.
  CHECK(wt::is_filtered_str("scenario", "verified", mode::failures) == false);
  CHECK(wt::is_filtered_str("scenario", "verified", mode::all) == true);
}

TEST_CASE("a FAILING scenario is ACTIVE, not a failure terminal", "[workbench][terminal]") {
  // The name collision is the trap: `failing` means the test is red, which
  // is live work whose file must stay on disk, not "abandoned". A port that
  // grouped it with the other failure-shaped words would silently delete
  // every red scenario's file on the next `gc`.
  CHECK(wt::classify(kind::test_scenario, "failing") == classification::active);
  CHECK(wt::is_filtered_str("scenario", "failing", mode::all) == false);
}

TEST_CASE("a decision has NO success terminal", "[workbench][terminal]") {
  // `accepted` is ACTIVE -- an accepted decision is live documentation and
  // stays on disk even under `--filter-mode all`.
  CHECK(wt::classify(kind::decision, "accepted") == classification::active);
  CHECK(wt::is_filtered_str("decision", "accepted", mode::all) == false);
  // ...and so does an `active` artifact.
  CHECK(wt::is_filtered_str("artifact", "active", mode::all) == false);
}

TEST_CASE("active statuses are never filtered", "[workbench][terminal]") {
  for (auto const entity_kind : k_all_kinds) {
    for (auto const status : wt::statuses_of(entity_kind)) {
      if (wt::classify(entity_kind, status) != classification::active) {
        continue;
      }
      INFO(std::format("kind {} status {}", static_cast<int>(entity_kind), status));
      CHECK_FALSE(wt::is_filtered(classification::active, mode::failures));
      CHECK_FALSE(wt::is_filtered(classification::active, mode::all));
    }
  }
}

TEST_CASE("an unrecognized kind or status yields UNSET, not false", "[workbench][terminal]") {
  // The distinction is load-bearing: every caller treats unset as KEEP, so a
  // schema addition degrades to "gc does nothing" rather than to "gc deletes
  // files it does not understand". Returning `false` would look identical
  // here and be identical in effect -- but `nullopt` is what lets a future
  // caller choose to refuse instead.
  CHECK_FALSE(wt::is_filtered_str("session", "done", mode::all).has_value());
  CHECK_FALSE(wt::is_filtered_str("task", "invented", mode::all).has_value());
  CHECK_FALSE(wt::is_filtered_str("task", "", mode::all).has_value());
  CHECK_FALSE(wt::classify(kind::task, "abandoned").has_value()); // plan's word, not a task's
}

TEST_CASE("the same word classifies differently per kind", "[workbench][terminal]") {
  // `retired` is a failure terminal for an artifact and for a scenario, and
  // is not a status a task or plan has at all. Proves the table is keyed by
  // (kind, status) rather than by status alone.
  CHECK(wt::is_filtered_str("artifact", "retired", mode::failures) == true);
  CHECK(wt::is_filtered_str("scenario", "retired", mode::failures) == true);
  CHECK_FALSE(wt::is_filtered_str("task", "retired", mode::failures).has_value());
  CHECK_FALSE(wt::is_filtered_str("plan", "retired", mode::failures).has_value());
  // `draft` is active for a plan, a scenario and an artifact alike.
  CHECK(wt::classify(kind::plan, "draft") == classification::active);
  CHECK(wt::classify(kind::test_scenario, "draft") == classification::active);
  CHECK(wt::classify(kind::artifact, "draft") == classification::active);
}
