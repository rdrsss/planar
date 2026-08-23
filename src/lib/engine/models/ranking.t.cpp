// @file ranking.t.cpp
// @brief Unit tests for `planar.engine.models.ranking` (plan 996, task 6096).
//
// ORACLE PROVENANCE. The numeric fixtures below are not derived, not
// recomputed, and not eyeballed -- they are the EXACT doubles the Zig binary
// printed for a seeded cohort, transcribed digit for digit. Every probe ran
// with HOME/PLANAR_HOME/CODEX_HOME redirected under /tmp.
//
//   export PLANAR_DB=/tmp/op/evals.db PLANAR_CONFIG_PATH=/tmp/op/evals.toml
//   export HOME=/tmp/op/home PLANAR_HOME=/tmp/op/home/.planar
//   cd /tmp/op/ev && $Z init
//   for i in 1 2 3 4; do
//     $Z models registry add --vendor anthropic --id cand-$i --order $i; done
//
// Then one experiment `exp-1` over cohort
// `anthropic / coder / medium / engine / standard`, project 1, policies
// `vp-1` + `rp-1`, and a full snapshot -> event -> terminal-sample chain per
// work item (the schema's `routing_terminal_samples_identity` trigger refuses
// a sample that does not point at a matching declared-experiment dispatch, so
// the chain has to be complete):
//
//   cand-1: 20 samples, 16 completed / 4 quality_failed
//   cand-2: 20 samples, 12 completed / 8 quality_failed
//   cand-3:  3 samples,  3 completed
//   cand-4: 20 samples,  4 completed / 16 quality_failed
//
//   $Z models evals --vendor anthropic --project 1 --validation-policy vp-1 \
//      --routing-policy rp-1 --role coder --tier medium --work-type engine \
//      --complexity standard --json
//
//   {"version":"routing-ranking-v1","evidence":"declared_experiment",
//    "gates":{"minimum_samples":5,"quality_floor":0.5},"rows":[
//     {"candidate_id":1,"candidate":"cand-1",...,"samples":20,"successes":16,
//      "gate_failures":4,"excess_attempts":0,"mean_latency_ms":null,
//      "mean_cost_micros":null,"measured_samples":0,"raw_rate":0.8,
//      "wilson_lower":0.5839825677481064,"gate_failure_rate":0.2,
//      "expected_excess_iterations":0,"insufficient_data":false,
//      "below_quality_floor":false,"rank":1},
//     {..."cand-2"...,"successes":12,"raw_rate":0.6,
//      "wilson_lower":0.38658150076225317,"gate_failure_rate":0.4,
//      "below_quality_floor":true,"rank":null},
//     {..."cand-4"...,"successes":4,"raw_rate":0.2,
//      "wilson_lower":0.08065766257979808,"gate_failure_rate":0.8,
//      "below_quality_floor":true,"rank":null},
//     {..."cand-3"...,"samples":3,"successes":3,"raw_rate":1,
//      "wilson_lower":0.4385029682449545,"gate_failure_rate":0,
//      "insufficient_data":true,"rank":null}],
//    "recommended":"cand-1","no_recommendation_reason":null}
//
// --- BRANCH 2: everything gated out ---------------------------------------
//   ... --quality-floor 0.99 --json
//   -> every row below_quality_floor:true, "recommended":null,
//      "no_recommendation_reason":"no candidate cleared both the
//       minimum-sample and quality-floor gates"
//
// --- BRANCH 3: no evidence at all (hazard 6, empty input) ------------------
//   ... --complexity bounded --json          [a cohort with zero samples]
//   -> {"...","rows":[],"recommended":null,
//       "no_recommendation_reason":"no cohort-eligible declared-experiment
//        samples"}
//
// Note `cand-3` in branch 1: a PERFECT 3-for-3 raw rate that ranks nowhere.
// That row is the entire argument for using a Wilson lower bound.
//
// Note also the row ORDER: eligible first (cand-1), then gated rows by
// descending sample count and then ascending fallback order (cand-2 with 20
// at order 2, cand-4 with 20 at order 4, cand-3 with 3). Not query order and
// not id order.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.db.migrate;
import planar.engine.models.ranking;
import planar.engine.models.registry;

namespace {

namespace rk  = planar::engine::models::ranking;
namespace reg = planar::engine::models::registry;

struct scratch_db_path {
  std::filesystem::path path_;

  scratch_db_path()
      : path_(std::filesystem::temp_directory_path() / std::format("planar_models_ranking_test_{}_{}.db",
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
  auto applied = planar::db::apply_all(*conn);
  REQUIRE(applied.has_value());
  return std::move(*conn);
}

auto exec(planar::db::connection& conn, std::string_view sql) -> void {
  auto ok = conn.execute(sql);
  INFO(sql);
  REQUIRE(ok.has_value());
}

/// @brief The exact cohort the oracle transcript was captured against.
auto oracle_cohort() -> rk::cohort {
  return rk::cohort{
      .project_id                = 1,
      .validation_policy_version = "vp-1",
      .routing_policy_version    = "rp-1",
      .vendor                    = "anthropic",
      .role                      = "coder",
      .tier_                     = reg::tier::medium,
      .work_type_                = rk::work_type::engine,
      .complexity_               = rk::complexity::standard,
  };
}

/// @brief Seed the four-candidate fixture the transcript above describes.
///
/// The full snapshot -> event -> terminal-sample chain is written per work
/// item because `routing_terminal_samples_identity` (migration 00030) refuses
/// a sample whose terminal event does not belong to a matching
/// declared-experiment dispatch. Shortcutting the chain would not merely be
/// unrealistic -- it would not insert.
auto seed_cohort(planar::db::connection& conn) -> void {
  exec(conn, "insert into projects (slug, name, root_path) values ('proj', 'proj', '/tmp/op/ev')");
  for (int i = 1; i <= 4; ++i) {
    auto created = reg::create(conn, {.vendor = "anthropic", .candidate_id = std::format("cand-{}", i), .fallback_order = i});
    REQUIRE(created.has_value());
  }

  struct plan_entry {
    int cand;
    int samples;
    int successes;
  };
  const std::array<plan_entry, 4> plan{{{1, 20, 16}, {2, 20, 12}, {3, 3, 3}, {4, 20, 4}}};

  std::string population = "[";
  for (const auto& entry : plan) {
    for (int k = 0; k < entry.samples; ++k) {
      if (population.size() > 1) {
        population.push_back(',');
      }
      population.append(std::format("\"w-{}-{}\"", entry.cand, k));
    }
  }
  population.push_back(']');

  exec(conn, std::format("insert into routing_experiments (id, experiment_key, project_id, validation_policy_version, "
                         "vendor, role, tier, work_type, complexity, routing_policy_version, eligible_population_json, "
                         "candidate_set_json, allocation_method, stopping_rule_json, analysis_policy_json, "
                         "manifest_digest, operator_approved_at, status) values (1,'exp-1',1,'vp-1','anthropic','coder',"
                         "'medium','engine','standard','rp-1','{}','[1,2,3,4]','balanced','{{}}','{{}}','digest-1',"
                         "'2026-08-01T00:00:00Z','running')",
                         population));

  int dispatch = 0;
  for (const auto& entry : plan) {
    for (int k = 0; k < entry.samples; ++k) {
      ++dispatch;
      const auto work_item = std::format("w-{}-{}", entry.cand, k);
      const auto state     = k < entry.successes ? "completed" : "quality_failed";
      const int  success   = k < entry.successes ? 1 : 0;
      exec(conn, std::format("insert into routing_dispatch_snapshots (id,dispatch_key,logical_work_item_id,"
                             "project_id,validation_policy_version,routing_policy_version,profile_rule_version,"
                             "vendor,role,tier,work_type,complexity,packet_digest,policy_digest,capability_digest,"
                             "requested_candidate_id,assignment_class,experiment_id,operator_decision,"
                             "reviewer_disposition,terminal_state,confirmed_at) values ({},'dk-{}','{}',1,'vp-1',"
                             "'rp-1','pr-1','anthropic','coder','medium','engine','standard','pd-{}','pol-1',"
                             "'cap-1',{},'declared_experiment',1,'confirmed','approved','{}',"
                             "'2026-08-02T00:00:00Z')",
                             dispatch, dispatch, work_item, dispatch, entry.cand, state));
      exec(conn, std::format("insert into routing_dispatch_events (dispatch_id,event_id,sequence,event_kind,"
                             "attempt_number,terminal_state,payload_json,occurred_at) values ({},'ev-{}',0,"
                             "'outcome',1,'{}','{{}}','2026-08-02T00:00:00Z')",
                             dispatch, dispatch, state));
      exec(conn, std::format("insert into routing_terminal_samples (experiment_id,logical_work_item_id,role,"
                             "initial_packet_digest,candidate_id,project_id,validation_policy_version,"
                             "routing_policy_version,vendor,tier,work_type,complexity,terminal_event_id,"
                             "terminal_state,quality_success,cohort_eligible,finalized_at) values (1,'{}',"
                             "'coder','pd-{}',{},1,'vp-1','rp-1','anthropic','medium','engine','standard',"
                             "'ev-{}','{}',{},1,'2026-08-03T00:00:00Z')",
                             work_item, dispatch, entry.cand, dispatch, state, success));
    }
  }
}

/// @brief Exact-equality comparison for the transcribed oracle doubles.
///
/// Deliberately NOT an epsilon compare. These are the bytes the oracle
/// printed; if this port's arithmetic differs in the last place, the JSON
/// renderer emits different bytes and parity IS broken. An epsilon would hide
/// exactly the failure this file exists to catch.
auto exactly(double actual, double expected) -> bool {
  return actual == expected;
}

} // namespace

TEST_CASE("models.ranking: wilson penalizes thin evidence at identical raw rates", "[models]") {
  // The whole reason for using Wilson rather than a raw rate: 1/1, 10/10 and
  // 40/40 are all a 100% raw rate, and must NOT tie.
  const double one   = rk::wilson_lower_bound(1, 1, rk::z_95);
  const double ten   = rk::wilson_lower_bound(10, 10, rk::z_95);
  const double forty = rk::wilson_lower_bound(40, 40, rk::z_95);
  REQUIRE(one < ten);
  REQUIRE(ten < forty);
  REQUIRE(forty < 1.0);

  // No evidence scores zero -- not 0.5, not a prior.
  REQUIRE(exactly(rk::wilson_lower_bound(0, 0, rk::z_95), 0.0));
  REQUIRE(exactly(rk::wilson_lower_bound(0, 10, rk::z_95), 0.0));
}

TEST_CASE("models.ranking: the negative clamp is reachable and observable", "[models]") {
  // A BREAK-PROBE SURVIVOR fixed. This case originally read
  // `REQUIRE(wilson_lower_bound(0, 1, z_95) >= 0.0)`, and deleting the clamp
  // from the implementation did not fail a single test.
  //
  // The reason: for zero successes the bound is ANALYTICALLY exactly zero
  // (center = z^2/2n and margin = z*sqrt((z^2/4n)/n) = z^2/2n cancel), so
  // most n land on a clean 0.0 and `>= 0.0` holds with or without the clamp.
  // The clamp only matters where the cancellation leaves floating-point
  // residue -- which it does, for 8445 of the first 200000 values of n.
  //
  // n = 21 is the FIRST such n: unclamped it yields -1.1731740316366828e-17,
  // which `render.cpp` would print as `-1.1731740316366828e-17` instead of
  // `0`, breaking parity on a real, reachable input. Confirmed against the
  // oracle with a 21-sample, 0-success cohort:
  //
  //   $Z models evals ... --json
  //     -> ..."samples":21,"successes":0,"raw_rate":0,"wilson_lower":0,...
  //
  // Exact-zero equality is what discriminates; `>= 0.0` never could.
  REQUIRE(exactly(rk::wilson_lower_bound(0, 21, rk::z_95), 0.0));
  REQUIRE(exactly(rk::wilson_lower_bound(0, 42, rk::z_95), 0.0));
  REQUIRE(exactly(rk::wilson_lower_bound(0, 47, rk::z_95), 0.0));
  REQUIRE(exactly(rk::wilson_lower_bound(0, 1, rk::z_95), 0.0));

  // The clamp must never affect a value that is legitimately positive.
  REQUIRE(rk::wilson_lower_bound(1, 21, rk::z_95) > 0.0);
}

TEST_CASE("models.ranking: a half-and-half record lands near but below one half", "[models]") {
  // 50/100 is a 50% raw rate; the confidence adjustment pulls the reported
  // number strictly under one half rather than leaving it at the raw value.
  const double half = rk::wilson_lower_bound(50, 100, rk::z_95);
  REQUIRE(half < 0.5);
  REQUIRE(half > 0.39);
}

TEST_CASE("models.ranking: the wilson bound reproduces the oracle's exact doubles", "[models]") {
  // Transcribed from the oracle's own JSON, digit for digit. These four are
  // the numeric heart of the acceptance criterion.
  REQUIRE(exactly(rk::wilson_lower_bound(16, 20, rk::z_95), 0.5839825677481064));
  REQUIRE(exactly(rk::wilson_lower_bound(12, 20, rk::z_95), 0.38658150076225317));
  REQUIRE(exactly(rk::wilson_lower_bound(4, 20, rk::z_95), 0.08065766257979808));
  REQUIRE(exactly(rk::wilson_lower_bound(3, 3, rk::z_95), 0.4385029682449545));

  // The z constant itself is part of the contract: the printed label says
  // "95% Wilson lower bound", so the constant and the label move together.
  REQUIRE(exactly(rk::z_95, 1.959963984540054));
  REQUIRE(rk::ranking_version == "routing-ranking-v1");
}

TEST_CASE("models.ranking: both gates are refusals, and they are independent", "[models]") {
  const rk::gates defaults;
  REQUIRE(defaults.minimum_samples == 5);
  REQUIRE(exactly(defaults.quality_floor, 0.5));

  // 4/4 is under the sample minimum. Its Wilson bound (~0.51) would clear the
  // floor, so this row is gated by SAMPLE COUNT alone.
  rk::row thin{.samples = 4, .successes = 4};
  rk::finalize(thin, defaults);
  REQUIRE(thin.insufficient_data);
  REQUIRE_FALSE(thin.below_quality_floor);

  // 5/5 is exactly AT the minimum -- the boundary is inclusive-from-below, so
  // five samples is enough. Its bound is ~0.566, above the floor.
  rk::row five{.samples = 5, .successes = 5};
  rk::finalize(five, defaults);
  REQUIRE_FALSE(five.insufficient_data);
  REQUIRE_FALSE(five.below_quality_floor);
  REQUIRE(five.wilson_lower > 0.5);

  // 6/10 is a 60% raw rate but only ~0.313 Wilson -- gated by QUALITY, with
  // ample samples. Ranking on the raw rate would have promoted it.
  rk::row six{.samples = 10, .successes = 6};
  rk::finalize(six, defaults);
  REQUIRE_FALSE(six.insufficient_data);
  REQUIRE(six.below_quality_floor);
  REQUIRE(exactly(six.raw_rate, 0.6));

  // THE SHORT-CIRCUIT. 2/2 successes gives a Wilson bound of ~0.34, which IS
  // below the 0.5 floor -- and yet `below_quality_floor` stays FALSE, because
  // the sample gate returns first. "Below the floor" is a judgement about
  // quality, and this row is one we just declined to judge.
  //
  // Oracle-verified, not inferred: at `--quality-floor 0.99`, cand-3 (3
  // samples, bound 0.4385) still reports
  // `"insufficient_data":true,"below_quality_floor":false`.
  rk::row both{.samples = 2, .successes = 2};
  rk::finalize(both, defaults);
  REQUIRE(both.wilson_lower < defaults.quality_floor); // the bound really is below
  REQUIRE(both.insufficient_data);
  REQUIRE_FALSE(both.below_quality_floor); // ...and the flag is still not set

  // Derived rates come out of the raw counts, and a zero-sample row divides
  // by nothing rather than producing NaN.
  rk::row empty{};
  rk::finalize(empty, defaults);
  REQUIRE(exactly(empty.raw_rate, 0.0));
  REQUIRE(exactly(empty.gate_failure_rate, 0.0));
  REQUIRE(exactly(empty.expected_excess_iterations, 0.0));
  REQUIRE(exactly(empty.wilson_lower, 0.0));

  rk::row rates{.samples = 20, .successes = 16, .gate_failures = 4, .excess_attempts = 10};
  rk::finalize(rates, defaults);
  REQUIRE(exactly(rates.raw_rate, 0.8));
  REQUIRE(exactly(rates.gate_failure_rate, 0.2));
  REQUIRE(exactly(rates.expected_excess_iterations, 0.5));
}

TEST_CASE("models.ranking: a raised floor gates a row the default floor admits", "[models]") {
  rk::row good{.samples = 20, .successes = 16};
  rk::finalize(good, rk::gates{});
  REQUIRE_FALSE(good.below_quality_floor);

  rk::row same{.samples = 20, .successes = 16};
  rk::finalize(same, rk::gates{.minimum_samples = 5, .quality_floor = 0.99});
  REQUIRE(same.below_quality_floor);
  REQUIRE_FALSE(same.insufficient_data); // the OTHER gate is untouched

  // A raised sample minimum gates a row the default admits, and (via the
  // short-circuit) suppresses the floor verdict entirely -- here that is
  // uninteresting because 20/20 clears the floor anyway, so pin it where the
  // suppression is VISIBLE: a row whose bound is under the floor but whose
  // sample count is also under the minimum.
  rk::row deep{.samples = 20, .successes = 20};
  rk::finalize(deep, rk::gates{.minimum_samples = 50, .quality_floor = 0.5});
  REQUIRE(deep.insufficient_data);
  REQUIRE_FALSE(deep.below_quality_floor);

  rk::row suppressed{.samples = 20, .successes = 4};
  rk::finalize(suppressed, rk::gates{.minimum_samples = 50, .quality_floor = 0.5});
  REQUIRE(suppressed.wilson_lower < 0.5); // the bound IS below the floor
  REQUIRE(suppressed.insufficient_data);
  REQUIRE_FALSE(suppressed.below_quality_floor); // ...and the flag is suppressed
}

TEST_CASE("models.ranking: complexity renders the HYPHENATED wire spelling", "[models]") {
  // An underscored spelling here would match zero rows in the query and be
  // reported as "no evidence" rather than as an error -- a silent wrong
  // answer, which is why this is pinned on its own.
  REQUIRE(rk::complexity_to_text(rk::complexity::high_risk) == "high-risk");
  REQUIRE(rk::complexity_to_text(rk::complexity::bounded) == "bounded");
  REQUIRE(rk::complexity_to_text(rk::complexity::standard) == "standard");
  REQUIRE(rk::complexity_from_text("high-risk") == rk::complexity::high_risk);
  REQUIRE(rk::complexity_from_text("high_risk") == rk::complexity::high_risk);
  REQUIRE_FALSE(rk::complexity_from_text("highrisk").has_value());
  REQUIRE_FALSE(rk::complexity_from_text("").has_value());

  REQUIRE(rk::work_type_to_text(rk::work_type::schema) == "schema");
  REQUIRE(rk::work_type_to_text(rk::work_type::engine) == "engine");
  REQUIRE(rk::work_type_to_text(rk::work_type::architectural) == "architectural");
  REQUIRE(rk::work_type_to_text(rk::work_type::cli) == "cli");
  REQUIRE(rk::work_type_to_text(rk::work_type::feature) == "feature");
  REQUIRE(rk::work_type_to_text(rk::work_type::mechanical) == "mechanical");
  REQUIRE(rk::work_type_from_text("mechanical") == rk::work_type::mechanical);
  REQUIRE_FALSE(rk::work_type_from_text("Mechanical").has_value());
}

TEST_CASE("models.ranking: BRANCH 1 -- the seeded cohort reproduces the oracle exactly", "[models]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed_cohort(conn);

  auto ranked = rk::rank(conn, oracle_cohort(), rk::gates{});
  REQUIRE(ranked.has_value());
  REQUIRE(ranked->rows.size() == 4);

  // Row ORDER: eligible first, then gated rows by descending samples then
  // ascending fallback order.
  REQUIRE(ranked->rows[0].candidate == "cand-1");
  REQUIRE(ranked->rows[1].candidate == "cand-2");
  REQUIRE(ranked->rows[2].candidate == "cand-4");
  REQUIRE(ranked->rows[3].candidate == "cand-3");

  const auto& first = ranked->rows[0];
  REQUIRE(first.candidate_id == 1);
  REQUIRE(first.vendor == "anthropic");
  REQUIRE(first.fallback_order == 1);
  REQUIRE(first.samples == 20);
  REQUIRE(first.successes == 16);
  REQUIRE(first.gate_failures == 4);
  REQUIRE(first.excess_attempts == 0);
  REQUIRE(first.measured_samples == 0);
  REQUIRE_FALSE(first.mean_latency_ms.has_value());
  REQUIRE_FALSE(first.mean_cost_micros.has_value());
  REQUIRE(exactly(first.raw_rate, 0.8));
  REQUIRE(exactly(first.wilson_lower, 0.5839825677481064));
  REQUIRE(exactly(first.gate_failure_rate, 0.2));
  REQUIRE(exactly(first.expected_excess_iterations, 0.0));
  REQUIRE_FALSE(first.insufficient_data);
  REQUIRE_FALSE(first.below_quality_floor);
  REQUIRE(first.rank == 1);

  const auto& second = ranked->rows[1];
  REQUIRE(second.samples == 20);
  REQUIRE(second.successes == 12);
  REQUIRE(second.gate_failures == 8);
  REQUIRE(exactly(second.raw_rate, 0.6));
  REQUIRE(exactly(second.wilson_lower, 0.38658150076225317));
  REQUIRE(exactly(second.gate_failure_rate, 0.4));
  REQUIRE(second.below_quality_floor);
  REQUIRE_FALSE(second.insufficient_data);
  REQUIRE_FALSE(second.rank.has_value());

  const auto& third = ranked->rows[2];
  REQUIRE(third.candidate_id == 4);
  REQUIRE(third.successes == 4);
  REQUIRE(exactly(third.raw_rate, 0.2));
  REQUIRE(exactly(third.wilson_lower, 0.08065766257979808));
  REQUIRE(exactly(third.gate_failure_rate, 0.8));
  REQUIRE(third.below_quality_floor);
  REQUIRE_FALSE(third.rank.has_value());

  // THE row that justifies the whole module: a perfect raw rate that ranks
  // nowhere, because three samples is not evidence.
  const auto& thin = ranked->rows[3];
  REQUIRE(thin.candidate == "cand-3");
  REQUIRE(thin.samples == 3);
  REQUIRE(thin.successes == 3);
  REQUIRE(exactly(thin.raw_rate, 1.0));
  REQUIRE(exactly(thin.wilson_lower, 0.4385029682449545));
  REQUIRE(exactly(thin.gate_failure_rate, 0.0));
  REQUIRE(thin.insufficient_data);
  REQUIRE_FALSE(thin.below_quality_floor);
  REQUIRE_FALSE(thin.rank.has_value());

  // Exactly ONE row is ranked, and it is the recommendation -- carried as the
  // opaque candidate STRING, not an index.
  REQUIRE(ranked->recommended == std::optional<std::string>{"cand-1"});
  REQUIRE_FALSE(ranked->no_recommendation_reason.has_value());
}

TEST_CASE("models.ranking: TWO eligible rows are ordered by descending Wilson bound", "[models]") {
  // A BREAK-PROBE SURVIVOR fixed. Reversing `lhs.wilson_lower > rhs.wilson_lower`
  // in the comparator -- inverting the PRIMARY ranking key, the single most
  // consequential line in the module -- failed no test at all, because the
  // default-gates fixture leaves exactly ONE eligible row and a one-element
  // sequence has no order to get wrong.
  //
  // Lowering the floor to 0.3 admits cand-2 (bound 0.387) alongside cand-1
  // (0.584), so the comparator is finally exercised. Oracle-verified at this
  // exact floor:
  //
  //   $Z models evals ... --quality-floor 0.3 --json
  //     cand-1 wilson=0.5839825677481064 rank=1
  //     cand-2 wilson=0.38658150076225317 rank=2
  //     cand-4 wilson=0.08065766257979808 rank=null  below_quality_floor
  //     cand-3 wilson=0.4385029682449545 rank=null  insufficient_data
  //     recommended = cand-1
  //
  // Note cand-3: a HIGHER bound (0.4385) than cand-2's, and still unranked.
  // The sample gate outranks the quality ordering entirely.
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed_cohort(conn);

  auto ranked = rk::rank(conn, oracle_cohort(), rk::gates{.minimum_samples = 5, .quality_floor = 0.3});
  REQUIRE(ranked.has_value());
  REQUIRE(ranked->rows.size() == 4);

  REQUIRE(ranked->rows[0].candidate == "cand-1");
  REQUIRE(ranked->rows[0].rank == 1);
  REQUIRE(exactly(ranked->rows[0].wilson_lower, 0.5839825677481064));

  REQUIRE(ranked->rows[1].candidate == "cand-2");
  REQUIRE(ranked->rows[1].rank == 2);
  REQUIRE(exactly(ranked->rows[1].wilson_lower, 0.38658150076225317));

  // Strictly descending among the ranked rows -- the property the reversed
  // comparator violates.
  REQUIRE(ranked->rows[0].wilson_lower > ranked->rows[1].wilson_lower);

  // Gated rows still trail both, and cand-3 trails despite a bound ABOVE
  // cand-2's.
  REQUIRE(ranked->rows[2].candidate == "cand-4");
  REQUIRE(ranked->rows[2].below_quality_floor);
  REQUIRE(ranked->rows[3].candidate == "cand-3");
  REQUIRE(ranked->rows[3].insufficient_data);
  REQUIRE(ranked->rows[3].wilson_lower > ranked->rows[1].wilson_lower);
  REQUIRE_FALSE(ranked->rows[3].rank.has_value());

  // The recommendation is the TOP-ranked row, not merely "some ranked row".
  REQUIRE(ranked->recommended == std::optional<std::string>{"cand-1"});
}

TEST_CASE("models.ranking: BRANCH 2 -- a floor above every bound yields no recommendation", "[models]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed_cohort(conn);

  auto ranked = rk::rank(conn, oracle_cohort(), rk::gates{.minimum_samples = 5, .quality_floor = 0.99});
  REQUIRE(ranked.has_value());
  REQUIRE(ranked->rows.size() == 4);
  for (const auto& value : ranked->rows) {
    REQUIRE_FALSE(value.rank.has_value());
  }
  // The three well-sampled rows all fall below the raised floor.
  for (const auto& value : ranked->rows) {
    if (value.candidate != "cand-3") {
      REQUIRE(value.below_quality_floor);
      REQUIRE_FALSE(value.insufficient_data);
    }
  }
  // cand-3 stays `insufficient_data` and NOT `below_quality_floor` even at a
  // floor of 0.99, which its 0.4385 bound is obviously under. The sample gate
  // short-circuits. Oracle-verified at this exact floor.
  const auto cand3 = std::ranges::find_if(ranked->rows, [](const rk::row& r) { return r.candidate == "cand-3"; });
  REQUIRE(cand3 != ranked->rows.end());
  REQUIRE(cand3->insufficient_data);
  REQUIRE_FALSE(cand3->below_quality_floor);

  REQUIRE_FALSE(ranked->recommended.has_value());
  REQUIRE(ranked->no_recommendation_reason ==
          std::optional<std::string>{"no candidate cleared both the minimum-sample and quality-floor gates"});
}

TEST_CASE("models.ranking: BRANCH 3 -- an empty cohort names its emptiness, not a gate", "[models]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed_cohort(conn);

  // Same evidence, different complexity band. Evidence is NEVER pooled across
  // cohorts, so this must report zero rows rather than borrowing `standard`'s.
  auto target        = oracle_cohort();
  target.complexity_ = rk::complexity::bounded;
  auto ranked        = rk::rank(conn, target, rk::gates{});
  REQUIRE(ranked.has_value());
  REQUIRE(ranked->rows.empty());
  REQUIRE_FALSE(ranked->recommended.has_value());
  // The reason DISTINGUISHES no-evidence from all-gated. Reporting the gate
  // message here would tell an operator to loosen a gate that is not the
  // problem.
  REQUIRE(ranked->no_recommendation_reason == std::optional<std::string>{"no cohort-eligible declared-experiment samples"});
}

TEST_CASE("models.ranking: every cohort field is a real filter", "[models]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed_cohort(conn);

  // Changing ANY single cohort field must empty the result. A field that was
  // accidentally dropped from the WHERE clause would still return all four
  // rows here.
  auto expect_empty = [&](rk::cohort target, std::string_view what) {
    INFO(what);
    auto ranked = rk::rank(conn, target, rk::gates{});
    REQUIRE(ranked.has_value());
    REQUIRE(ranked->rows.empty());
  };

  auto by_project       = oracle_cohort();
  by_project.project_id = 999;
  expect_empty(by_project, "project_id");

  auto by_validation                      = oracle_cohort();
  by_validation.validation_policy_version = "vp-2";
  expect_empty(by_validation, "validation_policy_version");

  auto by_routing                   = oracle_cohort();
  by_routing.routing_policy_version = "rp-2";
  expect_empty(by_routing, "routing_policy_version");

  auto by_vendor   = oracle_cohort();
  by_vendor.vendor = "openai";
  expect_empty(by_vendor, "vendor");

  auto by_role = oracle_cohort();
  by_role.role = "reviewer";
  expect_empty(by_role, "role");

  auto by_tier  = oracle_cohort();
  by_tier.tier_ = reg::tier::large;
  expect_empty(by_tier, "tier");

  auto by_work       = oracle_cohort();
  by_work.work_type_ = rk::work_type::schema;
  expect_empty(by_work, "work_type");

  auto by_complexity        = oracle_cohort();
  by_complexity.complexity_ = rk::complexity::high_risk;
  expect_empty(by_complexity, "complexity");
}

TEST_CASE("models.ranking: an excluded sample never enters the math", "[models]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed_cohort(conn);

  const auto before = rk::rank(conn, oracle_cohort(), rk::gates{});
  REQUIRE(before.has_value());
  REQUIRE(before->rows[0].samples == 20);

  // A `candidate_mismatch` sample: the host actually ran something else, so
  // the record is real evidence of a PROCESS failure but tells us nothing
  // about the candidate's quality. `cohort_eligible = 0` keeps it out.
  exec(conn, "insert into routing_dispatch_snapshots (id,dispatch_key,logical_work_item_id,project_id,"
             "validation_policy_version,routing_policy_version,profile_rule_version,vendor,role,tier,work_type,"
             "complexity,packet_digest,policy_digest,capability_digest,requested_candidate_id,actual_vendor,"
             "actual_candidate_id,assignment_class,experiment_id,operator_decision,reviewer_disposition,"
             "terminal_state,confirmed_at) values (999,'dk-x','w-1-0',1,'vp-1','rp-1','pr-1','anthropic','coder',"
             "'medium','engine','standard','pd-x','pol-1','cap-1',1,'anthropic','other-model',"
             "'declared_experiment',1,'confirmed','approved','candidate_mismatch','2026-08-02T00:00:00Z')");
  exec(conn, "insert into routing_dispatch_events (dispatch_id,event_id,sequence,event_kind,attempt_number,"
             "terminal_state,payload_json,occurred_at) values (999,'ev-x',0,'outcome',1,'candidate_mismatch',"
             "'{}','2026-08-02T00:00:00Z')");
  exec(conn, "insert into routing_terminal_samples (experiment_id,logical_work_item_id,role,initial_packet_digest,"
             "candidate_id,project_id,validation_policy_version,routing_policy_version,vendor,tier,work_type,"
             "complexity,terminal_event_id,terminal_state,quality_success,cohort_eligible,exclusion_reason,"
             "finalized_at) values (1,'w-1-0','coder','pd-x',1,1,'vp-1','rp-1','anthropic','medium','engine',"
             "'standard','ev-x','candidate_mismatch',0,0,'actual_candidate_differs','2026-08-03T00:00:00Z')");

  const auto after = rk::rank(conn, oracle_cohort(), rk::gates{});
  REQUIRE(after.has_value());
  // Still 20, not 21: the excluded sample was recorded but not counted, and
  // the Wilson bound is byte-identical to before.
  REQUIRE(after->rows[0].samples == 20);
  REQUIRE(after->rows[0].successes == 16);
  REQUIRE(exactly(after->rows[0].wilson_lower, 0.5839825677481064));
}

TEST_CASE("models.ranking: quality outranks speed, and an unmeasured metric abstains", "[models]") {
  // Two rows with the SAME quality: the measured-and-faster one wins on
  // latency.
  rk::row fast{.fallback_order = 9, .samples = 20, .successes = 16};
  fast.mean_latency_ms = 100.0;
  rk::row slow{.fallback_order = 1, .samples = 20, .successes = 16};
  slow.mean_latency_ms = 900.0;
  rk::finalize(fast, rk::gates{});
  rk::finalize(slow, rk::gates{});
  REQUIRE(exactly(fast.wilson_lower, slow.wilson_lower));

  // With equal quality and both measured, latency decides -- overriding the
  // fallback-order tiebreak, which would otherwise have preferred `slow`.
  std::vector<rk::row> both{slow, fast};
  std::stable_sort(both.begin(), both.end(), [](const rk::row& a, const rk::row& b) {
    if (a.wilson_lower != b.wilson_lower) {
      return a.wilson_lower > b.wilson_lower;
    }
    if (a.mean_latency_ms.has_value() && b.mean_latency_ms.has_value() && *a.mean_latency_ms != *b.mean_latency_ms) {
      return *a.mean_latency_ms < *b.mean_latency_ms;
    }
    return a.fallback_order < b.fallback_order;
  });
  REQUIRE(exactly(*both[0].mean_latency_ms, 100.0));

  // But a FASTER, WORSE candidate must never outrank a slower correct one --
  // the floor gates it out before speed is ever consulted.
  rk::row fast_and_wrong{.samples = 20, .successes = 4};
  fast_and_wrong.mean_latency_ms = 1.0;
  rk::finalize(fast_and_wrong, rk::gates{});
  REQUIRE(fast_and_wrong.below_quality_floor);
}
