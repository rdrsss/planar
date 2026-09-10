// @file profile.t.cpp
// @brief Unit tests for `planar.engine.models.profile` (plan 996, task 6111).
//
// ## WHY THE FACT KINDS BELOW ARE STRING LITERALS, NOT `fact_kinds::` CONSTANTS
//
// Every fixture spells its fact kinds out in full ("schema.migration_touched")
// rather than referring to `profile::fact_kinds::migration_touched`. That is
// deliberate and it is the difference between a test and a tautology.
//
// The `fact_kinds` constants ARE part of what this port had to get right --
// they are the contract with the ingest materializer that writes
// `routing_task_facts.fact_kind`. A fixture built from those constants would
// feed the implementation its own spelling on both sides: mistype
// `migration_touched` as `"schema.migration-touched"` and the rule table, the
// fixture, and the assertion would all agree, the suite would stay green, and
// the classifier would silently never match a real materialized fact.
//
// Spelling them out means the literals below are an INDEPENDENT transcription
// of the oracle's `zig/src/engine/routing/profile.zig:49-81`, so a divergence
// in either direction is a failure. `the fact-kind constants match their wire
// spellings` below pins that mapping explicitly, once.
//
// ## ORACLE REACHABILITY, STATED PLAINLY
//
// `profile::compile` runs only for a task whose packet is READY, and the
// verb that reaches it (`models resolve --role <task-role> --task <id>`) is
// NOT YET WIRED in this tree. It was still exercised against the parity
// oracle for this task, which established two things this suite relies on:
//
//   - The unready path is what a bare seeded task produces. A fresh project +
//     plan + task yields sixteen readiness reasons, so
//     `models resolve --role coder --task 1 --json` reports
//     `"source":"static_fallback"`, `"fallback_reason":"packet_not_ready"`,
//     `"first_readiness_reason":"missing_acceptance_section"` and NEVER
//     reaches this module. That transcript is pinned in `roles.t.cpp`.
//   - Clearing all sixteen requires the full ingest materializer path
//     (product/tech/roadmap/test specs, locked decisions, dependencies,
//     touches, validation gates, covering scenarios, and materialized
//     `routing_task_facts` whose `source_digest` and `materializer_version`
//     both still verify). That fixture is the FIRST job of the cycle that
//     wires the verb, and it is what will turn the cases below from
//     transcribed-rule tests into byte-differential ones.
//
// So: the rule TABLES here are transcribed from the oracle and pinned
// structurally by the shape test that runs first; the rule BEHAVIOR is
// covered branch by branch and every case was proven non-vacuous by mutating
// the implementation (see the task's break-probe transcript). No case in this
// file claims to be an oracle byte-diff, because none of them is one yet.

import std;
import planar.engine.models.profile;
import planar.engine.models.ranking;
import planar.engine.models.registry;

#include <catch2/catch_test_macros.hpp>

namespace profile_ = planar::engine::models::profile;
namespace ranking  = planar::engine::models::ranking;
namespace registry = planar::engine::models::registry;

using profile_::fact;
using ranking::complexity;
using ranking::work_type;
using registry::tier;

namespace {

/// Build a fact. `text` defaults to `"true"`, which ASSERTS the fact -- the
/// oracle treats every text except `"false"` and `"0"` as an assertion.
[[nodiscard]] auto f(std::string_view kind, std::string_view text = "true") -> fact {
  return fact{.kind = kind, .locator = "loc", .text = text, .provenance = "task", .current_digest = "digest", .id = 7};
}

/// The one fact `compile` requires to be PRESENT before it will classify.
[[nodiscard]] auto acceptance(std::string_view text = "true") -> fact {
  return f("acceptance_complete", text);
}

/// Extract the profile alternative, failing the test if the outcome was a
/// refusal. Keeps every behavioral assertion from silently reading a
/// default-constructed profile out of the wrong variant alternative.
[[nodiscard]] auto must_profile(const profile_::outcome& out) -> profile_::profile {
  REQUIRE(std::holds_alternative<profile_::profile>(out));
  return std::get<profile_::profile>(out);
}

} // namespace

// ---------------------------------------------------------------------------
// FIXTURE SHAPE. This runs before any behavioral case and exists so that a
// fixture which silently matches nothing cannot pass vacuously.
// ---------------------------------------------------------------------------

TEST_CASE("models.profile: the rule tables have the shape every later case assumes", "[models][profile]") {
  const auto rules = profile_::work_type_rules();

  // FIVE rules, not six: `feature` is the FALL-THROUGH and has no row. A
  // sixth row here would mean `feature` had become matchable, which would
  // change what an unmatched packet classifies as.
  REQUIRE(rules.size() == 5);

  // PRECEDENCE ORDER, which is NOT `enum class work_type`'s declaration order
  // (schema, engine, architectural, cli, feature, mechanical). If these two
  // ever coincide, a later reader may conclude precedence can be derived from
  // the enum. It cannot.
  CHECK(rules[0].type == work_type::schema);
  CHECK(rules[1].type == work_type::architectural);
  CHECK(rules[2].type == work_type::engine);
  CHECK(rules[3].type == work_type::cli);
  CHECK(rules[4].type == work_type::mechanical);

  CHECK(rules[0].id == "work-type.schema.v1");
  CHECK(rules[1].id == "work-type.architectural.v1");
  CHECK(rules[2].id == "work-type.engine.v1");
  CHECK(rules[3].id == "work-type.cli.v1");
  CHECK(rules[4].id == "work-type.mechanical.v1");
  CHECK(profile_::default_rule_id == "work-type.feature.v1");

  CHECK(rules[0].any_of.size() == 3);
  CHECK(rules[1].any_of.size() == 2);
  CHECK(rules[2].any_of.size() == 8);
  CHECK(rules[3].any_of.size() == 1);
  CHECK(rules[4].any_of.size() == 2);

  CHECK(profile_::contradictions().size() == 7);

  const auto ts = profile_::thresholds();
  REQUIRE(ts.size() == 2);
  CHECK(ts[0].id == "complexity.touched-units.v1");
  CHECK(ts[0].metric == "capacity.touched_unit_count");
  CHECK(ts[0].bounded_max == 2);
  CHECK(ts[0].large_min == 12);
  CHECK(ts[1].id == "complexity.validation-gates.v1");
  CHECK(ts[1].metric == "capacity.validation_gate_count");
  CHECK(ts[1].bounded_max == 1);
  CHECK(ts[1].large_min == 6);

  CHECK(profile_::rule_version == "routing-profile-v1");
}

TEST_CASE("models.profile: the fact-kind constants match their wire spellings", "[models][profile]") {
  // The one place the constants are compared against literals. Everything
  // else in this file uses literals so this mapping is tested exactly once
  // and cannot be assumed by the cases that follow.
  namespace k = profile_::fact_kinds;
  CHECK(k::migration_touched == "schema.migration_touched");
  CHECK(k::schema_version_contract == "schema.version_contract_change");
  CHECK(k::constraint_or_index_redesign == "schema.constraint_or_index_redesign");
  CHECK(k::new_subsystem == "architectural.new_subsystem");
  CHECK(k::new_binary == "architectural.new_binary");
  CHECK(k::module_breadth == "architectural.module_breadth");
  CHECK(k::transaction_change == "engine.transaction_change");
  CHECK(k::concurrency_change == "engine.concurrency_change");
  CHECK(k::ownership_change == "engine.ownership_change");
  CHECK(k::security_change == "engine.security_change");
  CHECK(k::resource_lifecycle_change == "engine.resource_lifecycle_change");
  CHECK(k::status_transition_change == "engine.status_transition_change");
  CHECK(k::scope_resolution_change == "engine.scope_resolution_change");
  CHECK(k::capability_boundary_change == "engine.capability_boundary_change");
  CHECK(k::cli_surface_change == "cli.surface_change");
  CHECK(k::docs_only == "mechanical.docs_only");
  CHECK(k::rename_or_format_only == "mechanical.rename_or_format_only");
  CHECK(k::touched_unit_count == "capacity.touched_unit_count");
  CHECK(k::validation_gate_count == "capacity.validation_gate_count");
  CHECK(k::explicit_risk == "risk.explicit");
  CHECK(k::acceptance_complete == "acceptance_complete");
  CHECK(k::observable_correctness == "acceptance.observable_correctness");
  CHECK(k::high_judgment_acceptance == "acceptance.high_judgment");
}

TEST_CASE("models.profile: module_breadth is declared but consumed by no rule", "[models][profile]") {
  // Pins a real property of the oracle rather than papering over it: the
  // constant exists in `fact_kinds` and appears in NO `any_of` list. A task
  // carrying only this fact classifies as `feature`, not `architectural`.
  for (const auto& rule : profile_::work_type_rules()) {
    for (const auto& kind : rule.any_of) {
      CHECK(kind != "architectural.module_breadth");
    }
  }

  const std::array facts{f("architectural.module_breadth"), acceptance()};
  const auto       out = must_profile(profile_::compile(facts));
  CHECK(out.type == work_type::feature);
  CHECK(out.work_type_rule_id == "work-type.feature.v1");
}

// ---------------------------------------------------------------------------
// Refusals
// ---------------------------------------------------------------------------

TEST_CASE("models.profile: every contradiction pair refuses rather than picking a side", "[models][profile]") {
  const std::array<std::array<std::string_view, 2>, 7> pairs{{
      {"mechanical.docs_only", "schema.migration_touched"},
      {"mechanical.docs_only", "schema.version_contract_change"},
      {"mechanical.docs_only", "architectural.new_subsystem"},
      {"mechanical.docs_only", "architectural.new_binary"},
      {"mechanical.docs_only", "cli.surface_change"},
      {"mechanical.rename_or_format_only", "architectural.new_subsystem"},
      {"mechanical.rename_or_format_only", "schema.version_contract_change"},
  }};

  for (const auto& pair : pairs) {
    INFO("pair: " << pair[0] << " + " << pair[1]);
    const std::array facts{f(pair[0]), f(pair[1]), acceptance()};
    const auto       out = profile_::compile(facts);
    REQUIRE(std::holds_alternative<profile_::not_ready_reason>(out));
    CHECK(std::get<profile_::not_ready_reason>(out) == profile_::not_ready_reason::contradictory_facts);
  }

  // The PRESENT case, so the loop above cannot pass because the fixture built
  // nothing: each half alone must classify fine.
  const std::array docs_only{f("mechanical.docs_only"), acceptance()};
  CHECK(must_profile(profile_::compile(docs_only)).type == work_type::mechanical);
  const std::array migration{f("schema.migration_touched"), acceptance()};
  CHECK(must_profile(profile_::compile(migration)).type == work_type::schema);
}

TEST_CASE("models.profile: a contradiction only fires when BOTH halves assert true", "[models][profile]") {
  // A pair where one half is recorded `false` is not a contradiction -- the
  // task is asserting the property does NOT hold.
  const std::array facts{f("mechanical.docs_only", "false"), f("schema.migration_touched"), acceptance()};
  const auto       out = must_profile(profile_::compile(facts));
  CHECK(out.type == work_type::schema);
}

TEST_CASE("models.profile: an absent acceptance_complete refuses as a missing required fact", "[models][profile]") {
  const std::array facts{f("schema.migration_touched")};
  const auto       out = profile_::compile(facts);
  REQUIRE(std::holds_alternative<profile_::not_ready_reason>(out));
  CHECK(std::get<profile_::not_ready_reason>(out) == profile_::not_ready_reason::missing_required_fact);

  // The absence assertion above can pass for the wrong reason (e.g. if
  // compile refused everything), so assert the PRESENT case too.
  const std::array with{f("schema.migration_touched"), acceptance()};
  CHECK(std::holds_alternative<profile_::profile>(profile_::compile(with)));
}

TEST_CASE("models.profile: acceptance_complete recorded false is PRESENT, not missing", "[models][profile]") {
  // The load-bearing asymmetry: step 2 requires PRESENCE, step 5 requires
  // TRUTH. A `false` acceptance fact classifies fine but denies the
  // `bounded-complete` tier floor.
  const std::array facts{acceptance("false"), f("capacity.touched_unit_count", "1")};
  const auto       out = must_profile(profile_::compile(facts));
  CHECK(out.risk == complexity::bounded);
  // bounded but NOT complete -> falls through to the standard floor.
  CHECK(out.tier_floor == tier::medium);
  CHECK(out.tier_floor_rule_id == "tier-floor.standard.v1");

  // And the true case takes the small floor, proving the branch above is the
  // `complete` check and not something incidental.
  const std::array complete{acceptance("true"), f("capacity.touched_unit_count", "1")};
  const auto       ok = must_profile(profile_::compile(complete));
  CHECK(ok.tier_floor == tier::small);
  CHECK(ok.tier_floor_rule_id == "tier-floor.bounded-complete.v1");
}

// ---------------------------------------------------------------------------
// Work type
// ---------------------------------------------------------------------------

TEST_CASE("models.profile: work type follows table order, so schema outranks cli", "[models][profile]") {
  const std::array facts{f("cli.surface_change"), f("schema.migration_touched"), acceptance()};
  const auto       out = must_profile(profile_::compile(facts));
  CHECK(out.type == work_type::schema);
  CHECK(out.work_type_rule_id == "work-type.schema.v1");

  // The reverse fixture, so this cannot pass merely because `schema` is the
  // enum's first value: cli ALONE must still classify as cli.
  const std::array cli_only{f("cli.surface_change"), acceptance()};
  CHECK(must_profile(profile_::compile(cli_only)).type == work_type::cli);
}

TEST_CASE("models.profile: architectural outranks engine and cli but not schema", "[models][profile]") {
  const std::array arch_engine{f("engine.transaction_change"), f("architectural.new_binary"), acceptance()};
  CHECK(must_profile(profile_::compile(arch_engine)).type == work_type::architectural);

  const std::array arch_schema{f("architectural.new_binary"), f("schema.constraint_or_index_redesign"), acceptance()};
  CHECK(must_profile(profile_::compile(arch_schema)).type == work_type::schema);
}

TEST_CASE("models.profile: mechanical is the LOWEST-precedence matchable rule", "[models][profile]") {
  const std::array with_cli{f("mechanical.rename_or_format_only"), f("cli.surface_change"), acceptance()};
  CHECK(must_profile(profile_::compile(with_cli)).type == work_type::cli);

  const std::array alone{f("mechanical.rename_or_format_only"), acceptance()};
  CHECK(must_profile(profile_::compile(alone)).type == work_type::mechanical);
}

TEST_CASE("models.profile: every engine fact kind reaches the engine rule", "[models][profile]") {
  // Eight kinds in one `any_of`; a typo in any single one would otherwise
  // hide behind the other seven.
  const std::array<std::string_view, 8> kinds{"engine.transaction_change",        "engine.concurrency_change",
                                              "engine.ownership_change",          "engine.security_change",
                                              "engine.resource_lifecycle_change", "engine.status_transition_change",
                                              "engine.scope_resolution_change",   "engine.capability_boundary_change"};
  for (const auto& kind : kinds) {
    INFO("kind: " << kind);
    const std::array facts{f(kind), acceptance()};
    const auto       out = must_profile(profile_::compile(facts));
    CHECK(out.type == work_type::engine);
    CHECK(out.work_type_rule_id == "work-type.engine.v1");
  }
}

TEST_CASE("models.profile: every schema and architectural fact kind reaches its rule", "[models][profile]") {
  const std::array<std::string_view, 3> schema_kinds{"schema.migration_touched", "schema.version_contract_change",
                                                     "schema.constraint_or_index_redesign"};
  for (const auto& kind : schema_kinds) {
    INFO("kind: " << kind);
    const std::array facts{f(kind), acceptance()};
    CHECK(must_profile(profile_::compile(facts)).type == work_type::schema);
  }

  const std::array<std::string_view, 2> arch_kinds{"architectural.new_subsystem", "architectural.new_binary"};
  for (const auto& kind : arch_kinds) {
    INFO("kind: " << kind);
    const std::array facts{f(kind), acceptance()};
    CHECK(must_profile(profile_::compile(facts)).type == work_type::architectural);
  }
}

TEST_CASE("models.profile: a fact recorded false does not match the rule keyed on it", "[models][profile]") {
  for (const auto& text : {std::string_view{"false"}, std::string_view{"0"}}) {
    INFO("text: " << text);
    const std::array facts{f("schema.migration_touched", text), acceptance()};
    const auto       out = must_profile(profile_::compile(facts));
    CHECK(out.type == work_type::feature);
    CHECK(out.work_type_rule_id == "work-type.feature.v1");
    CHECK(out.matched_facts.empty());
  }

  // Every OTHER text asserts, including the empty string -- the oracle's
  // check is a negated equality, not a truthiness parse.
  for (const auto& text : {std::string_view{""}, std::string_view{"no"}, std::string_view{"1"}}) {
    INFO("text: " << text);
    const std::array facts{f("schema.migration_touched", text), acceptance()};
    CHECK(must_profile(profile_::compile(facts)).type == work_type::schema);
  }
}

TEST_CASE("models.profile: a matched work-type rule cites the fact that matched it", "[models][profile]") {
  const std::array facts{fact{.kind           = "schema.migration_touched",
                              .locator        = "migrations/00042_thing.up.sql",
                              .text           = "true",
                              .provenance     = "artifact",
                              .current_digest = "abc123",
                              .id             = 99},
                         acceptance()};
  const auto       out = must_profile(profile_::compile(facts));

  REQUIRE(out.matched_facts.size() == 1);
  CHECK(out.matched_facts[0].kind == "schema.migration_touched");
  CHECK(out.matched_facts[0].locator == "migrations/00042_thing.up.sql");
  CHECK(out.matched_facts[0].source_entity_kind == "artifact"); // from `provenance`
  CHECK(out.matched_facts[0].source_entity_id == 99);
  CHECK(out.matched_facts[0].source_digest == "abc123"); // from `current_digest`
  CHECK(out.matched_facts[0].text == "true");

  REQUIRE(out.citations.size() == 1);
  CHECK(out.citations[0].rule_id == "work-type.schema.v1");
  CHECK(out.citations[0].fact_kind == "schema.migration_touched");
  CHECK(out.citations[0].locator == "migrations/00042_thing.up.sql");
}

// ---------------------------------------------------------------------------
// Complexity
// ---------------------------------------------------------------------------

TEST_CASE("models.profile: explicit risk short-circuits capacity entirely", "[models][profile]") {
  // Capacity says "tiny"; the operator says "risky". Risk wins, and the
  // threshold id stays EMPTY because no capacity metric decided it.
  const std::array facts{f("risk.explicit"), f("capacity.touched_unit_count", "1"), acceptance()};
  const auto       out = must_profile(profile_::compile(facts));
  CHECK(out.risk == complexity::high_risk);
  CHECK(out.complexity_threshold_id.empty());
  CHECK(out.tier_floor == tier::large);
  CHECK(out.tier_floor_rule_id == "tier-floor.high-risk.v1");

  // Without the risk fact the same capacity reading is `bounded` -- proving
  // the short-circuit is what changed the answer.
  const std::array without{f("capacity.touched_unit_count", "1"), acceptance()};
  CHECK(must_profile(profile_::compile(without)).risk == complexity::bounded);
}

TEST_CASE("models.profile: a capacity reading at or above large_min is high risk", "[models][profile]") {
  const std::array at{f("capacity.touched_unit_count", "12"), acceptance()};
  const auto       out = must_profile(profile_::compile(at));
  CHECK(out.risk == complexity::high_risk);
  CHECK(out.complexity_threshold_id == "complexity.touched-units.v1");

  // The boundary is `>=`, so one below is NOT high risk.
  const std::array below{f("capacity.touched_unit_count", "11"), acceptance()};
  CHECK(must_profile(profile_::compile(below)).risk == complexity::standard);
}

TEST_CASE("models.profile: the validation-gate metric has its own boundaries", "[models][profile]") {
  const std::array at{f("capacity.validation_gate_count", "6"), acceptance()};
  const auto       out = must_profile(profile_::compile(at));
  CHECK(out.risk == complexity::high_risk);
  CHECK(out.complexity_threshold_id == "complexity.validation-gates.v1");

  const std::array below{f("capacity.validation_gate_count", "5"), acceptance()};
  CHECK(must_profile(profile_::compile(below)).risk == complexity::standard);
}

TEST_CASE("models.profile: bounded requires EVERY populated metric to sit at or under its max", "[models][profile]") {
  // touched_unit_count <= 2 and validation_gate_count <= 1 -> bounded.
  const std::array both_small{f("capacity.touched_unit_count", "2"), f("capacity.validation_gate_count", "1"), acceptance()};
  CHECK(must_profile(profile_::compile(both_small)).risk == complexity::bounded);

  // One metric over its bounded_max is enough to deny `bounded` -- and it is
  // `standard`, not high risk, because it is still under large_min.
  const std::array one_over{f("capacity.touched_unit_count", "3"), f("capacity.validation_gate_count", "1"), acceptance()};
  CHECK(must_profile(profile_::compile(one_over)).risk == complexity::standard);

  const std::array other_over{f("capacity.touched_unit_count", "2"), f("capacity.validation_gate_count", "2"), acceptance()};
  CHECK(must_profile(profile_::compile(other_over)).risk == complexity::standard);
}

TEST_CASE("models.profile: no capacity reading at all leaves complexity standard", "[models][profile]") {
  // `bounded` requires having SEEN a capacity metric. Absence is not
  // smallness -- an unmeasured task is not a small one.
  const std::array facts{acceptance()};
  const auto       out = must_profile(profile_::compile(facts));
  CHECK(out.risk == complexity::standard);
  CHECK(out.complexity_threshold_id.empty());
  CHECK(out.tier_floor == tier::medium);
}

TEST_CASE("models.profile: an unparseable capacity reading is invisible, not fatal", "[models][profile]") {
  // The oracle's `parseInt(...) catch null` swallows the failure, so a
  // garbage reading behaves exactly like an absent one.
  for (const auto& text : {std::string_view{"lots"}, std::string_view{""}, std::string_view{"3.5"}}) {
    INFO("text: " << text);
    const std::array facts{f("capacity.touched_unit_count", text), acceptance()};
    const auto       out = must_profile(profile_::compile(facts));
    CHECK(out.risk == complexity::standard);
  }

  // The oracle trims SPACE and TAB only, so a trailing NEWLINE is a parse
  // failure -- and a surrounding space is not.
  const std::array newline{f("capacity.touched_unit_count", "1\n"), acceptance()};
  CHECK(must_profile(profile_::compile(newline)).risk == complexity::standard);

  const std::array spaced{f("capacity.touched_unit_count", "  1\t"), acceptance()};
  CHECK(must_profile(profile_::compile(spaced)).risk == complexity::bounded);

  // Zig's parseInt accepts a leading `+`; std::from_chars does not, so this
  // pins the explicit sign handling.
  const std::array plus{f("capacity.touched_unit_count", "+1"), acceptance()};
  CHECK(must_profile(profile_::compile(plus)).risk == complexity::bounded);
}

TEST_CASE("models.profile: the bounded threshold id is ALWAYS the touched-units row", "[models][profile]") {
  // A deliberate oracle quirk. When only `validation_gate_count` was
  // populated, the reported `complexity_threshold_id` is STILL
  // `complexity.touched-units.v1` -- the oracle looks that row up
  // unconditionally on the bounded path. Reproduced rather than tidied.
  const std::array facts{f("capacity.validation_gate_count", "1"), acceptance()};
  const auto       out = must_profile(profile_::compile(facts));
  CHECK(out.risk == complexity::bounded);
  CHECK(out.complexity_threshold_id == "complexity.touched-units.v1");
}

// ---------------------------------------------------------------------------
// Tier floor
// ---------------------------------------------------------------------------

TEST_CASE("models.profile: bounded high-judgment observable work floors at LARGE", "[models][profile]") {
  // Small-looking but consequential: the whole point of the rule.
  const std::array facts{f("capacity.touched_unit_count", "1"), f("acceptance.high_judgment"),
                         f("acceptance.observable_correctness"), acceptance()};
  const auto       out = must_profile(profile_::compile(facts));
  CHECK(out.risk == complexity::bounded);
  CHECK(out.tier_floor == tier::large);
  CHECK(out.tier_floor_rule_id == "tier-floor.bounded-high-judgment.v1");

  // BOTH facts are required; either alone falls back to the small floor.
  const std::array judgment_only{f("capacity.touched_unit_count", "1"), f("acceptance.high_judgment"), acceptance()};
  CHECK(must_profile(profile_::compile(judgment_only)).tier_floor == tier::small);

  const std::array observable_only{f("capacity.touched_unit_count", "1"), f("acceptance.observable_correctness"), acceptance()};
  CHECK(must_profile(profile_::compile(observable_only)).tier_floor == tier::small);
}

TEST_CASE("models.profile: the high-judgment floor cites the fact that raised it", "[models][profile]") {
  const std::array facts{f("capacity.touched_unit_count", "1"), f("acceptance.high_judgment"),
                         f("acceptance.observable_correctness"), acceptance()};
  const auto       out = must_profile(profile_::compile(facts));

  const auto it = std::ranges::find(out.citations, "tier-floor.bounded-high-judgment.v1", &profile_::citation::rule_id);
  REQUIRE(it != out.citations.end());
  CHECK(it->fact_kind == "acceptance.high_judgment");
}

TEST_CASE("models.profile: high risk floors at large regardless of judgment facts", "[models][profile]") {
  const std::array facts{f("risk.explicit"), f("acceptance.high_judgment"), acceptance()};
  const auto       out = must_profile(profile_::compile(facts));
  CHECK(out.tier_floor == tier::large);
  CHECK(out.tier_floor_rule_id == "tier-floor.high-risk.v1");
}

TEST_CASE("models.profile: standard complexity floors at medium", "[models][profile]") {
  const std::array facts{f("capacity.touched_unit_count", "5"), acceptance()};
  const auto       out = must_profile(profile_::compile(facts));
  CHECK(out.risk == complexity::standard);
  CHECK(out.tier_floor == tier::medium);
  CHECK(out.tier_floor_rule_id == "tier-floor.standard.v1");
}

// ---------------------------------------------------------------------------
// Tier acceptance
// ---------------------------------------------------------------------------

TEST_CASE("models.profile: an operator may raise the tier but never lower it", "[models][profile]") {
  const std::array facts{f("capacity.touched_unit_count", "1"), acceptance()};
  const auto       out = must_profile(profile_::compile(facts));
  REQUIRE(out.tier_floor == tier::small);

  CHECK(profile_::accept_tier(out, tier::small) == tier::small);
  CHECK(profile_::accept_tier(out, tier::medium) == tier::medium);
  CHECK(profile_::accept_tier(out, tier::large) == tier::large);

  const std::array risky{f("risk.explicit"), acceptance()};
  const auto       high = must_profile(profile_::compile(risky));
  REQUIRE(high.tier_floor == tier::large);
  CHECK(profile_::accept_tier(high, tier::large) == tier::large);
  CHECK(profile_::accept_tier(high, tier::medium) == std::nullopt);
  CHECK(profile_::accept_tier(high, tier::small) == std::nullopt);
}

TEST_CASE("models.profile: threshold_for covers exactly the two capacity metrics", "[models][profile]") {
  CHECK(profile_::threshold_for("capacity.touched_unit_count").has_value());
  CHECK(profile_::threshold_for("capacity.validation_gate_count").has_value());
  CHECK(profile_::threshold_for("risk.explicit") == std::nullopt);
  CHECK(profile_::threshold_for("") == std::nullopt);
}

TEST_CASE("models.profile: not_ready reasons render the oracle's own spellings", "[models][profile]") {
  CHECK(profile_::not_ready_reason_name(profile_::not_ready_reason::contradictory_facts) == "contradictory_facts");
  CHECK(profile_::not_ready_reason_name(profile_::not_ready_reason::missing_required_fact) == "missing_required_fact");
}

TEST_CASE("models.profile: every compiled profile carries the rule version", "[models][profile]") {
  const std::array facts{acceptance()};
  CHECK(must_profile(profile_::compile(facts)).rule_version_ == "routing-profile-v1");
}

TEST_CASE("models.profile: policy_not_ready is UNREACHABLE under the shipped threshold table", "[models][profile]") {
  // Stated as a test rather than a comment because it is a property of the
  // DATA, and the data can change without anyone revisiting this file.
  //
  // `compile` returns `policy_gap` on two paths, and the shipped table closes
  // both: a metric with no covering row (impossible -- the two capacity
  // metrics `compile` iterates are exactly the two rows in `thresholds()`),
  // and a covering row whose `large_min` is empty (impossible -- both rows
  // populate it). So no fixture in this file can produce a `policy_gap`, and
  // that is a fact about the policy, not a coverage hole in the port.
  //
  // If a future row drops `large_min`, or a third capacity metric is
  // iterated without a matching row, this test fails and the branch becomes
  // reachable -- at which point it needs a real fixture and an oracle diff.
  for (const auto& metric :
       {std::string_view{"capacity.touched_unit_count"}, std::string_view{"capacity.validation_gate_count"}}) {
    INFO("metric: " << metric);
    const auto row = profile_::threshold_for(metric);
    REQUIRE(row.has_value());
    CHECK(row->large_min.has_value());
  }

  // And confirm no fixture accidentally reaches it.
  const std::array facts{f("capacity.touched_unit_count", "50"), f("capacity.validation_gate_count", "50"), acceptance()};
  CHECK_FALSE(std::holds_alternative<profile_::policy_gap>(profile_::compile(facts)));
}
