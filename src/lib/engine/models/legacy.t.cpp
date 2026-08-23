// @file legacy.t.cpp
// @brief Tests for `planar.engine.models.legacy` (plan 996, task 6111).
//
// ============================================================================
// HOME SAFETY
// ============================================================================
// Every database here is a uniquely-named scratch file under
// `temp_directory_path()`, removed on destruction. This module reads no
// environment variable and touches no path of its own — unlike the Zig
// handler's legacy path, which also resolves and reads `$PLANAR_CONFIG_PATH`
// (see "the config read that feeds nothing" below); that read is deliberately
// NOT reproduced in this layer-2 module.
//
// ============================================================================
// ORACLE PROVENANCE
// ============================================================================
// Probes ran with PLANAR_DB / PLANAR_CONFIG_PATH / PLANAR_HOME /
// PLANAR_LOCAL_HOME / HOME all redirected under /tmp, against a scratch
// database created by `planar init`. Captured with
// `python3 -c "print(repr(open(f,'rb').read()))"` on separated stdout/stderr
// files — never through a pipe.
//
// --- branch selection ------------------------------------------------------
//   $Z models evals --role coder --tier medium --json
//     -> exit 0, the LEGACY envelope. Both flags are silently IGNORED: only
//        `--vendor` selects the cohort branch.
//   $Z models evals --vendor claude --json
//     -> exit 2, stderr `error: --project is required when ranking a cohort\n`
//
// --- empty database, hazard 6 ----------------------------------------------
//   $Z models evals --json   exit 0:
//     {"scorecard":[],"recommendations":[],"signals_sourced":{
//      "reviewer_disposition":true,"iteration_count":true,
//      "quality_gate_pass_fail":false,"test_coder_expansion":true},
//      "legacy_dispatch_notes_skipped":0}
//
//   $Z models evals          exit 0:
//     routing evals scorecard (per work-type, candidate) — read-only, writes nothing:
//       (no completed dispatch history recorded yet)
//     <blank>
//     recommendations (preview only — writes nothing; apply is a separate operator-gated step):
//       (none — no work type has scored dispatch history yet)
//     <blank>
//     signals sourced: reviewer_disposition=yes iteration_count=yes quality_gate_pass_fail=no test_coder_expansion=yes
//
//   `quality_gate_pass_fail` is FALSE while the other three are true, on an
//   EMPTY database. It is a capability statement, never derived from data.
//
// --- populated fixture -----------------------------------------------------
// Five notes seeded into session_entries (task 1 named twice; task 9 named
// with an incomplete triple; one note with no model_choice line at all),
// three claims (completed / aborted / completed) and two test-coder actions
// (ok / error). Verbatim:
//
//   {"scorecard":[
//     {"work_type":"feature","candidate":"claude-sonnet-5","vendor":"claude",
//      "tier":"medium","dispatch_count":2,"approved_count":1,"aborted_count":1,
//      "other_count":0,"approval_rate":0.5,"avg_iterations":1.5,
//      "test_coder_ok_count":1,"test_coder_other_count":1,
//      "insufficient_data":false,"rank":1},
//     {"work_type":"schema","candidate":"claude-opus-5","vendor":"claude",
//      "tier":"large","dispatch_count":1,"approved_count":1,"aborted_count":0,
//      "other_count":0,"approval_rate":1,"avg_iterations":1,
//      "test_coder_ok_count":0,"test_coder_other_count":0,
//      "insufficient_data":false,"rank":1}],
//    "recommendations":[
//     {"work_type":"feature","vendor":"claude","tier":"medium",
//      "candidate":"claude-sonnet-5","rationale":"1/2 approved, avg 1.50
//      iterations to approval (highest-ranked scored candidate for feature)"},
//     {"work_type":"schema",...,"rationale":"1/1 approved, avg 1.00 ..."}],
//    "signals_sourced":{...},"legacy_dispatch_notes_skipped":1}
//
//   TWO float-rendering facts fall out of that and are asserted separately:
//   `approval_rate` 1.0 serialises as `1` (not `1.0`), while the RATIONALE for
//   the same row says `1.00`. The JSON number and the prose beside it disagree
//   on format, on purpose.
//
//   Text form of the same fixture:
//     routing evals scorecard (per work-type, candidate) — read-only, writes nothing:
//       feature        claude-sonnet-5          rank 1  1/2 approved  avg 1.50 iter  [claude/medium]
//       schema         claude-opus-5            rank 1  1/1 approved  avg 1.00 iter  [claude/large]
//     <blank>
//     recommendations (preview only — writes nothing; apply is a separate operator-gated step):
//       feature → claude-sonnet-5 [claude/medium]: 1/2 approved, avg 1.50 iterations ...
//       schema → claude-opus-5 [claude/large]: 1/1 approved, avg 1.00 iterations ...
//     <blank>
//     signals sourced: ...
//     note: 1 dispatch note(s) skipped — missing/malformed model_choice work_type
//
// --- the config read that feeds nothing ------------------------------------
// Before aggregating, the Zig handler resolves `$PLANAR_CONFIG_PATH`, reads it,
// calls `engine.config.resolve`, binds the result, and NEVER REFERENCES IT
// AGAIN. It is dead work with a live failure mode — a malformed config file
// makes `models evals` exit 1 with `error: resolving configuration: ParseFailed`
// even though nothing on this path consumes configuration.
//
// This port does NOT reproduce that read here, and the omission is deliberate
// rather than an oversight. Reproducing it would require `engine_models` to
// depend on `engine_config`, which is a layer-2-to-layer-2 edge that
// cmake/architecture.cmake FATALs on (D18) — and the dependency would exist
// solely to reproduce three error messages from a value that is discarded. The
// config read is genuinely part of the command's argument handling, not of the
// aggregation, so it belongs at the `cmd_*` layer where config already lives.
// Named here so the cycle that lands `cmd_planar` does not have to rediscover
// it.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.db.migrate;
import planar.engine.models.legacy;

namespace lg = planar::engine::models::legacy;

namespace {

/// @brief A per-test scratch database file, removed on destruction.
struct scratch_db_path {
  std::filesystem::path path_;

  scratch_db_path()
      : path_(std::filesystem::temp_directory_path() / std::format("planar_models_legacy_test_{}_{}.db",
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

auto exec(planar::db::connection& conn, std::string_view sql) -> void {
  REQUIRE(conn.execute(sql).has_value());
}

/// @brief Seed one session so the foreign keys on the three tables resolve.
auto seed_session(planar::db::connection& conn) -> void {
  exec(conn, "insert into sessions (id, vendor) values (1, 'claude')");
}

auto add_note(planar::db::connection& conn, int ordinal, std::string_view body) -> void {
  auto stmt = conn.prepare("insert into session_entries (session_id, ordinal, prefix, body) "
                           "values (1, ?, 'note', ?)");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->bind_int64(1, ordinal).has_value());
  REQUIRE(stmt->bind_text(2, body).has_value());
  REQUIRE(stmt->step().has_value());
}

auto add_claim(planar::db::connection& conn, std::string_view token, std::int64_t task_id, std::string_view status,
               std::string_view model, std::string_view claimed_at) -> void {
  auto stmt = conn.prepare("insert into agent_work_claims (claim_token, session_id, entity_kind, entity_id, status, vendor, "
                           "model, lease_expires_at, claimed_at) values (?, 1, 'task', ?, ?, 'claude', ?, "
                           "'2030-01-01T00:00:00Z', ?)");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->bind_text(1, token).has_value());
  REQUIRE(stmt->bind_int64(2, task_id).has_value());
  REQUIRE(stmt->bind_text(3, status).has_value());
  REQUIRE(stmt->bind_text(4, model).has_value());
  REQUIRE(stmt->bind_text(5, claimed_at).has_value());
  REQUIRE(stmt->step().has_value());
}

auto add_test_coder(planar::db::connection& conn, std::int64_t task_id, std::string_view outcome) -> void {
  auto stmt = conn.prepare("insert into agent_actions (session_id, action_kind, entity_kind, entity_id, "
                           "vendor, outcome) values (1, 'test_coder', 'task', ?, 'claude', ?)");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->bind_int64(1, task_id).has_value());
  REQUIRE(stmt->bind_text(2, outcome).has_value());
  REQUIRE(stmt->step().has_value());
}

/// @brief The exact fixture the oracle captures above were taken against.
auto seed_oracle_fixture(planar::db::connection& conn) -> void {
  seed_session(conn);
  add_note(conn, 1,
           "dispatch_shape: fan-out\n"
           "model_choice: {\"1\":{\"tier\":\"medium\",\"candidate\":\"claude-sonnet-5\","
           "\"work_type\":\"feature\"}}");
  add_note(conn, 2,
           "dispatch_shape: fan-out\n"
           "model_choice: {\"1\":{\"tier\":\"medium\",\"candidate\":\"claude-sonnet-5\","
           "\"work_type\":\"feature\"},\"2\":{\"tier\":\"medium\",\"candidate\":\"claude-sonnet-5\","
           "\"work_type\":\"feature\"}}");
  add_note(conn, 3,
           "dispatch_shape: single\n"
           "model_choice: {\"3\":{\"tier\":\"large\",\"candidate\":\"claude-opus-5\","
           "\"work_type\":\"schema\"}}");
  add_note(conn, 4, "dispatch_shape: legacy note with no model_choice line");
  add_note(conn, 5,
           "dispatch_shape: partial\n"
           "model_choice: {\"9\":{\"tier\":\"medium\",\"candidate\":\"c\"}}");

  add_claim(conn, "t1", 1, "completed", "claude-sonnet-5", "2026-01-01T00:00:01Z");
  add_claim(conn, "t2", 2, "aborted", "claude-sonnet-5", "2026-01-01T00:00:02Z");
  add_claim(conn, "t3", 3, "completed", "claude-opus-5", "2026-01-01T00:00:03Z");
  add_test_coder(conn, 1, "ok");
  add_test_coder(conn, 2, "error");
}

} // namespace

// ===========================================================================
// process_dispatch_body — the forgiving parse, rule by rule
// ===========================================================================

TEST_CASE("legacy process_dispatch_body reads a complete triple") {
  std::map<std::int64_t, lg::task_info> tasks;
  lg::process_dispatch_body("dispatch_shape: single\n"
                            "model_choice: {\"7\":{\"tier\":\"large\",\"candidate\":\"c-1\","
                            "\"work_type\":\"schema\"}}",
                            tasks);
  REQUIRE(tasks.size() == 1);
  REQUIRE(tasks[7].work_type == "schema");
  REQUIRE(tasks[7].candidate == "c-1");
  REQUIRE(tasks[7].tier == "large");
  REQUIRE(tasks[7].iterations == 1);
  REQUIRE(tasks[7].has_info);
}

TEST_CASE("legacy process_dispatch_body counts an iteration even for an INCOMPLETE triple") {
  // The dispatch happened; only its classification is unusable. This is what
  // makes a task named twice — once completely, once not — report avg 1.5.
  std::map<std::int64_t, lg::task_info> tasks;
  lg::process_dispatch_body("model_choice: {\"9\":{\"tier\":\"medium\",\"candidate\":\"c\"}}", tasks);
  REQUIRE(tasks.size() == 1);
  REQUIRE(tasks[9].iterations == 1);
  REQUIRE_FALSE(tasks[9].has_info);
  REQUIRE(tasks[9].work_type.empty());
}

TEST_CASE("legacy process_dispatch_body takes the LAST complete triple") {
  std::map<std::int64_t, lg::task_info> tasks;
  lg::process_dispatch_body("model_choice: {\"1\":{\"tier\":\"medium\",\"candidate\":\"old\",\"work_type\":\"feature\"}}", tasks);
  lg::process_dispatch_body("model_choice: {\"1\":{\"tier\":\"large\",\"candidate\":\"new\",\"work_type\":\"schema\"}}", tasks);
  REQUIRE(tasks[1].candidate == "new");
  REQUIRE(tasks[1].work_type == "schema");
  REQUIRE(tasks[1].iterations == 2);
}

TEST_CASE("legacy process_dispatch_body reads ONLY the first model_choice line") {
  // The reader returns as soon as it has processed one, so a second line is
  // invisible. A note carrying two would silently lose the second half.
  std::map<std::int64_t, lg::task_info> tasks;
  lg::process_dispatch_body("model_choice: {\"1\":{\"tier\":\"t\",\"candidate\":\"c\",\"work_type\":\"feature\"}}\n"
                            "model_choice: {\"2\":{\"tier\":\"t\",\"candidate\":\"c\",\"work_type\":\"feature\"}}",
                            tasks);
  REQUIRE(tasks.size() == 1);
  REQUIRE(tasks.contains(1));
  REQUIRE_FALSE(tasks.contains(2));
}

TEST_CASE("legacy process_dispatch_body ignores a note with no model_choice line") {
  // Contributes NOTHING — it does not even create a map entry, which is why it
  // is not counted in legacy_dispatch_notes_skipped either.
  std::map<std::int64_t, lg::task_info> tasks;
  lg::process_dispatch_body("dispatch_shape: legacy note with no model_choice line", tasks);
  REQUIRE(tasks.empty());
}

TEST_CASE("legacy process_dispatch_body silently skips malformed input") {
  // Every one of these is a skip, never an error: a malformed dispatch note is
  // an orchestrator bug the operator cannot act on from here, and failing the
  // whole scorecard over one would hide every good row behind it.
  std::map<std::int64_t, lg::task_info> tasks;
  lg::process_dispatch_body("model_choice:", tasks);                      // empty value
  lg::process_dispatch_body("model_choice: {not json", tasks);            // unparseable
  lg::process_dispatch_body("model_choice: [1,2,3]", tasks);              // not an object
  lg::process_dispatch_body("model_choice: {\"notanint\":{}}", tasks);    // non-integer key
  lg::process_dispatch_body("model_choice: {\"1\":\"a string\"}", tasks); // entry not an object
  REQUIRE(tasks.size() == 1);
  // The non-object ENTRY still incremented the iteration, because the key
  // parsed; the non-integer KEY did not, because it never became an entry.
  REQUIRE(tasks[1].iterations == 1);
  REQUIRE_FALSE(tasks[1].has_info);
}

TEST_CASE("legacy process_dispatch_body rejects a NON-STRING triple field") {
  std::map<std::int64_t, lg::task_info> tasks;
  lg::process_dispatch_body("model_choice: {\"1\":{\"tier\":\"t\",\"candidate\":\"c\",\"work_type\":42}}", tasks);
  REQUIRE_FALSE(tasks[1].has_info);
}

TEST_CASE("legacy process_dispatch_body rejects an EMPTY triple field") {
  std::map<std::int64_t, lg::task_info> tasks;
  lg::process_dispatch_body("model_choice: {\"1\":{\"tier\":\"\",\"candidate\":\"c\",\"work_type\":\"feature\"}}", tasks);
  REQUIRE_FALSE(tasks[1].has_info);
}

TEST_CASE("legacy process_dispatch_body tolerates leading whitespace and CR") {
  std::map<std::int64_t, lg::task_info> tasks;
  lg::process_dispatch_body("   model_choice: {\"1\":{\"tier\":\"t\",\"candidate\":\"c\",\"work_type\":\"f\"}}\r", tasks);
  REQUIRE(tasks[1].has_info);
}

// ===========================================================================
// aggregate
// ===========================================================================

TEST_CASE("legacy aggregate on an empty database reports nothing skipped") {
  const scratch_db_path scratch;
  auto                  conn  = open_migrated(scratch);
  const auto            value = lg::aggregate(conn);
  REQUIRE(value.has_value());
  REQUIRE(value->scorecard.empty());
  REQUIRE(value->recommendations.empty());
  REQUIRE(value->legacy_dispatch_notes_skipped == 0);
}

TEST_CASE("legacy aggregate reproduces the oracle's populated scorecard") {
  const scratch_db_path scratch;
  auto                  conn = open_migrated(scratch);
  seed_oracle_fixture(conn);

  const auto value = lg::aggregate(conn);
  REQUIRE(value.has_value());
  REQUIRE(value->scorecard.size() == 2);

  const auto& feature = value->scorecard[0];
  REQUIRE(feature.work_type == "feature");
  REQUIRE(feature.candidate == "claude-sonnet-5");
  REQUIRE(feature.vendor == "claude");
  REQUIRE(feature.tier == "medium");
  REQUIRE(feature.dispatch_count == 2);
  REQUIRE(feature.approved_count == 1);
  REQUIRE(feature.aborted_count == 1);
  REQUIRE(feature.other_count == 0);
  REQUIRE(feature.approval_rate == 0.5);
  // Task 1 was named in two notes, task 2 in one: 3 iterations over 2 tasks.
  REQUIRE(feature.avg_iterations == 1.5);
  REQUIRE(feature.test_coder_ok_count == 1);
  REQUIRE(feature.test_coder_other_count == 1);
  REQUIRE_FALSE(feature.insufficient_data);
  REQUIRE(feature.rank == 1);

  const auto& schema = value->scorecard[1];
  REQUIRE(schema.work_type == "schema");
  REQUIRE(schema.tier == "large");
  REQUIRE(schema.approval_rate == 1.0);
  REQUIRE(schema.test_coder_ok_count == 0);
  REQUIRE(schema.test_coder_other_count == 0);
  // rank restarts at 1 at every work_type boundary.
  REQUIRE(schema.rank == 1);

  // Task 9 was named with an incomplete triple. The counter counts TASKS.
  REQUIRE(value->legacy_dispatch_notes_skipped == 1);
}

TEST_CASE("legacy aggregate folds a task with NO claim into other_count") {
  // An undispatched task must not raise the approval rate, and it does not get
  // a bucket of its own.
  const scratch_db_path scratch;
  auto                  conn = open_migrated(scratch);
  seed_session(conn);
  add_note(conn, 1,
           "dispatch_shape: x\nmodel_choice: {\"1\":{\"tier\":\"t\",\"candidate\":\"c\","
           "\"work_type\":\"feature\"}}");

  const auto value = lg::aggregate(conn);
  REQUIRE(value->scorecard.size() == 1);
  REQUIRE(value->scorecard[0].other_count == 1);
  REQUIRE(value->scorecard[0].approved_count == 0);
  REQUIRE(value->scorecard[0].approval_rate == 0.0);
}

TEST_CASE("legacy aggregate reads the MOST RECENT claim, not the first") {
  const scratch_db_path scratch;
  auto                  conn = open_migrated(scratch);
  seed_session(conn);
  add_note(conn, 1,
           "model_choice: {\"1\":{\"tier\":\"t\",\"candidate\":\"c\",\"work_type\":\"feature\"}}\n"
           "dispatch_shape: x");
  add_claim(conn, "old", 1, "aborted", "c", "2026-01-01T00:00:01Z");
  add_claim(conn, "new", 1, "completed", "c", "2026-01-01T00:00:09Z");

  const auto value = lg::aggregate(conn);
  REQUIRE(value->scorecard[0].approved_count == 1);
  REQUIRE(value->scorecard[0].aborted_count == 0);
}

TEST_CASE("legacy aggregate leaves both test-coder counters at zero when no action exists") {
  // They need not sum to dispatch_count, and that is the reason.
  const scratch_db_path scratch;
  auto                  conn = open_migrated(scratch);
  seed_session(conn);
  add_note(conn, 1,
           "dispatch_shape: x\nmodel_choice: {\"1\":{\"tier\":\"t\",\"candidate\":\"c\","
           "\"work_type\":\"feature\"}}");
  add_claim(conn, "t", 1, "completed", "c", "2026-01-01T00:00:01Z");

  const auto value = lg::aggregate(conn);
  REQUIRE(value->scorecard[0].dispatch_count == 1);
  REQUIRE(value->scorecard[0].test_coder_ok_count == 0);
  REQUIRE(value->scorecard[0].test_coder_other_count == 0);
}

TEST_CASE("legacy aggregate reports a NULL vendor when no claim carries the model") {
  // Vendor is read back from what an agent recorded, never inferred from a
  // catalog. No claim naming this model string means no answer, not a guess.
  const scratch_db_path scratch;
  auto                  conn = open_migrated(scratch);
  seed_session(conn);
  add_note(conn, 1,
           "dispatch_shape: x\nmodel_choice: {\"1\":{\"tier\":\"t\",\"candidate\":\"unknown-model\","
           "\"work_type\":\"feature\"}}");
  add_claim(conn, "t", 1, "completed", "some-other-model", "2026-01-01T00:00:01Z");

  const auto value = lg::aggregate(conn);
  REQUIRE_FALSE(value->scorecard[0].vendor.has_value());
}

TEST_CASE("legacy aggregate ranks by approval rate DESCENDING within a work type") {
  const scratch_db_path scratch;
  auto                  conn = open_migrated(scratch);
  seed_session(conn);
  // `low` is alphabetically first and has the worse rate; correctness must win.
  add_note(conn, 1,
           "dispatch_shape: x\nmodel_choice: {\"1\":{\"tier\":\"t\",\"candidate\":\"low\","
           "\"work_type\":\"feature\"},\"2\":{\"tier\":\"t\",\"candidate\":\"zhigh\","
           "\"work_type\":\"feature\"}}");
  add_claim(conn, "c1", 1, "aborted", "low", "2026-01-01T00:00:01Z");
  add_claim(conn, "c2", 2, "completed", "zhigh", "2026-01-01T00:00:02Z");

  const auto value = lg::aggregate(conn);
  REQUIRE(value->scorecard.size() == 2);
  REQUIRE(value->scorecard[0].candidate == "zhigh");
  REQUIRE(value->scorecard[0].rank == 1);
  REQUIRE(value->scorecard[1].candidate == "low");
  REQUIRE(value->scorecard[1].rank == 2);
}

TEST_CASE("legacy aggregate breaks an approval-rate tie on FEWER iterations") {
  const scratch_db_path scratch;
  auto                  conn = open_migrated(scratch);
  seed_session(conn);
  // Both approved; `slow` needed two notes, `zfast` one. Speed is only ever a
  // tie-break among equally-approved candidates.
  add_note(conn, 1,
           "dispatch_shape: x\nmodel_choice: {\"1\":{\"tier\":\"t\",\"candidate\":\"slow\","
           "\"work_type\":\"feature\"},\"2\":{\"tier\":\"t\",\"candidate\":\"zfast\","
           "\"work_type\":\"feature\"}}");
  add_note(conn, 2,
           "dispatch_shape: x\nmodel_choice: {\"1\":{\"tier\":\"t\",\"candidate\":\"slow\","
           "\"work_type\":\"feature\"}}");
  add_claim(conn, "c1", 1, "completed", "slow", "2026-01-01T00:00:01Z");
  add_claim(conn, "c2", 2, "completed", "zfast", "2026-01-01T00:00:02Z");

  const auto value = lg::aggregate(conn);
  REQUIRE(value->scorecard[0].candidate == "zfast");
  REQUIRE(value->scorecard[0].avg_iterations == 1.0);
  REQUIRE(value->scorecard[1].candidate == "slow");
  REQUIRE(value->scorecard[1].avg_iterations == 2.0);
}

TEST_CASE("legacy aggregate breaks a full tie on candidate name") {
  // The final key makes the order TOTAL, which is what makes two runs over the
  // same database byte-identical.
  //
  // BREAK-PROBE NOTE. The obvious two-row form of this test does NOT
  // discriminate, and a mutant proved it: deleting the candidate key entirely
  // (`return false;`) left a two-row fixture passing. The reason is that this
  // port accumulates groups in a `std::map` keyed `work_type\0candidate`, so
  // rows reach the sort ALREADY in candidate order and a comparator missing its
  // last key cannot get them wrong. (The Zig original accumulates in an
  // unordered `StringHashMap`, where the key is genuinely load-bearing — this
  // port is incidentally more deterministic than the code it mirrors.)
  //
  // Three rows were tried next, arranged so the tie group is NOT adjacent in
  // map order: `aaa` and `ccc` tie on rate, `bbb` outranks both and sits
  // between them alphabetically.
  //
  // THAT STILL DID NOT KILL THE MUTANT, and the honest conclusion is that
  // nothing can. Within one work_type the map key `work_type\0candidate`
  // already orders rows by candidate, so a tie group arrives at the sort in
  // exactly the order the final key would impose; whichever equivalent element
  // the sort emits first is the right one either way. The key is therefore
  // REDUNDANT-BY-CONSTRUCTION in this port.
  //
  // It is kept rather than deleted, because `std::ranges::sort` does not
  // promise stability: removing it would make the output depend on an
  // unspecified property of the sort rather than on the comparator, and the
  // determinism guarantee ("two runs over the same database are
  // byte-identical") would rest on luck. It is a guarantee the accumulator
  // happens to also provide, not dead code. This test still earns its place —
  // it pins the ORDER, which is what determines `rank`.
  const scratch_db_path scratch;
  auto                  conn = open_migrated(scratch);
  seed_session(conn);
  add_note(conn, 1,
           "dispatch_shape: x\nmodel_choice: {\"1\":{\"tier\":\"t\",\"candidate\":\"aaa\","
           "\"work_type\":\"feature\"},\"2\":{\"tier\":\"t\",\"candidate\":\"bbb\","
           "\"work_type\":\"feature\"},\"3\":{\"tier\":\"t\",\"candidate\":\"ccc\","
           "\"work_type\":\"feature\"}}");
  add_claim(conn, "c1", 1, "aborted", "aaa", "2026-01-01T00:00:01Z");
  add_claim(conn, "c2", 2, "completed", "bbb", "2026-01-01T00:00:02Z");
  add_claim(conn, "c3", 3, "aborted", "ccc", "2026-01-01T00:00:03Z");

  const auto value = lg::aggregate(conn);
  REQUIRE(value->scorecard.size() == 3);
  REQUIRE(value->scorecard[0].candidate == "bbb");
  REQUIRE(value->scorecard[1].candidate == "aaa");
  REQUIRE(value->scorecard[2].candidate == "ccc");
}

TEST_CASE("legacy aggregate treats a RELEASED claim as other, never as approved") {
  // `released` is a graceful give-up, not an approval. Only `completed` counts.
  // Found by a mutant: widening the predicate to `completed || released` was
  // invisible to every other test in this file, and it would silently inflate
  // the approval rate of any candidate whose work was handed back.
  const scratch_db_path scratch;
  auto                  conn = open_migrated(scratch);
  seed_session(conn);
  add_note(conn, 1,
           "dispatch_shape: x\nmodel_choice: {\"1\":{\"tier\":\"t\",\"candidate\":\"c\","
           "\"work_type\":\"feature\"}}");
  add_claim(conn, "t", 1, "released", "c", "2026-01-01T00:00:01Z");

  const auto value = lg::aggregate(conn);
  REQUIRE(value->scorecard[0].approved_count == 0);
  REQUIRE(value->scorecard[0].aborted_count == 0);
  REQUIRE(value->scorecard[0].other_count == 1);
  REQUIRE(value->scorecard[0].approval_rate == 0.0);
}

TEST_CASE("legacy aggregate treats a STALE claim as other too") {
  // The other non-terminal-success status the schema permits. Same reasoning.
  const scratch_db_path scratch;
  auto                  conn = open_migrated(scratch);
  seed_session(conn);
  add_note(conn, 1,
           "dispatch_shape: x\nmodel_choice: {\"1\":{\"tier\":\"t\",\"candidate\":\"c\","
           "\"work_type\":\"feature\"}}");
  add_claim(conn, "t", 1, "stale", "c", "2026-01-01T00:00:01Z");
  REQUIRE(lg::aggregate(conn)->scorecard[0].other_count == 1);
}

TEST_CASE("legacy aggregate recommends ONLY the rank-1 row, never a runner-up") {
  // A recommendation is a routing suggestion; emitting one per row would turn
  // it into a listing and make "recommended" meaningless. Found by a mutant:
  // no other test in this file had two scored rows in ONE work type AND
  // asserted on the recommendations.
  const scratch_db_path scratch;
  auto                  conn = open_migrated(scratch);
  seed_session(conn);
  add_note(conn, 1,
           "dispatch_shape: x\nmodel_choice: {\"1\":{\"tier\":\"t\",\"candidate\":\"low\","
           "\"work_type\":\"feature\"},\"2\":{\"tier\":\"t\",\"candidate\":\"zhigh\","
           "\"work_type\":\"feature\"}}");
  add_claim(conn, "c1", 1, "aborted", "low", "2026-01-01T00:00:01Z");
  add_claim(conn, "c2", 2, "completed", "zhigh", "2026-01-01T00:00:02Z");

  const auto value = lg::aggregate(conn);
  REQUIRE(value->scorecard.size() == 2);
  REQUIRE(value->recommendations.size() == 1);
  REQUIRE(value->recommendations[0].candidate == "zhigh");
}

TEST_CASE("legacy aggregate recommends exactly the rank-1 row of each work type") {
  const scratch_db_path scratch;
  auto                  conn = open_migrated(scratch);
  seed_oracle_fixture(conn);

  const auto value = lg::aggregate(conn);
  REQUIRE(value->recommendations.size() == 2);
  REQUIRE(value->recommendations[0].work_type == "feature");
  REQUIRE(value->recommendations[0].candidate == "claude-sonnet-5");
  REQUIRE(value->recommendations[0].rationale ==
          "1/2 approved, avg 1.50 iterations to approval (highest-ranked scored candidate for feature)");
  REQUIRE(value->recommendations[1].work_type == "schema");
  // Two decimals ALWAYS, even for a whole number — the rationale uses a fixed
  // format while the JSON number beside it does not.
  REQUIRE(value->recommendations[1].rationale ==
          "1/1 approved, avg 1.00 iterations to approval (highest-ranked scored candidate for schema)");
}

TEST_CASE("legacy aggregate is deterministic across repeated runs") {
  const scratch_db_path scratch;
  auto                  conn = open_migrated(scratch);
  seed_oracle_fixture(conn);
  REQUIRE(lg::evals_json(*lg::aggregate(conn)) == lg::evals_json(*lg::aggregate(conn)));
}

TEST_CASE("legacy aggregate writes nothing") {
  // D8: read-only. A run must leave the tables it reads byte-identical.
  const scratch_db_path scratch;
  auto                  conn = open_migrated(scratch);
  seed_oracle_fixture(conn);

  const auto count_rows = [&conn](std::string_view table) {
    auto stmt = conn.prepare(std::format("select count(*) from {}", table));
    REQUIRE(stmt.has_value());
    REQUIRE(stmt->step().has_value());
    return stmt->column_int64(0);
  };
  const auto before = std::tuple{count_rows("session_entries"), count_rows("agent_work_claims"), count_rows("agent_actions")};
  REQUIRE(lg::aggregate(conn).has_value());
  const auto after = std::tuple{count_rows("session_entries"), count_rows("agent_work_claims"), count_rows("agent_actions")};
  REQUIRE(before == after);
}

// ===========================================================================
// renderers — byte exactness
// ===========================================================================

TEST_CASE("legacy evals_json on an empty result reproduces the oracle's bytes") {
  REQUIRE(lg::evals_json({}) == "{\"scorecard\":[],\"recommendations\":[],\"signals_sourced\":{\"reviewer_disposition\":true,"
                                "\"iteration_count\":true,\"quality_gate_pass_fail\":false,\"test_coder_expansion\":true},"
                                "\"legacy_dispatch_notes_skipped\":0}\n");
}

TEST_CASE("legacy evals_text on an empty result reproduces the oracle's bytes") {
  REQUIRE(lg::evals_text({}) == "routing evals scorecard (per work-type, candidate) — read-only, writes nothing:\n"
                                "  (no completed dispatch history recorded yet)\n"
                                "\n"
                                "recommendations (preview only — writes nothing; apply is a separate operator-gated step):\n"
                                "  (none — no work type has scored dispatch history yet)\n"
                                "\n"
                                "signals sourced: reviewer_disposition=yes iteration_count=yes quality_gate_pass_fail=no "
                                "test_coder_expansion=yes\n");
}

TEST_CASE("legacy the empty envelope reports quality_gate_pass_fail FALSE") {
  // On an EMPTY database, while the other three are true. It is a capability
  // statement about which signals the implementation sources at all — the
  // quality gate is not persisted anywhere in the schema — and must never be
  // derived from data.
  REQUIRE(lg::evals_json({}).find("\"quality_gate_pass_fail\":false") != std::string::npos);
  REQUIRE(lg::evals_json({}).find("\"reviewer_disposition\":true") != std::string::npos);
  REQUIRE(lg::evals_text({}).find("quality_gate_pass_fail=no") != std::string::npos);
}

TEST_CASE("legacy evals_json reproduces the oracle's populated bytes") {
  const scratch_db_path scratch;
  auto                  conn = open_migrated(scratch);
  seed_oracle_fixture(conn);

  REQUIRE(lg::evals_json(*lg::aggregate(conn)) ==
          "{\"scorecard\":["
          "{\"work_type\":\"feature\",\"candidate\":\"claude-sonnet-5\",\"vendor\":\"claude\",\"tier\":"
          "\"medium\",\"dispatch_count\":2,\"approved_count\":1,\"aborted_count\":1,\"other_count\":0,"
          "\"approval_rate\":0.5,\"avg_iterations\":1.5,\"test_coder_ok_count\":1,"
          "\"test_coder_other_count\":1,\"insufficient_data\":false,\"rank\":1},"
          "{\"work_type\":\"schema\",\"candidate\":\"claude-opus-5\",\"vendor\":\"claude\",\"tier\":"
          "\"large\",\"dispatch_count\":1,\"approved_count\":1,\"aborted_count\":0,\"other_count\":0,"
          "\"approval_rate\":1,\"avg_iterations\":1,\"test_coder_ok_count\":0,"
          "\"test_coder_other_count\":0,\"insufficient_data\":false,\"rank\":1}],"
          "\"recommendations\":["
          "{\"work_type\":\"feature\",\"vendor\":\"claude\",\"tier\":\"medium\",\"candidate\":"
          "\"claude-sonnet-5\",\"rationale\":\"1/2 approved, avg 1.50 iterations to approval "
          "(highest-ranked scored candidate for feature)\"},"
          "{\"work_type\":\"schema\",\"vendor\":\"claude\",\"tier\":\"large\",\"candidate\":"
          "\"claude-opus-5\",\"rationale\":\"1/1 approved, avg 1.00 iterations to approval "
          "(highest-ranked scored candidate for schema)\"}],"
          "\"signals_sourced\":{\"reviewer_disposition\":true,\"iteration_count\":true,"
          "\"quality_gate_pass_fail\":false,\"test_coder_expansion\":true},"
          "\"legacy_dispatch_notes_skipped\":1}\n");
}

TEST_CASE("legacy evals_json renders a whole approval rate as 1, not 1.0") {
  // Shortest round-trippable, no forced fractional part. The RATIONALE for the
  // very same row says `1.00`; the two formats disagree on purpose.
  const scratch_db_path scratch;
  auto                  conn = open_migrated(scratch);
  seed_oracle_fixture(conn);
  const auto out = lg::evals_json(*lg::aggregate(conn));
  REQUIRE(out.find("\"approval_rate\":1,") != std::string::npos);
  REQUIRE(out.find("\"approval_rate\":1.0") == std::string::npos);
  REQUIRE(out.find("avg 1.00 iterations") != std::string::npos);
}

TEST_CASE("legacy evals_text reproduces the oracle's populated bytes") {
  const scratch_db_path scratch;
  auto                  conn = open_migrated(scratch);
  seed_oracle_fixture(conn);

  REQUIRE(lg::evals_text(*lg::aggregate(conn)) ==
          "routing evals scorecard (per work-type, candidate) — read-only, writes nothing:\n"
          "  feature        claude-sonnet-5          rank 1  1/2 approved  avg 1.50 iter  [claude/medium]\n"
          "  schema         claude-opus-5            rank 1  1/1 approved  avg 1.00 iter  [claude/large]\n"
          "\n"
          "recommendations (preview only — writes nothing; apply is a separate operator-gated step):\n"
          "  feature → claude-sonnet-5 [claude/medium]: 1/2 approved, avg 1.50 iterations to approval "
          "(highest-ranked scored candidate for feature)\n"
          "  schema → claude-opus-5 [claude/large]: 1/1 approved, avg 1.00 iterations to approval "
          "(highest-ranked scored candidate for schema)\n"
          "\n"
          "signals sourced: reviewer_disposition=yes iteration_count=yes quality_gate_pass_fail=no "
          "test_coder_expansion=yes\n"
          "note: 1 dispatch note(s) skipped — missing/malformed model_choice work_type\n");
}

TEST_CASE("legacy evals_text omits the note line when nothing was skipped") {
  lg::result value;
  REQUIRE(lg::evals_text(value).find("dispatch note(s) skipped") == std::string::npos);
  value.legacy_dispatch_notes_skipped = 3;
  REQUIRE(lg::evals_text(value).ends_with("note: 3 dispatch note(s) skipped — missing/malformed model_choice work_type\n"));
}

TEST_CASE("legacy evals_text renders a null vendor as a question mark") {
  lg::result value;
  value.scorecard.push_back({"feature", "c", std::nullopt, "medium", 1, 1, 0, 0, 1.0, 1.0, 0, 0, false, 1});
  REQUIRE(lg::evals_text(value).find("[?/medium]") != std::string::npos);
}

TEST_CASE("legacy evals_json renders a null vendor as JSON null") {
  lg::result value;
  value.scorecard.push_back({"feature", "c", std::nullopt, "medium", 1, 1, 0, 0, 1.0, 1.0, 0, 0, false, 1});
  REQUIRE(lg::evals_json(value).find("\"vendor\":null") != std::string::npos);
}

TEST_CASE("legacy renders the insufficient-data row, which the CLI cannot reach") {
  // Sibling enumeration was deleted with the model catalog (plan 950), so no
  // aggregation ever sets this flag and no end-to-end fixture can exercise it.
  // The struct field and both render arms are still part of the wire contract,
  // so they are driven DIRECTLY here rather than left unexecuted.
  lg::result value;
  value.scorecard.push_back({"feature", "unseen", "claude", "medium", 0, 0, 0, 0, 0.0, 0.0, 0, 0, true, std::nullopt});
  REQUIRE(lg::evals_text(value).find("  feature        unseen                   insufficient-data  [claude/medium]\n") !=
          std::string::npos);
  REQUIRE(lg::evals_json(value).find("\"insufficient_data\":true,\"rank\":null") != std::string::npos);
}

TEST_CASE("legacy evals_json escapes an operator-authored candidate string") {
  lg::result value;
  value.scorecard.push_back({"feature", "q\"uote\\back", "claude", "medium", 1, 1, 0, 0, 1.0, 1.0, 0, 0, false, 1});
  REQUIRE(lg::evals_json(value).find("\"candidate\":\"q\\\"uote\\\\back\"") != std::string::npos);
}

TEST_CASE("legacy cohort_requires_project_error is the cohort branch's refusal") {
  // The ONLY thing `--vendor` does when the rest of the cohort is unspecified,
  // and what distinguishes "fell through to legacy" from "tried to rank".
  REQUIRE(lg::cohort_requires_project_error() == "error: --project is required when ranking a cohort\n");
}
