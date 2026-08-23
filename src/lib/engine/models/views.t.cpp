// @file views.t.cpp
// @brief Unit tests for `planar.engine.models.views` (plan 996, task 6096).
//
// ORACLE PROVENANCE. Captured by RUNNING the Zig binary against the same
// seeded four-candidate cohort ranking.t.cpp describes, with
// HOME/PLANAR_HOME/CODEX_HOME redirected under /tmp:
//
//   $Z models experiments --json
//   {"views_version":"routing-views-v1","experiments":[
//     {"id":1,"experiment_key":"exp-1","status":"running","vendor":"anthropic",
//      "role":"coder","tier":"medium","work_type":"engine",
//      "complexity":"standard","validation_policy_version":"vp-1",
//      "routing_policy_version":"rp-1","manifest_digest":"digest-1",
//      "operator_approved_at":"2026-08-01T00:00:00Z","population_size":63,
//      "candidate_count":4,"samples":63,"eligible_samples":63}]}
//
//   ... after adding one candidate_mismatch sample excluded as
//       'actual_candidate_differs':
//   $Z models experiments
//       samples: 64 recorded, 63 counted (1 excluded)
//
//   $Z models outcomes --limit 2 --json
//   {"views_version":"routing-views-v1","outcomes":[
//     {"id":64,...,"candidate":"cand-1","terminal_state":"candidate_mismatch",
//      "quality_success":false,"counts_toward_recommendation":false,
//      "exclusion_reason":"actual_candidate_differs",...},
//     {"id":63,...,"candidate":"cand-4","terminal_state":"quality_failed",
//      "quality_success":false,"counts_toward_recommendation":true,
//      "exclusion_reason":null,...}]}
//
// Note the SECOND row: `quality_failed` still COUNTS toward recommendations.
// A failure is evidence; suppressing it would bias every rate upward. Only an
// evidence-BOUNDARY problem (the wrong model actually ran) excludes.
//
// --- ordering -------------------------------------------------------------
//   experiments: id ASC.  outcomes: id DESC (newest first).
//   `$Z models outcomes` with no --limit returned ids 63 down to 14 -- fifty
//   rows, confirming the CLI's default limit of 50.
//
// --- limit refusals (handled at the CLI layer, above this module) ---------
//   $Z models outcomes --limit 0    -> exit 2, "error: --limit must be positive"
//   $Z models outcomes --limit abc  -> exit 2,
//        "error: invalid --limit 'abc': expected integer"
//
// --- EMPTY DATABASE (hazard 6) --------------------------------------------
//   $Z models experiments --json -> {"views_version":"routing-views-v1","experiments":[]}
//   $Z models outcomes --json    -> {"views_version":"routing-views-v1","outcomes":[]}

#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.db.migrate;
import planar.engine.models.views;

namespace {

namespace vw = planar::engine::models::views;

struct scratch_db_path {
  std::filesystem::path path_;

  scratch_db_path()
      : path_(std::filesystem::temp_directory_path() / std::format("planar_models_views_test_{}_{}.db",
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

/// @brief Declare one experiment with an explicit population and candidate set.
auto declare_experiment(planar::db::connection& conn, int id, std::string_view key, std::string_view status,
                        std::string_view population, std::string_view candidates) -> void {
  exec(conn, std::format("insert into routing_experiments (id, experiment_key, project_id, "
                         "validation_policy_version, vendor, role, tier, work_type, complexity, "
                         "routing_policy_version, eligible_population_json, candidate_set_json, "
                         "allocation_method, stopping_rule_json, analysis_policy_json, manifest_digest, "
                         "operator_approved_at, status) values ({},'{}',1,'vp-1','anthropic','coder','medium',"
                         "'engine','standard','rp-1','{}','{}','balanced','{{}}','{{}}','digest-{}',"
                         "'2026-08-01T00:00:00Z','{}')",
                         id, key, population, candidates, id, status));
}

/// @brief Write one complete dispatch -> event -> terminal-sample chain.
///
/// `routing_terminal_samples_identity` refuses a sample whose terminal event
/// does not belong to a matching declared-experiment dispatch, so the chain
/// must be complete.
auto record_sample(planar::db::connection& conn, int dispatch, std::string_view work_item, int candidate, std::string_view state,
                   int quality_success, bool eligible, std::string_view exclusion) -> void {
  const std::string actual = eligible ? std::string{"null,null"} : std::string{"'anthropic','other-model'"};
  exec(conn, std::format("insert into routing_dispatch_snapshots (id,dispatch_key,logical_work_item_id,project_id,"
                         "validation_policy_version,routing_policy_version,profile_rule_version,vendor,role,tier,"
                         "work_type,complexity,packet_digest,policy_digest,capability_digest,"
                         "requested_candidate_id,actual_vendor,actual_candidate_id,assignment_class,experiment_id,"
                         "operator_decision,reviewer_disposition,terminal_state,confirmed_at) values ({},'dk-{}',"
                         "'{}',1,'vp-1','rp-1','pr-1','anthropic','coder','medium','engine','standard','pd-{}',"
                         "'pol-1','cap-1',{},{},'declared_experiment',1,'confirmed','approved','{}',"
                         "'2026-08-02T00:00:00Z')",
                         dispatch, dispatch, work_item, dispatch, candidate, actual, state));
  exec(conn, std::format("insert into routing_dispatch_events (dispatch_id,event_id,sequence,event_kind,"
                         "attempt_number,terminal_state,payload_json,occurred_at) values ({},'ev-{}',0,'outcome',"
                         "1,'{}','{{}}','2026-08-02T00:00:00Z')",
                         dispatch, dispatch, state));
  const std::string reason = exclusion.empty() ? std::string{"null"} : std::format("'{}'", exclusion);
  exec(conn, std::format("insert into routing_terminal_samples (experiment_id,logical_work_item_id,role,"
                         "initial_packet_digest,candidate_id,project_id,validation_policy_version,"
                         "routing_policy_version,vendor,tier,work_type,complexity,terminal_event_id,"
                         "terminal_state,quality_success,cohort_eligible,exclusion_reason,finalized_at) values "
                         "(1,'{}','coder','pd-{}',{},1,'vp-1','rp-1','anthropic','medium','engine','standard',"
                         "'ev-{}','{}',{},{},{},'2026-08-03T00:00:00Z')",
                         work_item, dispatch, candidate, dispatch, state, quality_success, eligible ? 1 : 0, reason));
}

auto seed_project_and_candidates(planar::db::connection& conn) -> void {
  exec(conn, "insert into projects (slug, name, root_path) values ('proj', 'proj', '/tmp/op/ev')");
  for (int i = 1; i <= 4; ++i) {
    exec(conn, std::format("insert into routing_candidates (vendor, candidate_id, enabled, fallback_order, "
                           "compatibility_source) values ('anthropic','cand-{}',1,{},'native')",
                           i, i));
  }
}

} // namespace

TEST_CASE("models.views: the views version is part of the wire contract", "[models]") {
  REQUIRE(vw::views_version == "routing-views-v1");
}

TEST_CASE("models.views: an empty database yields empty vectors, not errors", "[models]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto experiments = vw::list_experiments(conn);
  REQUIRE(experiments.has_value());
  REQUIRE(experiments->empty());

  auto outcomes = vw::list_outcomes(conn, 50);
  REQUIRE(outcomes.has_value());
  REQUIRE(outcomes->empty());
}

TEST_CASE("models.views: experiments report frozen manifest counts, oldest first", "[models]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed_project_and_candidates(conn);

  declare_experiment(conn, 1, "exp-1", "running", R"(["w-1","w-2","w-3"])", "[1,2,3,4]");
  declare_experiment(conn, 2, "exp-2", "declared", R"(["a"])", "[1]");

  auto experiments = vw::list_experiments(conn);
  REQUIRE(experiments.has_value());
  REQUIRE(experiments->size() == 2);

  // id ASC -- declaration order, which is the order an operator declared them.
  REQUIRE((*experiments)[0].experiment_key == "exp-1");
  REQUIRE((*experiments)[1].experiment_key == "exp-2");

  const auto& first = (*experiments)[0];
  REQUIRE(first.id == 1);
  REQUIRE(first.status == "running");
  REQUIRE(first.vendor == "anthropic");
  REQUIRE(first.role == "coder");
  REQUIRE(first.tier == "medium");
  REQUIRE(first.work_type == "engine");
  REQUIRE(first.complexity == "standard");
  REQUIRE(first.validation_policy_version == "vp-1");
  REQUIRE(first.routing_policy_version == "rp-1");
  REQUIRE(first.manifest_digest == "digest-1");
  REQUIRE(first.operator_approved_at == "2026-08-01T00:00:00Z");
  // Counted out of the FROZEN manifests, not re-derived from live eligibility.
  REQUIRE(first.population_size == 3);
  REQUIRE(first.candidate_count == 4);
  REQUIRE(first.samples == 0);
  REQUIRE(first.eligible_samples == 0);

  REQUIRE((*experiments)[1].population_size == 1);
  REQUIRE((*experiments)[1].candidate_count == 1);
  REQUIRE((*experiments)[1].status == "declared");
}

TEST_CASE("models.views: recorded and counted sample totals are reported separately", "[models]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed_project_and_candidates(conn);
  declare_experiment(conn, 1, "exp-1", "running", R"(["w-1","w-2","w-3"])", "[1,2,3,4]");

  record_sample(conn, 1, "w-1", 1, "completed", 1, true, "");
  record_sample(conn, 2, "w-2", 1, "quality_failed", 0, true, "");
  record_sample(conn, 3, "w-3", 1, "candidate_mismatch", 0, false, "actual_candidate_differs");

  auto experiments = vw::list_experiments(conn);
  REQUIRE(experiments.has_value());
  // 3 recorded, 2 counted -- the difference IS the excluded count, and it is
  // visible rather than folded into one total.
  REQUIRE((*experiments)[0].samples == 3);
  REQUIRE((*experiments)[0].eligible_samples == 2);
}

TEST_CASE("models.views: outcomes come back newest first and honour the limit", "[models]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed_project_and_candidates(conn);
  declare_experiment(conn, 1, "exp-1", "running", R"(["w-1","w-2","w-3","w-4"])", "[1,2,3,4]");

  record_sample(conn, 1, "w-1", 1, "completed", 1, true, "");
  record_sample(conn, 2, "w-2", 2, "completed", 1, true, "");
  record_sample(conn, 3, "w-3", 3, "quality_failed", 0, true, "");
  record_sample(conn, 4, "w-4", 4, "candidate_mismatch", 0, false, "actual_candidate_differs");

  auto all = vw::list_outcomes(conn, 50);
  REQUIRE(all.has_value());
  REQUIRE(all->size() == 4);
  // id DESC: the newest sample is first.
  REQUIRE((*all)[0].id == 4);
  REQUIRE((*all)[3].id == 1);

  // The limit truncates from the NEWEST end, not the oldest.
  auto two = vw::list_outcomes(conn, 2);
  REQUIRE(two.has_value());
  REQUIRE(two->size() == 2);
  REQUIRE((*two)[0].id == 4);
  REQUIRE((*two)[1].id == 3);

  auto one = vw::list_outcomes(conn, 1);
  REQUIRE(one.has_value());
  REQUIRE(one->size() == 1);
  REQUIRE((*one)[0].id == 4);
}

TEST_CASE("models.views: a quality failure COUNTS; only a boundary problem excludes", "[models]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed_project_and_candidates(conn);
  declare_experiment(conn, 1, "exp-1", "running", R"(["w-1","w-2","w-3"])", "[1,2,3,4]");

  record_sample(conn, 1, "w-1", 1, "completed", 1, true, "");
  record_sample(conn, 2, "w-2", 4, "quality_failed", 0, true, "");
  record_sample(conn, 3, "w-3", 1, "candidate_mismatch", 0, false, "actual_candidate_differs");

  auto outcomes = vw::list_outcomes(conn, 50);
  REQUIRE(outcomes.has_value());
  REQUIRE(outcomes->size() == 3);

  const auto& excluded = (*outcomes)[0];
  REQUIRE(excluded.id == 3);
  REQUIRE(excluded.terminal_state == "candidate_mismatch");
  REQUIRE_FALSE(excluded.quality_success);
  REQUIRE_FALSE(excluded.counts_toward_recommendation);
  // An exclusion is ALWAYS named. An unexplained exclusion is
  // indistinguishable from a bug.
  REQUIRE(excluded.exclusion_reason == std::optional<std::string>{"actual_candidate_differs"});
  REQUIRE(excluded.candidate == "cand-1");
  REQUIRE(excluded.logical_work_item_id == "w-3");
  REQUIRE(excluded.role == "coder");
  REQUIRE(excluded.vendor == "anthropic");
  REQUIRE(excluded.tier == "medium");
  REQUIRE(excluded.work_type == "engine");
  REQUIRE(excluded.complexity == "standard");
  REQUIRE(excluded.experiment_id == 1);
  REQUIRE(excluded.finalized_at == "2026-08-03T00:00:00Z");

  // A FAILED run still counts. This is the row a naive "only show successes"
  // implementation would drop, biasing every rate upward.
  const auto& failed = (*outcomes)[1];
  REQUIRE(failed.terminal_state == "quality_failed");
  REQUIRE_FALSE(failed.quality_success);
  REQUIRE(failed.counts_toward_recommendation);
  REQUIRE_FALSE(failed.exclusion_reason.has_value()); // NOT an empty string
  REQUIRE(failed.candidate == "cand-4");

  const auto& passed = (*outcomes)[2];
  REQUIRE(passed.terminal_state == "completed");
  REQUIRE(passed.quality_success);
  REQUIRE(passed.counts_toward_recommendation);
  REQUIRE_FALSE(passed.exclusion_reason.has_value());
}

TEST_CASE("models.views: the candidate column resolves the OPAQUE id, not the row id", "[models]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed_project_and_candidates(conn);
  declare_experiment(conn, 1, "exp-1", "running", R"(["w-1"])", "[1,2,3,4]");
  record_sample(conn, 1, "w-1", 3, "completed", 1, true, "");

  auto outcomes = vw::list_outcomes(conn, 50);
  REQUIRE(outcomes.has_value());
  // The join reaches routing_candidates.candidate_id -- an operator reading
  // this needs the model identity, not an internal key.
  REQUIRE((*outcomes)[0].candidate == "cand-3");
}
