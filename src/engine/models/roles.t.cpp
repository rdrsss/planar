// @file roles.t.cpp
// @brief Unit tests for `planar.engine.models.roles` (plan 996, task 6111).
//
// ## ORACLE PROVENANCE
//
// Unlike `profile`, this module's behavior IS reachable through the parity
// oracle today, because every fallback path runs without a ready packet. The
// transcripts pinned below are the exact bytes the Zig binary emitted for
// this task, captured with all four home variables redirected under a scratch
// root (HOME, PLANAR_HOME, PLANAR_LOCAL_HOME, CODEX_HOME) and PLANAR_DB
// pointing into it, so nothing outside the temp tree was read or written:
//
//   $ planar models resolve --role planner --json
//   {"resolution_version":"routing-roles-v1","role":"planner",
//    "packet_class":"planning","source":"static_fallback",
//    "packet_backed":false,"tier":"medium","work_type":null,
//    "complexity":null,"fallback_reason":"no_packet",
//    "first_readiness_reason":null,"rule_version":null}
//
//   $ planar models resolve --role spec-reviewer --json
//   ... "role":"spec_reviewer" ...        <- HYPHEN in, UNDERSCORE out
//
//   $ planar models resolve --role spec_reviewer --json
//   ... exit 0, "role":"spec_reviewer"    <- UNDERSCORE also ACCEPTED,
//                                            though --help lists only the
//                                            hyphenated spelling
//
//   $ planar models resolve --role planner --fallback-tier small --json
//   ... "tier":"small" ...
//
//   $ planar models resolve --role coder --json
//   exit 2, stderr: error: --task is required for task-bound role 'coder'
//   (identical with --fallback-tier large: the refusal precedes it)
//
//   $ planar models resolve --role nosuch --json
//   exit 2, stderr: error: unknown role 'nosuch'
//
// And, against a seeded scratch project (assoc -> plan -> task, no
// materialized facts), the task-bound UNREADY path:
//
//   $ planar models resolve --role coder --task 1 --json
//   {"resolution_version":"routing-roles-v1","role":"coder",
//    "packet_class":"task","source":"static_fallback","packet_backed":false,
//    "tier":"medium","work_type":null,"complexity":null,
//    "fallback_reason":"packet_not_ready",
//    "first_readiness_reason":"missing_acceptance_section","rule_version":null}
//
// The refusal wording and exit codes belong to the not-yet-written handler,
// so they are recorded here for the cycle that wires it rather than asserted.
// What IS asserted below is everything this module owns: the role table, the
// wire mapping, the packet classes, and the three resolution entry points.

import std;
import planar.engine.models.roles;
import planar.engine.models.profile;
import planar.engine.models.ranking;
import planar.engine.models.registry;

#include <catch2/catch_test_macros.hpp>

namespace roles    = planar::engine::models::roles;
namespace profile_ = planar::engine::models::profile;
namespace ranking  = planar::engine::models::ranking;
namespace registry = planar::engine::models::registry;

using ranking::complexity;
using ranking::work_type;
using registry::tier;
using roles::fallback_reason;
using roles::packet_class;
using roles::role;
using roles::source;

namespace {

/// The nine roles paired with their EMITTED spelling and packet class. Spelled
/// out as literals rather than read back from the module, so a wrong entry in
/// the module's own table cannot agree with itself.
struct expectation {
  role             value;
  std::string_view emitted;
  std::string_view hyphenated;
  packet_class     class_;
};

constexpr std::array all_roles{
    expectation{role::planner, "planner", "planner", packet_class::planning},
    expectation{role::spec_reviewer, "spec_reviewer", "spec-reviewer", packet_class::planning},
    expectation{role::ingestor, "ingestor", "ingestor", packet_class::planning},
    expectation{role::orchestrator, "orchestrator", "orchestrator", packet_class::planning},
    expectation{role::coder, "coder", "coder", packet_class::task},
    expectation{role::test_coder, "test_coder", "test-coder", packet_class::task},
    expectation{role::reviewer, "reviewer", "reviewer", packet_class::task},
    expectation{role::research, "research", "research", packet_class::task},
    expectation{role::janitor, "janitor", "janitor", packet_class::task},
};

/// A minimal compiled profile, used to drive `resolve_task`'s packet-backed
/// branch without depending on `profile::compile`'s rule tables.
[[nodiscard]] auto some_profile(tier floor, work_type type, complexity risk) -> profile_::profile {
  return profile_::profile{.type                    = type,
                           .work_type_rule_id       = "work-type.schema.v1",
                           .rule_version_           = profile_::rule_version,
                           .risk                    = risk,
                           .complexity_threshold_id = "",
                           .tier_floor              = floor,
                           .tier_floor_rule_id      = "tier-floor.high-risk.v1",
                           .matched_facts           = {},
                           .citations               = {}};
}

} // namespace

// ---------------------------------------------------------------------------
// FIXTURE SHAPE. Runs before any behavioral case so a table that silently
// matches nothing cannot pass vacuously.
// ---------------------------------------------------------------------------

TEST_CASE("models.roles: the role table has the shape every later case assumes", "[models][roles]") {
  REQUIRE(all_roles.size() == 9);

  // FOUR planning roles and FIVE task roles. The split is the module's whole
  // subject, so it is pinned by count before anything reads it.
  const auto planning = std::ranges::count(all_roles, packet_class::planning, &expectation::class_);
  const auto task     = std::ranges::count(all_roles, packet_class::task, &expectation::class_);
  CHECK(planning == 4);
  CHECK(task == 5);

  // Exactly two roles have a spelling that CHANGES between wire and enum. If
  // this ever reaches zero, the hyphen mapping has become untested.
  const auto hyphenated = std::ranges::count_if(all_roles, [](const expectation& e) { return e.emitted != e.hyphenated; });
  CHECK(hyphenated == 2);

  CHECK(roles::resolution_version == "routing-roles-v1");
}

TEST_CASE("models.roles: every role round-trips through its wire spelling", "[models][roles]") {
  for (const auto& e : all_roles) {
    INFO("role: " << e.emitted);
    // The HYPHENATED spelling is what `--help` documents and what an operator
    // types.
    CHECK(roles::role_from_wire(e.hyphenated) == e.value);
    // The EMITTED spelling is always underscored (`@tagName` in the oracle).
    CHECK(roles::role_to_text(e.value) == e.emitted);
    CHECK(roles::class_of(e.value) == e.class_);
  }
}

TEST_CASE("models.roles: the UNDERSCORED spelling is accepted too, as the oracle accepts it", "[models][roles]") {
  // Verified against the parity oracle, which exits 0 on
  // `--role spec_reviewer` and reports `"role":"spec_reviewer"`, even though
  // `--help` lists only `spec-reviewer`. The hyphen-to-underscore map is a
  // no-op on an already-underscored name, so the alias falls out of the
  // mapping rather than being declared anywhere. Pinned because it is
  // observable behavior an operator may depend on -- not endorsed.
  CHECK(roles::role_from_wire("spec_reviewer") == role::spec_reviewer);
  CHECK(roles::role_from_wire("test_coder") == role::test_coder);
}

TEST_CASE("models.roles: an unknown role is refused rather than defaulted", "[models][roles]") {
  CHECK(roles::role_from_wire("nosuch") == std::nullopt);
  CHECK(roles::role_from_wire("") == std::nullopt);
  CHECK(roles::role_from_wire("CODER") == std::nullopt); // case-sensitive
  CHECK(roles::role_from_wire("spec reviewer") == std::nullopt);
  // Underscore-to-hyphen is NOT applied; only hyphen-to-underscore.
  CHECK(roles::role_from_wire("--role") == std::nullopt);

  // The absence assertions above could pass because `role_from_wire` refused
  // everything, so assert a present case in the same test.
  CHECK(roles::role_from_wire("coder") == role::coder);
}

TEST_CASE("models.roles: an over-long role name is refused before lookup", "[models][roles]") {
  // The oracle copies the flag into a fixed 64-byte buffer and dies if the
  // name does not fit. Reproduced so the refusal happens at the same
  // boundary rather than as an incidental lookup miss.
  const std::string long_name(65, 'a');
  CHECK(roles::role_from_wire(long_name) == std::nullopt);

  const std::string at_limit(64, 'a');
  CHECK(roles::role_from_wire(at_limit) == std::nullopt); // fits, but no such role
}

TEST_CASE("models.roles: the enum spellings render as the oracle prints them", "[models][roles]") {
  CHECK(roles::packet_class_to_text(packet_class::planning) == "planning");
  CHECK(roles::packet_class_to_text(packet_class::task) == "task");
  CHECK(roles::source_to_text(source::packet) == "packet");
  CHECK(roles::source_to_text(source::static_fallback) == "static_fallback");
  CHECK(roles::fallback_reason_to_text(fallback_reason::no_packet) == "no_packet");
  CHECK(roles::fallback_reason_to_text(fallback_reason::packet_not_ready) == "packet_not_ready");
  CHECK(roles::fallback_reason_to_text(fallback_reason::policy_not_ready) == "policy_not_ready");
}

// ---------------------------------------------------------------------------
// Planning resolution
// ---------------------------------------------------------------------------

TEST_CASE("models.roles: a planning role with NO packet reproduces the oracle's fallback", "[models][roles]") {
  // Byte-for-byte the oracle transcript for `models resolve --role planner
  // --json` on an empty database, field by field.
  const auto out = roles::resolve_planning(role::planner, std::nullopt, {.tier_ = tier::medium}, "routing-packet-v1");

  CHECK(roles::role_to_text(out.role_) == "planner");
  CHECK(roles::packet_class_to_text(out.class_) == "planning");
  CHECK(roles::source_to_text(out.source_) == "static_fallback");
  CHECK(roles::packet_backed(out) == false);
  CHECK(registry::tier_to_text(out.tier_) == "medium");
  CHECK(out.work_type_ == std::nullopt);
  CHECK(out.complexity_ == std::nullopt);
  REQUIRE(out.fallback_reason_.has_value());
  CHECK(roles::fallback_reason_to_text(*out.fallback_reason_) == "no_packet");
  CHECK(out.rule_version == std::nullopt);
}

TEST_CASE("models.roles: the configured fallback tier is what a no-packet resolution reports", "[models][roles]") {
  // Oracle: --fallback-tier small -> "tier":"small"; large -> "tier":"large".
  for (const auto& t : {tier::small, tier::medium, tier::large}) {
    INFO("tier: " << registry::tier_to_text(t));
    const auto out = roles::resolve_planning(role::planner, std::nullopt, {.tier_ = t}, "routing-packet-v1");
    CHECK(out.tier_ == t);
    CHECK(roles::packet_backed(out) == false);
  }
}

TEST_CASE("models.roles: an UNREADY planning packet is distinguishable from an ABSENT one", "[models][roles]") {
  // The distinction the verb exists to show: both fall back to the same tier,
  // and only the reason tells the operator which happened.
  const auto absent  = roles::resolve_planning(role::ingestor, std::nullopt, {.tier_ = tier::medium}, "v");
  const auto unready = roles::resolve_planning(role::ingestor, false, {.tier_ = tier::medium}, "v");

  CHECK(absent.tier_ == unready.tier_);
  CHECK(absent.source_ == unready.source_);
  REQUIRE(absent.fallback_reason_.has_value());
  REQUIRE(unready.fallback_reason_.has_value());
  CHECK(*absent.fallback_reason_ == fallback_reason::no_packet);
  CHECK(*unready.fallback_reason_ == fallback_reason::packet_not_ready);
  CHECK(*absent.fallback_reason_ != *unready.fallback_reason_);
}

TEST_CASE("models.roles: a READY planning packet is packet-backed but classifies nothing", "[models][roles]") {
  const auto out = roles::resolve_planning(role::orchestrator, true, {.tier_ = tier::large}, "routing-packet-v1");

  CHECK(roles::packet_backed(out) == true);
  CHECK(roles::source_to_text(out.source_) == "packet");
  // The tier is STILL the configured fallback -- a ready planning packet
  // establishes readiness, it does not derive a tier.
  CHECK(out.tier_ == tier::large);
  // And it carries no classification, because there is no task to classify.
  CHECK(out.work_type_ == std::nullopt);
  CHECK(out.complexity_ == std::nullopt);
  CHECK(out.fallback_reason_ == std::nullopt);
  // It IS stamped with the packet layer's policy version, which is how a
  // reader tells a backed planning decision from an unbacked one.
  REQUIRE(out.rule_version.has_value());
  CHECK(*out.rule_version == "routing-packet-v1");
}

TEST_CASE("models.roles: every planning role resolves through the planning path", "[models][roles]") {
  for (const auto& e : all_roles) {
    if (e.class_ != packet_class::planning) {
      continue;
    }
    INFO("role: " << e.emitted);
    const auto out = roles::resolve_planning(e.value, std::nullopt, {.tier_ = tier::medium}, "v");
    CHECK(out.class_ == packet_class::planning);
    CHECK(roles::role_to_text(out.role_) == e.emitted);
  }
}

// ---------------------------------------------------------------------------
// Task resolution
// ---------------------------------------------------------------------------

TEST_CASE("models.roles: an UNREADY task packet never reaches the profile", "[models][roles]") {
  // Reproduces the seeded-project oracle transcript: a task with no
  // materialized facts resolves to the static fallback with
  // `packet_not_ready`. The outcome argument is deliberately a REAL profile
  // here -- if readiness were ignored, this would come back packet-backed.
  const auto outcome = profile_::outcome{some_profile(tier::large, work_type::schema, complexity::high_risk)};
  const auto out     = roles::resolve_task_packet(role::coder, false, outcome, {.tier_ = tier::medium});

  CHECK(roles::role_to_text(out.role_) == "coder");
  CHECK(roles::packet_class_to_text(out.class_) == "task");
  CHECK(roles::source_to_text(out.source_) == "static_fallback");
  CHECK(roles::packet_backed(out) == false);
  CHECK(registry::tier_to_text(out.tier_) == "medium");
  CHECK(out.work_type_ == std::nullopt);
  CHECK(out.complexity_ == std::nullopt);
  REQUIRE(out.fallback_reason_.has_value());
  CHECK(roles::fallback_reason_to_text(*out.fallback_reason_) == "packet_not_ready");
  CHECK(out.rule_version == std::nullopt);

  // The PRESENT case, so the assertions above cannot pass because the
  // fallback fires unconditionally.
  const auto ready = roles::resolve_task_packet(role::coder, true, outcome, {.tier_ = tier::medium});
  CHECK(roles::packet_backed(ready) == true);
}

TEST_CASE("models.roles: a compiled profile supplies the tier FLOOR and the classification", "[models][roles]") {
  const auto outcome = profile_::outcome{some_profile(tier::large, work_type::schema, complexity::high_risk)};
  const auto out     = roles::resolve_task(role::reviewer, outcome, {.tier_ = tier::small});

  CHECK(roles::packet_backed(out) == true);
  CHECK(roles::source_to_text(out.source_) == "packet");
  // The FLOOR wins over the configured fallback -- the fallback is not
  // consulted at all on this path.
  CHECK(out.tier_ == tier::large);
  REQUIRE(out.work_type_.has_value());
  CHECK(ranking::work_type_to_text(*out.work_type_) == "schema");
  REQUIRE(out.complexity_.has_value());
  CHECK(*out.complexity_ == complexity::high_risk);
  CHECK(out.fallback_reason_ == std::nullopt);
  REQUIRE(out.rule_version.has_value());
  CHECK(*out.rule_version == "routing-profile-v1");
}

TEST_CASE("models.roles: a task role with NO outcome falls back with no_packet", "[models][roles]") {
  const auto out = roles::resolve_task(role::janitor, std::nullopt, {.tier_ = tier::small});
  CHECK(roles::packet_backed(out) == false);
  REQUIRE(out.fallback_reason_.has_value());
  CHECK(*out.fallback_reason_ == fallback_reason::no_packet);
  CHECK(out.tier_ == tier::small);
}

TEST_CASE("models.roles: a not_ready outcome and a policy gap report DIFFERENT reasons", "[models][roles]") {
  // The whole point of `profile::outcome` having three alternatives: a broken
  // TASK and an unconfigured POLICY must not read the same to an operator.
  const auto broken_task   = roles::resolve_task(role::coder, profile_::outcome{profile_::not_ready_reason::contradictory_facts},
                                                 {.tier_ = tier::medium});
  const auto broken_policy = roles::resolve_task(
      role::coder, profile_::outcome{profile_::policy_gap{.metric = "capacity.touched_unit_count", .threshold_id = std::nullopt}},
      {.tier_ = tier::medium});

  REQUIRE(broken_task.fallback_reason_.has_value());
  REQUIRE(broken_policy.fallback_reason_.has_value());
  CHECK(*broken_task.fallback_reason_ == fallback_reason::packet_not_ready);
  CHECK(*broken_policy.fallback_reason_ == fallback_reason::policy_not_ready);
  CHECK(*broken_task.fallback_reason_ != *broken_policy.fallback_reason_);

  // Both are fallbacks, and neither leaks a classification.
  for (const auto& out : {broken_task, broken_policy}) {
    CHECK(roles::packet_backed(out) == false);
    CHECK(out.work_type_ == std::nullopt);
    CHECK(out.complexity_ == std::nullopt);
    CHECK(out.rule_version == std::nullopt);
  }

  // `missing_required_fact` takes the same path as `contradictory_facts`.
  const auto missing = roles::resolve_task(role::coder, profile_::outcome{profile_::not_ready_reason::missing_required_fact},
                                           {.tier_ = tier::medium});
  REQUIRE(missing.fallback_reason_.has_value());
  CHECK(*missing.fallback_reason_ == fallback_reason::packet_not_ready);
}

TEST_CASE("models.roles: every task role resolves through the task path", "[models][roles]") {
  const auto outcome = profile_::outcome{some_profile(tier::medium, work_type::cli, complexity::standard)};
  for (const auto& e : all_roles) {
    if (e.class_ != packet_class::task) {
      continue;
    }
    INFO("role: " << e.emitted);
    const auto out = roles::resolve_task_packet(e.value, true, outcome, {.tier_ = tier::small});
    CHECK(out.class_ == packet_class::task);
    CHECK(roles::role_to_text(out.role_) == e.emitted);
    CHECK(roles::packet_backed(out) == true);
  }
}

TEST_CASE("models.roles: a fallback NEVER carries a work type or complexity", "[models][roles]") {
  // A tier shown without provenance reads identically to one derived from
  // real evidence; this is the invariant that prevents it.
  const std::array fallbacks{
      roles::resolve_task(role::coder, std::nullopt, {.tier_ = tier::large}),
      roles::resolve_task_packet(role::coder, false, std::nullopt, {.tier_ = tier::large}),
      roles::resolve_planning(role::planner, std::nullopt, {.tier_ = tier::large}, "v"),
      roles::resolve_planning(role::planner, false, {.tier_ = tier::large}, "v"),
  };
  for (const auto& out : fallbacks) {
    CHECK(roles::packet_backed(out) == false);
    CHECK(out.source_ == source::static_fallback);
    CHECK(out.work_type_ == std::nullopt);
    CHECK(out.complexity_ == std::nullopt);
    CHECK(out.rule_version == std::nullopt);
    REQUIRE(out.fallback_reason_.has_value());
  }
}

TEST_CASE("models.roles: a packet-backed resolution NEVER carries a fallback reason", "[models][roles]") {
  const auto       outcome = profile_::outcome{some_profile(tier::medium, work_type::feature, complexity::bounded)};
  const std::array backed{
      roles::resolve_task(role::coder, outcome, {.tier_ = tier::small}),
      roles::resolve_task_packet(role::coder, true, outcome, {.tier_ = tier::small}),
      roles::resolve_planning(role::planner, true, {.tier_ = tier::small}, "v"),
  };
  for (const auto& out : backed) {
    CHECK(roles::packet_backed(out) == true);
    CHECK(out.source_ == source::packet);
    CHECK(out.fallback_reason_ == std::nullopt);
    CHECK(out.rule_version.has_value());
  }
}
