// @file models_leaves.t.cpp
// @brief In-process tests for all fourteen `models` leaves — the ten
// `models registry` leaves plus `evals`, `experiments` and `outcomes`,
// wired by plan 996 task 6149, and `resolve`, wired by task 6343.
//
// ## EVERY MUTATING CASE ASSERTS DATABASE ROWS
//
// Four of the ten registry leaves print NOTHING on success — `bind`,
// `unbind`, `update` and `remove` all exit 0 with zero bytes on both
// streams — so stdout carries no information about whether they did
// anything at all. `registry_rows`, `binding_rows` and `observation_rows`
// read the tables back with SQL NULL rendered as the literal `<NULL>`.
//
// ## EVERY FILTER IS PROVEN TO EXCLUDE, SURVIVAL ASSERTED FIRST
//
// Three filters live in this family and each is seeded on BOTH sides:
//
//   - `registry eligibility --host`. Two observations are written for one
//     candidate, by hosts `h1` and `h2`, with DIFFERENT verdicts and a
//     HIGHER version on `h2`. Asking for `h1` must resolve `h1`'s, not the
//     newer one — an observation is never substituted across hosts. A
//     third query names a host with no observation at all and must report
//     the expired/unavailable gates rather than silently borrowing one.
//   - `registry eligibility --role/--tier`. The candidate is bound for
//     (coder, medium) only; the (coder, large) query must report
//     `role_tier_not_bound` while the (coder, medium) one does not, on the
//     same row.
//   - `models outcomes --limit`. 63 samples are seeded. The default (50),
//     an explicit 2, and an explicit 1000 are all asserted, so the rows the
//     capped queries omit are demonstrably present under the wide one.
//     A capped query alone proves nothing: an engine returning zero rows
//     and one honouring the cap look the same at limit 2 on a 2-row table.
//
// ## THE NUMERIC BRANCHES ARE PINNED AS EXACT BYTES
//
// `models evals`'s cohort branch is a 95% Wilson lower bound, and the
// bound's LAST DIGIT reaches stdout because the renderer prints the
// shortest round-trippable form. `engine_models` compiles `ranking.cpp`
// with `-ffp-contract=off` for exactly this reason (clang's default fuses
// `p*(1-p) + z2/(4n)` into an `fma` and moves the result one ULP). That
// flag is guarded by an engine test; what is guarded HERE is that the
// digits survive the whole handler path into the emitted JSON.
//
// The fixture is the four-candidate cohort `ranking.cppm`'s header
// describes, seeded through the full experiment -> snapshot -> event ->
// terminal-sample chain (migration 00030's identity constraint refuses a
// shortcut). All THREE outcomes are pinned, and "no recommendation" is
// pinned as a CORRECT answer twice over:
//
//   default gates    -> rank 1 = cand-1, recommended "cand-1"
//   --quality-floor 0.99
//                    -> every row gated, recommended null, reason
//                       "no candidate cleared both the minimum-sample and
//                        quality-floor gates"
//   --complexity bounded (a cohort with no samples)
//                    -> rows [], recommended null, reason
//                       "no cohort-eligible declared-experiment samples"
//
// `cand-3` is the row the module exists for: 3-for-3, a PERFECT raw rate,
// and it ranks nowhere because three samples is under the minimum. It is
// asserted `insufficient_data` AND `below_quality_floor:false` — the
// sample gate short-circuits the quality gate, even though 0.4385 really
// is under the 0.5 floor.
//
// ## ORACLE PROVENANCE
//
// Every byte below was captured by running `zig/zig-out/bin/planar` and
// this binary over the SAME argv script, each pinned to its own scratch
// root, and diffing stdout / stderr / exit code / every row of
// `routing_candidates`, `routing_candidate_bindings` and
// `routing_host_observations`. 49 registry invocations and 72 evals /
// experiments / outcomes invocations agree byte for byte and column for
// column. The captures that decided a shape:
//
//   $Z models registry list        (empty)  exit 0, ZERO BYTES
//   $Z models registry list --json (empty)  exit 0, the full envelope with
//                                           "candidates":[]
//   $Z models registry bind ...    (ok)     exit 0, ZERO BYTES
//   $Z models registry export               stdout the envelope,
//                                           stderr b'warning: legacy catalog
//                                             compatibility is one-window and
//                                             non-authoritative\n'
//   $Z models registry export --json        the SAME stdout, stderr EMPTY
//   $Z models registry add <dup>   exit 3  b'error: registering opaque
//                                            candidate: Conflict\n'
//   $Z models registry bind --candidate 99
//                                  exit 1  b'error: binding candidate: QueryFailed\n'
//   $Z models registry observe --candidate 99
//                                  exit 3  b'error: recording host observation: Conflict\n'
//                                  ^ the SAME missing row, classified
//                                    differently by the two leaves.
//   $Z models registry verify-identity --candidate 99
//                                  exit 1  b'error: candidate 99 not found\n'
//                                  ^ the one leaf that does not use the
//                                    `<gerund>: <ErrorName>` shape.
//   $Z models evals --role r       exit 0, the LEGACY scorecard
//   $Z models evals --vendor v     exit 2, b'error: --project is required
//                                            when ranking a cohort\n'
//                                  ^ `--vendor` alone selects the branch.

#include <catch2/catch_test_macros.hpp>
// Glaze is this binary's JSON READER (see this target's CMakeLists note:
// `planar.json_text` is emit-only and stays that way). Task 6186 asks the
// `--json` payload to be asserted as PARSEABLE rather than by substring,
// which needs a parser rather than an emitter.
#include <glaze/glaze.hpp>

import std;
import cli11;
import planar.db;
import planar.cmd.planar.context;
import planar.cmd.planar.dispatch;
import planar.cmd.planar.main;

#include "json_envelope_test_support.hpp"

namespace {

using planar::cmd::context;

/// @brief One handler invocation's observable result.
struct invocation {
  int         code = 0;        ///< The exit code.
  std::string out;             ///< Everything written to stdout.
  std::string err;             ///< Everything written to stderr.
  bool        db_open = false; ///< Whether the verb opened SQLite at all.
};

/// @brief A scratch root plus the environment and database path every case
/// in this file dispatches against.
struct fixture {
  std::filesystem::path                           root;    ///< The scratch root.
  std::map<std::string, std::string, std::less<>> vars;    ///< The environment map.
  std::filesystem::path                           db_path; ///< The scratch database path.
};

/// @brief Build a fixture under a unique scratch directory. Reads nothing
/// from the real environment, so the operator's database is unreachable.
/// @param tag A short discriminator so a failure names its own case.
/// @return The fixture.
auto make_fixture(std::string_view tag) -> fixture {
  auto const      root = std::filesystem::temp_directory_path() /
                         std::format("planar_models_{}_{}", tag, std::chrono::steady_clock::now().time_since_epoch().count());
  std::error_code ec;
  std::filesystem::create_directories(root / "home", ec);
  std::filesystem::create_directories(root / "proj", ec);
  return fixture{
      .root    = root,
      .vars    = {{"PLANAR_HOME", (root / "home").string()},
                  {"HOME", (root / "fakehome").string()},
                  {"PWD", (root / "proj").string()}},
      .db_path = root / "planar.db",
  };
}

/// @brief Dispatch `args` against the real tree and table inside `fx`.
/// @param fx The fixture.
/// @param args The argv tail.
/// @return The captured invocation.
auto dispatch(const fixture& fx, std::vector<std::string> args) -> invocation {
  std::vector<std::string> argv{"planar"};
  argv.insert(argv.end(), args.begin(), args.end());

  std::ostringstream out;
  std::ostringstream err;
  context            ctx{std::move(argv),
                         planar::cmd::map_env(fx.vars),
                         fx.root / "proj",
                         std::make_shared<planar::cmd::database>(fx.db_path, err),
                         out,
                         err};
  auto const         tree  = planar::cmd::root_app();
  auto const         table = planar::cmd::make_handler_table(*tree);
  int const          code  = planar::cmd::run(ctx, *tree, table);
  return invocation{.code = code, .out = out.str(), .err = err.str(), .db_open = ctx.db().opened()};
}

/// @brief Open the fixture's database directly, for row assertions.
/// @param fx The fixture.
/// @return The open connection.
auto open_db(const fixture& fx) -> planar::db::connection {
  auto conn = planar::db::connection::open(fx.db_path.string());
  REQUIRE(conn.has_value());
  return std::move(*conn);
}

/// @brief Run one statement for its effect.
/// @param conn An open connection.
/// @param sql The statement.
auto exec(planar::db::connection& conn, std::string_view sql) -> void {
  auto stmt = conn.prepare(sql);
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->step().has_value());
}

/// @brief Collect one query's rows, pipe-joining columns and newline-joining
/// rows, with SQL NULL rendered as the literal `<NULL>`.
/// @param conn An open connection.
/// @param sql The query.
/// @param columns How many columns to read.
/// @return The rendered rows.
auto query(planar::db::connection& conn, std::string_view sql, int columns) -> std::string {
  auto stmt = conn.prepare(sql);
  REQUIRE(stmt.has_value());
  std::string joined;
  while (true) {
    auto stepped = stmt->step();
    REQUIRE(stepped.has_value());
    if (*stepped == planar::db::step_result::done) {
      break;
    }
    if (!joined.empty()) {
      joined += '\n';
    }
    for (int col = 0; col < columns; ++col) {
      if (col > 0) {
        joined += '|';
      }
      joined += stmt->column_text(col);
    }
  }
  return joined;
}

/// @brief Every `routing_candidates` row, non-timestamp columns only.
/// @param conn An open connection.
/// @return The rendered rows, one per line, in id order.
auto registry_rows(planar::db::connection& conn) -> std::string {
  return query(conn, R"(select cast(id as text), vendor, candidate_id, cast(enabled as text),
                          cast(fallback_order as text), cast(registration_version as text), compatibility_source
                        from routing_candidates order by id)",
               7);
}

/// @brief Every `routing_candidate_bindings` row.
/// @param conn An open connection.
/// @return The rendered rows, one per line.
auto binding_rows(planar::db::connection& conn) -> std::string {
  return query(conn, R"(select cast(candidate_id as text), role, tier
                        from routing_candidate_bindings order by candidate_id, role, tier)",
               3);
}

/// @brief Every `routing_host_observations` row.
/// @param conn An open connection.
/// @return The rendered rows, one per line, in id order.
auto observation_rows(planar::db::connection& conn) -> std::string {
  return query(conn, R"(select cast(candidate_id as text), host_id, cast(observation_version as text),
                          availability, spawn_verification, evidence_ref, captured_at, expires_at
                        from routing_host_observations order by id)",
               8);
}

/// @brief The registered project every case needs (`routing_experiments`
/// and `routing_terminal_samples` both carry a `project_id` FK).
/// @param fx The fixture.
auto seed_project(const fixture& fx) -> void {
  REQUIRE(dispatch(fx, {"init", "--json"}).code == 0);
}

/// @brief Seed the four-candidate cohort `ranking.cppm`'s header describes.
///
/// The registrations go through the CLI (that is one of the leaves under
/// test); the evidence chain is raw SQL because the verbs that would write
/// it are not part of this binary's surface at all. The full snapshot ->
/// event -> terminal-sample chain is written per work item because
/// `routing_terminal_samples_identity` (migration 00030) refuses a sample
/// whose terminal event does not belong to a matching declared-experiment
/// dispatch — shortcutting it would not merely be unrealistic, it would
/// not insert.
/// @param fx The fixture.
/// @param excluded How many extra `cohort_eligible = 0` samples to append
/// after the four-candidate plan. They are declared in the experiment's
/// frozen population like every other work item — see this function's note
/// on why they cannot be added later or left out of it.
auto seed_cohort(const fixture& fx, int excluded = 0) -> void {
  seed_project(fx);
  for (int i = 1; i <= 4; ++i) {
    REQUIRE(dispatch(fx, {"models", "registry", "add", "--vendor", "anthropic", "--id", std::format("cand-{}", i), "--order",
                          std::to_string(i)})
                .code == 0);
  }

  struct plan_entry {
    int cand;
    int samples;
    int successes;
  };
  constexpr std::array<plan_entry, 4> plan{{{1, 20, 16}, {2, 20, 12}, {3, 3, 3}, {4, 20, 4}}};

  std::string population = "[";
  for (auto const& entry : plan) {
    for (int k = 0; k < entry.samples; ++k) {
      if (population.size() > 1) {
        population.push_back(',');
      }
      population.append(std::format("\"w-{}-{}\"", entry.cand, k));
    }
  }
  for (int i = 0; i < excluded; ++i) {
    population.append(std::format(",\"x-{}\"", i));
  }
  population.push_back(']');

  auto conn = open_db(fx);
  exec(conn, std::format("insert into routing_experiments (id, experiment_key, project_id, validation_policy_version, "
                         "vendor, role, tier, work_type, complexity, routing_policy_version, eligible_population_json, "
                         "candidate_set_json, allocation_method, stopping_rule_json, analysis_policy_json, "
                         "manifest_digest, operator_approved_at, status) values (1,'exp-1',1,'vp-1','anthropic','coder',"
                         "'medium','engine','standard','rp-1','{}','[1,2,3,4]','balanced','{{}}','{{}}','digest-1',"
                         "'2026-08-01T00:00:00Z','running')",
                         population));

  int dispatch_id = 0;
  for (auto const& entry : plan) {
    for (int k = 0; k < entry.samples; ++k) {
      ++dispatch_id;
      auto const work_item = std::format("w-{}-{}", entry.cand, k);
      auto const state     = k < entry.successes ? "completed" : "quality_failed";
      int const  success   = k < entry.successes ? 1 : 0;
      exec(conn, std::format("insert into routing_dispatch_snapshots (id,dispatch_key,logical_work_item_id,"
                             "project_id,validation_policy_version,routing_policy_version,profile_rule_version,"
                             "vendor,role,tier,work_type,complexity,packet_digest,policy_digest,capability_digest,"
                             "requested_candidate_id,assignment_class,experiment_id,operator_decision,"
                             "reviewer_disposition,terminal_state,confirmed_at) values ({},'dk-{}','{}',1,'vp-1',"
                             "'rp-1','pr-1','anthropic','coder','medium','engine','standard','pd-{}','pol-1',"
                             "'cap-1',{},'declared_experiment',1,'confirmed','approved','{}','2026-08-02T00:00:00Z')",
                             dispatch_id, dispatch_id, work_item, dispatch_id, entry.cand, state));
      exec(conn, std::format("insert into routing_dispatch_events (dispatch_id,event_id,sequence,event_kind,"
                             "attempt_number,terminal_state,payload_json,occurred_at) values ({},'ev-{}',0,"
                             "'outcome',1,'{}','{{}}','2026-08-02T00:00:00Z')",
                             dispatch_id, dispatch_id, state));
      exec(conn, std::format("insert into routing_terminal_samples (experiment_id,logical_work_item_id,role,"
                             "initial_packet_digest,candidate_id,project_id,validation_policy_version,"
                             "routing_policy_version,vendor,tier,work_type,complexity,terminal_event_id,"
                             "terminal_state,quality_success,cohort_eligible,finalized_at) values (1,'{}',"
                             "'coder','pd-{}',{},1,'vp-1','rp-1','anthropic','medium','engine','standard',"
                             "'ev-{}','{}',{},1,'2026-08-03T00:00:00Z')",
                             work_item, dispatch_id, entry.cand, dispatch_id, state, success));
    }
  }

  // The EXCLUDED tail. Written here, at seed time, rather than flipped onto
  // existing rows afterwards, and that is forced by the schema in TWO
  // places rather than chosen:
  //
  //   - `routing_terminal_samples` carries a `..._immutable` trigger, so
  //     `update routing_terminal_samples set cohort_eligible = 0` silently
  //     changes ZERO rows. Terminal samples are append-only evidence.
  //   - `routing_dispatch_snapshots` refuses a dispatch whose work item is
  //     not in the experiment's FROZEN `eligible_population_json`
  //     ("routing dispatch does not match frozen experiment"), so an
  //     excluded sample cannot be smuggled in outside the declared
  //     population either.
  //
  // So an excluded sample is one that was declared and then set aside, with
  // a NAMED reason — which is exactly what `models outcomes` reports and
  // what makes `experiments`' two counts diverge.
  for (int i = 0; i < excluded; ++i) {
    int const  id        = 1000 + i;
    auto const work_item = std::format("x-{}", i);
    exec(conn, std::format("insert into routing_dispatch_snapshots (id,dispatch_key,logical_work_item_id,"
                           "project_id,validation_policy_version,routing_policy_version,profile_rule_version,"
                           "vendor,role,tier,work_type,complexity,packet_digest,policy_digest,capability_digest,"
                           "requested_candidate_id,assignment_class,experiment_id,operator_decision,"
                           "reviewer_disposition,terminal_state,confirmed_at) values ({},'dk-{}','{}',1,'vp-1',"
                           "'rp-1','pr-1','anthropic','coder','medium','engine','standard','pd-{}','pol-1',"
                           "'cap-1',1,'declared_experiment',1,'confirmed','approved','completed',"
                           "'2026-08-02T00:00:00Z')",
                           id, id, work_item, id));
    exec(conn, std::format("insert into routing_dispatch_events (dispatch_id,event_id,sequence,event_kind,"
                           "attempt_number,terminal_state,payload_json,occurred_at) values ({},'ev-{}',0,"
                           "'outcome',1,'completed','{{}}','2026-08-02T00:00:00Z')",
                           id, id));
    exec(conn, std::format("insert into routing_terminal_samples (experiment_id,logical_work_item_id,role,"
                           "initial_packet_digest,candidate_id,project_id,validation_policy_version,"
                           "routing_policy_version,vendor,tier,work_type,complexity,terminal_event_id,"
                           "terminal_state,quality_success,cohort_eligible,exclusion_reason,finalized_at) values "
                           "(1,'{}','coder','pd-{}',1,1,'vp-1','rp-1','anthropic','medium','engine','standard',"
                           "'ev-{}','completed',1,0,'outside_declared_population','2026-08-04T00:00:00Z')",
                           work_item, id, id));
  }
}

/// @brief The cohort flags that select the seeded experiment.
/// @return The flag list.
auto cohort_flags() -> std::vector<std::string> {
  return {"--vendor", "anthropic", "--project", "1",      "--validation-policy", "vp-1",   "--routing-policy", "rp-1",
          "--role",   "coder",     "--tier",    "medium", "--work-type",         "engine", "--complexity",     "standard"};
}

/// @brief `models evals` with the seeded cohort plus `extra`.
/// @param fx The fixture.
/// @param extra Additional flags.
/// @return The captured invocation.
auto evals(const fixture& fx, std::vector<std::string> extra) -> invocation {
  std::vector<std::string> args{"models", "evals", "--json"};
  auto const               cohort = cohort_flags();
  args.insert(args.end(), cohort.begin(), cohort.end());
  args.insert(args.end(), extra.begin(), extra.end());
  return dispatch(fx, args);
}

// --- Ready task-100 fixture, for `models resolve`'s task-bound branch -----
//
// Reviewer finding (task 6343 iteration 2, BLOCKING 1): `complexity_tag`,
// `to_profile_facts`, `prof::compile`, `resolve_task_packet` and the
// packet-backed `--json`/text arms were unreachable from every case in this
// file — only the planning arm and the pre-packet refusals were covered.
// This is `engine/ingest/packet.t.cpp`'s own `seed()` (task 100, `pkt-ready`)
// replicated here byte-for-byte, because `complexity_tag` lives in an
// anonymous namespace in the HANDLER translation unit and cannot be pinned
// from `engine_ingest`'s own tests. The digest constants are copied verbatim
// from that file — same preimages, same task id, same body — so the SAME
// stored digests are current here. One extra fact is appended,
// `risk.explicit`, which `profile.cpp:233-234` reads directly to force
// `complexity::high_risk` — the ONE band that differs from
// `ranking::complexity_to_text`'s spelling, so it is the only band that can
// actually kill a `complexity_tag` -> `complexity_to_text` substitution.

/// preimage: `task \0 100 \0 body#acceptance-criteria \0 The packet compiles with zero readiness reasons.`
constexpr std::string_view d_acceptance = "c36d26ffec18cdac6ce40994f45fcbf6728bd65a160f0c4b9c6f29bd53c1b4d3";
/// preimage: `task \0 100 \0 next_action \0 Port compileTask and assert both readiness arms.`
constexpr std::string_view d_next_action = "a735b836c075edb69bf4ebf0fec21139638ae4cb8f5cc093540080da737a7a29";
/// preimage: `artifact \0 10 \0 artifact:10#Overview \0 Spec section body for product_spec.`
constexpr std::string_view d_product = "7294d414ca5de50586d1bbe24cff9820566fc37dae93a6f75a5005b3948b8b27";
/// preimage: `artifact \0 11 \0 artifact:11#Overview \0 Spec section body for tech_spec.`
constexpr std::string_view d_tech = "ec43ff315eae393c7b9a6aa5cdb1e83b8c958c5917f3d3b12cec1bc5d6aac294";
/// preimage: `artifact \0 12 \0 artifact:12#Overview \0 Spec section body for roadmap.`
constexpr std::string_view d_roadmap = "fdd2302595324aaeaee4ecd136fbb2030178781f341d9bf9d88a8b560da197ad";
/// preimage: `artifact \0 13 \0 artifact:13#Overview \0 Spec section body for test_spec.`
constexpr std::string_view d_test_spec = "6fbf86941cd9618fbbd981162cad4e957b3e735e08a82ff465f6de627940368f";

/// @brief Seed a READY task 100 on plan 1, plus a `risk.explicit` fact that
/// forces `complexity::high_risk`. `seed_project(fx)` must run first — it is
/// what creates project id 1 that `init` registers for the fixture's cwd; a
/// second `projects` row is neither needed nor safe to insert (every entity
/// below is `scope_kind='global'`, so nothing here carries an FK to it).
/// @param fx The fixture.
auto seed_ready_task(const fixture& fx) -> void {
  auto conn = open_db(fx);
  exec(conn, "insert into plans (id, scope_kind, scope_id, title, slug, summary, status) "
             "values (1, 'global', null, 'Packet plan', 'packet-plan', 'Summary.', 'active')");
  exec(conn, "insert into tasks (id, scope_kind, scope_id, plan_id, title, body, status, priority, next_action, slug) "
             "values (100, 'global', null, 1, 'Compile the routing packet',"
             "'Implement the packet compiler.\n"
             "\n"
             "## Acceptance Criteria\n"
             "The packet compiles with zero readiness reasons.\n"
             "\n"
             "## Required validation\n"
             "cmake --build build/debug\n"
             "', 'doing', 100, 'Port compileTask and assert both readiness arms.', 'pkt-ready')");
  exec(conn, "insert into tasks (id, scope_kind, scope_id, plan_id, title, body, status, priority, slug) "
             "values (101, 'global', null, 1, 'Dependency', 'Body.', 'done', 100, 'pkt-dep')");
  exec(conn, "insert into artifacts (id, scope_kind, scope_id, kind, title, body, status) values "
             "(10, 'global', null, 'product_spec', 'Product', '## Overview\nSpec section body for product_spec.\n', 'active'),"
             "(11, 'global', null, 'tech_spec', 'Tech', '## Overview\nSpec section body for tech_spec.\n', 'active'),"
             "(12, 'global', null, 'roadmap', 'Roadmap', '## Overview\nSpec section body for roadmap.\n', 'active'),"
             "(13, 'global', null, 'test_spec', 'Tests', '## Overview\nSpec section body for test_spec.\n', 'active')");
  exec(conn, "insert into decisions (id, scope_kind, scope_id, title, body, status) "
             "values (20, 'global', null, 'Locked', 'Decision body.', 'accepted')");
  exec(conn, "insert into test_scenarios (id, scope_kind, scope_id, title, body, status) "
             "values (30, 'global', null, 'Scenario', 'Scenario body.', 'ready')");
  exec(conn, "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values "
             "('task', 100, 'artifact', 10, 'cites'),"
             "('task', 100, 'artifact', 11, 'cites'),"
             "('task', 100, 'artifact', 12, 'cites'),"
             "('task', 100, 'artifact', 13, 'cites'),"
             "('task', 100, 'decision', 20, 'cites'),"
             "('task', 100, 'task', 101, 'depends-on'),"
             "('test_scenario', 30, 'task', 100, 'verifies'),"
             "('test_scenario', 30, 'plan', 1, 'derives-from')");
  exec(conn, "insert into task_touch_paths (task_id, repo_id, path) "
             "values (100, 1, 'src/lib/engine/ingest/packet.cpp')");
  exec(conn, std::format("insert into routing_task_facts (task_id, fact_kind, value_type, value_bool, value_text, "
                         "source_entity_kind, source_entity_id, source_locator, source_digest, materializer_version) values "
                         "(100,'acceptance_complete','bool',1,null,'task',100,'body#acceptance-criteria','{}','spec-ingest-v1'),"
                         "(100,'next_action_exact','bool',1,null,'task',100,'next_action','{}','spec-ingest-v1'),"
                         "(100,'cited_artifact_section','text',null,'Spec section body for product_spec.','artifact',10,"
                         "'artifact:10#Overview','{}','spec-ingest-v1'),"
                         "(100,'cited_artifact_section','text',null,'Spec section body for tech_spec.','artifact',11,"
                         "'artifact:11#Overview','{}','spec-ingest-v1'),"
                         "(100,'cited_artifact_section','text',null,'Spec section body for roadmap.','artifact',12,"
                         "'artifact:12#Overview','{}','spec-ingest-v1'),"
                         "(100,'cited_artifact_section','text',null,'Spec section body for test_spec.','artifact',13,"
                         "'artifact:13#Overview','{}','spec-ingest-v1'),"
                         // The forcing fact. Reuses `next_action`'s locator/digest —
                         // `source_digest` is a hash of (source_kind, source_id,
                         // locator, semantic-text), which does not depend on
                         // `fact_kind` at all, so `d_next_action` is exactly the
                         // digest a live re-derivation of THIS row also produces.
                         "(100,'risk.explicit','bool',1,null,'task',100,'next_action','{}','spec-ingest-v1')",
                         d_acceptance, d_next_action, d_product, d_tech, d_roadmap, d_test_spec, d_next_action));
}

} // namespace

TEST_CASE("models registry list disagrees with itself about the empty case", "[cmd][models]") {
  auto const fx = make_fixture("empty");
  seed_project(fx);

  auto const text = dispatch(fx, {"models", "registry", "list"});
  CHECK(text.code == 0);
  // ZERO BYTES, not a "no candidates" sentence and not a bare newline.
  CHECK(text.out.empty());
  CHECK(text.err.empty());

  auto const json = dispatch(fx, {"models", "registry", "list", "--json"});
  CHECK(json.code == 0);
  CHECK(json.out == R"({"registry_version":1,"candidates":[],)"
                    R"("migration_warning":"legacy catalog compatibility is one-window and non-authoritative"})"
                    "\n");
}

TEST_CASE("models registry export writes BOTH streams, and --json mutes the warning", "[cmd][models]") {
  auto const fx = make_fixture("export");
  seed_project(fx);
  REQUIRE(dispatch(fx, {"models", "registry", "add", "--vendor", "claude", "--id", "opus-x", "--order", "1"}).code == 0);

  auto const plain = dispatch(fx, {"models", "registry", "export"});
  CHECK(plain.code == 0);
  CHECK(plain.err == "warning: legacy catalog compatibility is one-window and non-authoritative\n");

  auto const flagged = dispatch(fx, {"models", "registry", "export", "--json"});
  CHECK(flagged.code == 0);
  // The INVERSE of every other leaf: `--json` changes stderr, not stdout.
  CHECK(flagged.err.empty());
  CHECK(flagged.out == plain.out);
  // ...and it is the same envelope `list --json` prints.
  CHECK(flagged.out == dispatch(fx, {"models", "registry", "list", "--json"}).out);
}

TEST_CASE("models registry add writes the row, echoes the id, and refuses a duplicate at exit 3", "[cmd][models]") {
  auto const fx = make_fixture("add");
  seed_project(fx);

  auto const first = dispatch(fx, {"models", "registry", "add", "--vendor", "claude", "--id", "opus-x", "--order", "1"});
  CHECK(first.code == 0);
  CHECK(first.out == "1\n");
  auto const disabled =
      dispatch(fx, {"models", "registry", "add", "--vendor", "claude", "--id", "sonnet-x", "--order", "2", "--disabled"});
  CHECK(disabled.code == 0);
  CHECK(disabled.out == "2\n");

  auto const dup = dispatch(fx, {"models", "registry", "add", "--vendor", "claude", "--id", "opus-x", "--order", "9"});
  // Exit THREE. The `bench` family reports its own UNIQUE violation at
  // exit 6; neither is normalized to the other.
  CHECK(dup.code == 3);
  CHECK(dup.err == "error: registering opaque candidate: Conflict\n");

  for (auto const& args :
       {std::vector<std::string>{"models", "registry", "add", "--vendor", "", "--id", "x", "--order", "1"},
        std::vector<std::string>{"models", "registry", "add", "--vendor", "claude", "--id", "", "--order", "1"}}) {
    auto const empty = dispatch(fx, args);
    CHECK(empty.code == 2);
    CHECK(empty.err == "error: registering opaque candidate: InvalidValue\n");
  }

  auto conn = open_db(fx);
  // `--disabled` is the NEGATIVE form and the stored column is `enabled`:
  // a port that wrote the flag straight through would store 0 for the
  // ENABLED candidate, and `registry list --json` would still be
  // well-formed JSON. The duplicate did NOT bump the original's order to 9.
  CHECK(registry_rows(conn) == "1|claude|opus-x|1|1|1|native\n"
                               "2|claude|sonnet-x|0|2|1|native");
}

TEST_CASE("models registry bind and unbind print nothing and are proven by rows", "[cmd][models]") {
  auto const fx = make_fixture("bind");
  seed_project(fx);
  REQUIRE(dispatch(fx, {"models", "registry", "add", "--vendor", "claude", "--id", "opus-x", "--order", "1"}).code == 0);

  auto const bound = dispatch(fx, {"models", "registry", "bind", "--candidate", "1", "--role", "coder", "--tier", "medium"});
  CHECK(bound.code == 0);
  // ZERO BYTES on success — the only evidence the verb did anything is the
  // row, which is why this file exists.
  CHECK(bound.out.empty());
  CHECK(bound.err.empty());

  // Idempotent: a repeat succeeds and does NOT create a second row.
  CHECK(dispatch(fx, {"models", "registry", "bind", "--candidate", "1", "--role", "coder", "--tier", "medium"}).code == 0);
  CHECK(dispatch(fx, {"models", "registry", "bind", "--candidate", "1", "--role", "reviewer", "--tier", "large"}).code == 0);

  auto const missing = dispatch(fx, {"models", "registry", "bind", "--candidate", "99", "--role", "coder", "--tier", "medium"});
  // Exit 1 / QueryFailed. `observe` reports the SAME missing candidate at
  // exit 3 / Conflict — see this file's header.
  CHECK(missing.code == 1);
  CHECK(missing.err == "error: binding candidate: QueryFailed\n");

  auto const bad_tier = dispatch(fx, {"models", "registry", "bind", "--candidate", "1", "--role", "coder", "--tier", "bogus"});
  CHECK(bad_tier.code == 2);
  CHECK(bad_tier.err == "error: invalid tier: bogus\n");
  CHECK_FALSE(bad_tier.db_open);

  // Unbinding something never bound succeeds silently and removes nothing.
  CHECK(dispatch(fx, {"models", "registry", "unbind", "--candidate", "1", "--role", "nosuch", "--tier", "medium"}).code == 0);
  {
    auto conn = open_db(fx);
    CHECK(binding_rows(conn) == "1|coder|medium\n"
                                "1|reviewer|large");
  }

  CHECK(dispatch(fx, {"models", "registry", "unbind", "--candidate", "1", "--role", "reviewer", "--tier", "large"}).code == 0);
  auto conn = open_db(fx);
  // ONE binding removed, the other SURVIVING — an unbind that dropped every
  // row for the candidate would also have left the named one gone.
  CHECK(binding_rows(conn) == "1|coder|medium");
}

TEST_CASE("models registry observe appends versioned rows and validates its enums", "[cmd][models]") {
  auto const fx = make_fixture("observe");
  seed_project(fx);
  REQUIRE(dispatch(fx, {"models", "registry", "add", "--vendor", "claude", "--id", "opus-x", "--order", "1"}).code == 0);

  auto const written = dispatch(fx, {"models", "registry", "observe", "--candidate", "1", "--host", "h1", "--version", "1",
                                     "--availability", "available", "--spawn-verification", "verified", "--evidence-ref", "ev1",
                                     "--captured-at", "2026-08-01T00:00:00Z", "--expires-at", "2026-12-01T00:00:00Z"});
  CHECK(written.code == 0);
  CHECK(written.out == "1\n");

  auto const orphan = dispatch(fx, {"models", "registry", "observe", "--candidate", "99", "--host", "h", "--version", "1",
                                    "--availability", "available", "--spawn-verification", "verified", "--evidence-ref", "e",
                                    "--captured-at", "a", "--expires-at", "b"});
  // Exit THREE for a missing candidate, where `bind` gives exit 1.
  CHECK(orphan.code == 3);
  CHECK(orphan.err == "error: recording host observation: Conflict\n");

  auto const bad_avail = dispatch(fx, {"models", "registry", "observe", "--candidate", "1", "--host", "h", "--version", "1",
                                       "--availability", "bogus", "--spawn-verification", "verified", "--evidence-ref", "e",
                                       "--captured-at", "a", "--expires-at", "b"});
  CHECK(bad_avail.code == 2);
  CHECK(bad_avail.err == "error: invalid availability: bogus\n");
  CHECK_FALSE(bad_avail.db_open);

  auto const bad_spawn = dispatch(fx, {"models", "registry", "observe", "--candidate", "1", "--host", "h", "--version", "1",
                                       "--availability", "available", "--spawn-verification", "bogus", "--evidence-ref", "e",
                                       "--captured-at", "a", "--expires-at", "b"});
  CHECK(bad_spawn.code == 2);
  CHECK(bad_spawn.err == "error: invalid spawn verification: bogus\n");

  // `expires_at` must sort strictly AFTER `captured_at`, BYTEWISE — an
  // observation that expires before it was captured is refused rather than
  // stored as permanently stale.
  auto const inverted = dispatch(fx, {"models", "registry", "observe", "--candidate", "1", "--host", "h", "--version", "1",
                                      "--availability", "available", "--spawn-verification", "verified", "--evidence-ref", "e",
                                      "--captured-at", "z", "--expires-at", "a"});
  CHECK(inverted.code != 0);

  auto conn = open_db(fx);
  // Exactly the one accepted row. Four refusals wrote nothing.
  CHECK(observation_rows(conn) == "1|h1|1|available|verified|ev1|2026-08-01T00:00:00Z|2026-12-01T00:00:00Z");
}

TEST_CASE("models registry eligibility scopes to ONE host and ONE role/tier binding", "[cmd][models]") {
  auto const fx = make_fixture("elig");
  seed_project(fx);
  REQUIRE(dispatch(fx, {"models", "registry", "add", "--vendor", "claude", "--id", "opus-x", "--order", "1"}).code == 0);
  REQUIRE(dispatch(fx, {"models", "registry", "bind", "--candidate", "1", "--role", "coder", "--tier", "medium"}).code == 0);
  // h1 says available+verified at version 1; h2 says the OPPOSITE at the
  // HIGHER version 5. A resolver that took the newest observation overall
  // would answer h1's question with h2's evidence.
  REQUIRE(dispatch(fx, {"models", "registry", "observe", "--candidate", "1", "--host", "h1", "--version", "1", "--availability",
                        "available", "--spawn-verification", "verified", "--evidence-ref", "ev1", "--captured-at",
                        "2026-08-01T00:00:00Z", "--expires-at", "2026-12-01T00:00:00Z"})
              .code == 0);
  REQUIRE(dispatch(fx, {"models", "registry", "observe", "--candidate", "1", "--host", "h2", "--version", "5", "--availability",
                        "unavailable", "--spawn-verification", "unverified", "--evidence-ref", "ev2", "--captured-at",
                        "2026-08-02T00:00:00Z", "--expires-at", "2026-12-02T00:00:00Z"})
              .code == 0);

  auto const permissive = std::vector<std::string>{"--override-supported", "--policy-permits"};
  auto       ask        = [&](std::string_view host, std::string_view tier, std::string_view now) {
    return dispatch(fx, {"models", "registry", "eligibility", "--candidate", "1", "--host", std::string{host}, "--role", "coder",
                         "--tier", std::string{tier}, "--now", std::string{now}, permissive[0], permissive[1]});
  };

  auto const good = ask("h1", "medium", "2026-08-20T00:00:00Z");
  CHECK(good.code == 0);
  CHECK(good.out == R"({"candidate":1,"host":"h1","eligible":true,"gates":{"cli_available":true,)"
                    R"("exact_spawn_verified":true,"role_tier_bound":true,"role_surface_override_supported":true,)"
                    R"("host_policy_permits":true,"observation_fresh":true},"reasons":[]})"
                    "\n");

  // SAME candidate, SAME instant, DIFFERENT host. h2's own verdicts apply.
  auto const other_host = ask("h2", "medium", "2026-08-20T00:00:00Z");
  CHECK(other_host.code == 0);
  CHECK(other_host.out.find(R"("host":"h2")") != std::string::npos);
  CHECK(other_host.out.find(R"("cli_available":false)") != std::string::npos);
  CHECK(other_host.out.find(R"("exact_spawn_verified":false)") != std::string::npos);
  CHECK(other_host.out.find(R"("provider_cli_unavailable")") != std::string::npos);

  // A host with NO observation: the gates fail rather than borrowing one.
  auto const unknown_host = ask("nosuchhost", "medium", "2026-08-20T00:00:00Z");
  CHECK(unknown_host.code == 0);
  CHECK(unknown_host.out.find(R"("observation_fresh":false)") != std::string::npos);
  CHECK(unknown_host.out.find(R"("host_observation_expired")") != std::string::npos);

  // SAME host, SAME observation, a tier that is NOT bound. Only the
  // binding gate moves — everything else stays true, which is what makes
  // this an exclusion proof rather than a coincidence.
  auto const wrong_tier = ask("h1", "large", "2026-08-20T00:00:00Z");
  CHECK(wrong_tier.code == 0);
  CHECK(wrong_tier.out.find(R"("cli_available":true)") != std::string::npos);
  CHECK(wrong_tier.out.find(R"("role_tier_bound":false)") != std::string::npos);
  CHECK(wrong_tier.out.find(R"("role_tier_not_bound")") != std::string::npos);

  // Freshness is a BYTE comparison of `now` against `expires_at`.
  auto const stale = ask("h1", "medium", "2027-08-20T00:00:00Z");
  CHECK(stale.out.find(R"("observation_fresh":false)") != std::string::npos);

  auto const missing = dispatch(fx, {"models", "registry", "eligibility", "--candidate", "99", "--host", "h1", "--role", "coder",
                                     "--tier", "medium", "--now", "t"});
  CHECK(missing.code == 1);
  CHECK(missing.err == "error: reading candidate: NotFound\n");
}

TEST_CASE("models registry verify-identity compares both halves without aliasing", "[cmd][models]") {
  auto const fx = make_fixture("verify");
  seed_project(fx);
  REQUIRE(dispatch(fx, {"models", "registry", "add", "--vendor", "claude", "--id", "opus-x", "--order", "1"}).code == 0);

  auto ask = [&](std::string_view vendor, std::string_view id) {
    return dispatch(fx, {"models", "registry", "verify-identity", "--candidate", "1", "--actual-vendor", std::string{vendor},
                         "--actual-id", std::string{id}});
  };

  CHECK(ask("claude", "opus-x").out == R"({"candidate":1,"identity":"matched"})"
                                       "\n");
  CHECK(ask("claude", "other").out == R"({"candidate":1,"identity":"candidate_mismatch"})"
                                      "\n");
  // Vendor is checked BEFORE candidate, so a value differing in both halves
  // reports the VENDOR mismatch.
  CHECK(ask("openai", "other").out == R"({"candidate":1,"identity":"vendor_mismatch"})"
                                      "\n");
  // Every verdict is exit 0 — a mismatch is an ANSWER, not a failure.
  CHECK(ask("openai", "other").code == 0);

  auto const missing =
      dispatch(fx, {"models", "registry", "verify-identity", "--candidate", "99", "--actual-vendor", "v", "--actual-id", "i"});
  // The one leaf in the family that names the id instead of using the
  // `<gerund>: <ErrorName>` shape. Routing this through the host-scoped
  // read with an empty host made EVERY verify-identity report this, at
  // exit 1, for candidates that exist — caught by the differential harness.
  CHECK(missing.code == 1);
  CHECK(missing.err == "error: candidate 99 not found\n");
}

TEST_CASE("models registry update and remove are proven only by rows", "[cmd][models]") {
  auto const fx = make_fixture("update");
  seed_project(fx);
  REQUIRE(dispatch(fx, {"models", "registry", "add", "--vendor", "claude", "--id", "opus-x", "--order", "1"}).code == 0);
  REQUIRE(dispatch(fx, {"models", "registry", "add", "--vendor", "claude", "--id", "sonnet-x", "--order", "2"}).code == 0);

  auto const bumped = dispatch(fx, {"models", "registry", "update", "--candidate", "1", "--order", "7"});
  CHECK(bumped.code == 0);
  CHECK(bumped.out.empty());
  {
    auto conn = open_db(fx);
    // `--disabled` absent means ENABLED — the flag is read as `!--disabled`
    // on every call, so an update that omits it re-enables a disabled row.
    // `registration_version` is bumped by the UPDATE — 2 here, 1 on the
    // untouched row. Nothing on stdout reports it (the leaf prints zero
    // bytes) and `registry list --json` shows it only if the row is right.
    CHECK(registry_rows(conn) == "1|claude|opus-x|1|7|2|native\n"
                                 "2|claude|sonnet-x|1|2|1|native");
  }
  CHECK(dispatch(fx, {"models", "registry", "update", "--candidate", "1", "--order", "8", "--disabled"}).code == 0);

  auto const nothing = dispatch(fx, {"models", "registry", "update", "--candidate", "99", "--order", "3"});
  CHECK(nothing.code == 1);
  CHECK(nothing.err == "error: updating candidate: NotFound\n");

  auto const gone = dispatch(fx, {"models", "registry", "remove", "--candidate", "99"});
  CHECK(gone.code == 1);
  CHECK(gone.err == "error: removing candidate: NotFound\n");

  auto const removed = dispatch(fx, {"models", "registry", "remove", "--candidate", "2"});
  CHECK(removed.code == 0);
  CHECK(removed.out.empty());

  auto conn = open_db(fx);
  // Candidate 1 SURVIVES with the disabled update applied, candidate 2 is
  // gone. Asserting only that `show` now fails would also pass if `remove`
  // had corrupted the row rather than deleting it, and asserting only the
  // count would pass if it had deleted the wrong one.
  CHECK(registry_rows(conn) == "1|claude|opus-x|0|8|3|native");
}

TEST_CASE("models evals selects its branch on a NON-EMPTY --vendor alone", "[cmd][models]") {
  auto const fx = make_fixture("branch");
  seed_project(fx);

  // Every cohort flag EXCEPT `--vendor`, one at a time. All fall through to
  // the legacy scorecard at exit 0. A port that triggered the cohort branch
  // on "any cohort flag present" would turn each of these into an exit-2
  // refusal.
  for (auto const& flag : std::vector<std::vector<std::string>>{{"--role", "r"},
                                                                {"--tier", "medium"},
                                                                {"--project", "1"},
                                                                {"--work-type", "engine"},
                                                                {"--complexity", "standard"},
                                                                {"--validation-policy", "v"},
                                                                {"--routing-policy", "r"},
                                                                {"--min-samples", "3"},
                                                                {"--quality-floor", "0.9"},
                                                                {"--vendor", ""}}) {
    std::vector<std::string> args{"models", "evals", "--json"};
    args.insert(args.end(), flag.begin(), flag.end());
    auto const fell_through = dispatch(fx, args);
    INFO("flag: " << flag[0] << " " << flag[1]);
    CHECK(fell_through.code == 0);
    CHECK(fell_through.out == R"({"scorecard":[],"recommendations":[],"signals_sourced":{"reviewer_disposition":true,)"
                              R"("iteration_count":true,"quality_gate_pass_fail":false,"test_coder_expansion":true},)"
                              R"("legacy_dispatch_notes_skipped":0})"
                              "\n");
  }

  // ...and `--vendor` on its own DOES select it.
  auto const selected = dispatch(fx, {"models", "evals", "--json", "--vendor", "v"});
  CHECK(selected.code == 2);
  CHECK(selected.err == "error: --project is required when ranking a cohort\n");
}

TEST_CASE("models evals cohort validation fires in the oracle's exact ORDER", "[cmd][models]") {
  auto const fx = make_fixture("order");
  seed_project(fx);

  // Each probe satisfies every earlier check and violates exactly one
  // later, so no single probe could distinguish the orderings. All nine
  // exit 2 with empty stdout — an exit-code assertion sees nothing here.
  struct probe {
    std::vector<std::string> extra;
    std::string              message;
  };
  std::vector<probe> const probes{
      {{}, "--project is required when ranking a cohort\n"},
      {{"--project", "x"}, "invalid --project 'x': expected integer\n"},
      {{"--project", "1", "--min-samples", "x"}, "invalid --min-samples 'x'\n"},
      {{"--project", "1", "--quality-floor", "y"}, "invalid --quality-floor 'y'\n"},
      // `--min-samples` beats `--quality-floor` when BOTH are malformed.
      {{"--project", "1", "--min-samples", "x", "--quality-floor", "y"}, "invalid --min-samples 'x'\n"},
      {{"--project", "1"}, "--validation-policy is required when ranking a cohort\n"},
      {{"--project", "1", "--validation-policy", "vp"}, "--routing-policy is required when ranking a cohort\n"},
      {{"--project", "1", "--validation-policy", "vp", "--routing-policy", "rp"}, "--role is required when ranking a cohort\n"},
      {{"--project", "1", "--validation-policy", "vp", "--routing-policy", "rp", "--role", "r"},
       "--tier is required when ranking a cohort\n"},
      // required BEFORE valid, and both messages are live for all three enums.
      {{"--project", "1", "--validation-policy", "vp", "--routing-policy", "rp", "--role", "r", "--tier", "bogus"},
       "invalid --tier 'bogus'\n"},
      {{"--project", "1", "--validation-policy", "vp", "--routing-policy", "rp", "--role", "r", "--tier", "medium"},
       "--work-type is required when ranking a cohort\n"},
      {{"--project", "1", "--validation-policy", "vp", "--routing-policy", "rp", "--role", "r", "--tier", "medium", "--work-type",
        "bogus"},
       "invalid --work-type 'bogus'\n"},
      {{"--project", "1", "--validation-policy", "vp", "--routing-policy", "rp", "--role", "r", "--tier", "medium", "--work-type",
        "engine"},
       "--complexity is required when ranking a cohort\n"},
      {{"--project", "1", "--validation-policy", "vp", "--routing-policy", "rp", "--role", "r", "--tier", "medium", "--work-type",
        "engine", "--complexity", "bogus"},
       "invalid --complexity 'bogus'\n"},
  };
  for (auto const& one : probes) {
    std::vector<std::string> args{"models", "evals", "--json", "--vendor", "v"};
    args.insert(args.end(), one.extra.begin(), one.extra.end());
    auto const refused = dispatch(fx, args);
    INFO("probe: " << one.message);
    CHECK(refused.code == 2);
    CHECK(refused.out == planar::cmd::testsupport::json_error_envelope_line("models evals", "invalid_input"));
    CHECK(refused.err == "error: " + one.message);
  }
}

TEST_CASE("models evals --quality-floor honours Zig's float contract", "[cmd][models]") {
  auto const fx = make_fixture("float");
  seed_cohort(fx);

  auto floor_of = [&](std::string_view raw) -> std::string {
    auto const outcome = evals(fx, {"--quality-floor", std::string{raw}});
    if (outcome.code != 0) {
      return std::format("REFUSED:{}", outcome.err);
    }
    auto const key   = std::string_view{R"("quality_floor":)"};
    auto const start = outcome.out.find(key);
    REQUIRE(start != std::string::npos);
    auto const from = start + key.size();
    return outcome.out.substr(from, outcome.out.find('}', from) - from);
  };

  // The gate is echoed back in the payload, so the parse is observable.
  CHECK(floor_of("0.5") == "0.5");
  CHECK(floor_of("1e-1") == "0.1");
  CHECK(floor_of(".5") == "0.5");
  CHECK(floor_of("5.") == "5");
  CHECK(floor_of("+0.5") == "0.5");
  CHECK(floor_of("-0.5") == "-0.5");
  // Underscores are legal only BETWEEN DIGITS, and the digit class depends
  // on the literal's base. `1_e2` is refused because `e` is not a DECIMAL
  // digit — applying the hex class to a decimal literal accepts it and
  // parses 100, a gate value invented out of thin air at exit 0.
  CHECK(floor_of("0_5.0") == "5");
  CHECK(floor_of("_5").starts_with("REFUSED:"));
  CHECK(floor_of("5_").starts_with("REFUSED:"));
  CHECK(floor_of("0.5_").starts_with("REFUSED:"));
  CHECK(floor_of("0._5").starts_with("REFUSED:"));
  CHECK(floor_of("1_e2") == "REFUSED:error: invalid --quality-floor '1_e2'\n");
  // Hex float literals with a `p` exponent.
  CHECK(floor_of("0x1p-1") == "0.5");
  CHECK(floor_of("0X1P-1") == "0.5");
  // NON-FINITE VALUES ARE REFUSED (task 6186; decision 1090 authorises the
  // rewrite of this pin -- 6186 is one of the eight rows it names, and it
  // is NOT among 1067's nine). The oracle ACCEPTED all four and echoed them
  // ASYMMETRICALLY — bare `inf` / `-inf`, quoted `"nan"` with the sign
  // dropped. The quoted arm is valid JSON of the wrong TYPE; the bare arm
  // is not JSON at all, so `models evals --json`, whose entire purpose is
  // machine-readable routing advice, handed its consumer a parse error.
  //
  // Refusal rather than encoding, because neither value is a meaningful
  // gate: at `inf` every candidate is below the floor and nothing can ever
  // be recommended, and at `nan` every comparison is false so the gate
  // silently never fires. Both are answers the operator would read as
  // "your cohort is empty" rather than "your floor is nonsense". The
  // wording is the parse refusal this flag already had, reused verbatim
  // rather than invented — no new pinned byte.
  CHECK(floor_of("inf") == "REFUSED:error: invalid --quality-floor 'inf'\n");
  CHECK(floor_of("INF") == "REFUSED:error: invalid --quality-floor 'INF'\n");
  CHECK(floor_of("Infinity") == "REFUSED:error: invalid --quality-floor 'Infinity'\n");
  CHECK(floor_of("-inf") == "REFUSED:error: invalid --quality-floor '-inf'\n");
  CHECK(floor_of("nan") == "REFUSED:error: invalid --quality-floor 'nan'\n");
  CHECK(floor_of("NaN") == "REFUSED:error: invalid --quality-floor 'NaN'\n");
  CHECK(floor_of("-nan") == "REFUSED:error: invalid --quality-floor '-nan'\n");
  // Trailing garbage, a comma decimal, and a leading space are all refused.
  CHECK(floor_of("1,5").starts_with("REFUSED:"));
  CHECK(floor_of("0.5abc").starts_with("REFUSED:"));
  CHECK(floor_of(" 0.5").starts_with("REFUSED:"));
}

TEST_CASE("models evals --json emits a PARSEABLE document for every accepted gate", "[cmd][models][json]") {
  // Task 6186 asked for this shape explicitly: assert the payload PARSES,
  // not that it contains an expected substring. The old bare-`inf` output
  // would have satisfied any substring check.
  auto const fx = make_fixture("floatparse");
  seed_cohort(fx);

  for (auto const& raw : {"0.5", "0", "1", "-0.5", "0x1p-1", "1e-1", "0.000001375"}) {
    INFO("--quality-floor " << raw);
    auto const outcome = evals(fx, {"--quality-floor", std::string{raw}});
    REQUIRE(outcome.code == 0);
    // Glaze is this binary's JSON READER (see CMakeLists' note that
    // `planar.json_text` is emit-only); `validate_json` is a parse, not a
    // substring probe.
    CHECK(glz::validate_json(outcome.out).ec == glz::error_code::none);
  }

  // And the float that used to arrive in scientific notation now arrives
  // fixed, through the same shared formatter as every other float site.
  auto const small = evals(fx, {"--quality-floor", "0.000001375"});
  REQUIRE(small.code == 0);
  CHECK(small.out.find(R"("quality_floor":0.000001375)") != std::string::npos);
  CHECK(small.out.find("e-06") == std::string::npos);
}

TEST_CASE("models evals --min-samples is UNSIGNED", "[cmd][models]") {
  auto const fx = make_fixture("minsamples");
  seed_cohort(fx);

  auto samples_of = [&](std::string_view raw) -> std::string {
    auto const outcome = evals(fx, {"--min-samples", std::string{raw}});
    if (outcome.code != 0) {
      return std::format("REFUSED:{}", outcome.err);
    }
    auto const key   = std::string_view{R"("minimum_samples":)"};
    auto const start = outcome.out.find(key) + key.size();
    return outcome.out.substr(start, outcome.out.find(',', start) - start);
  };

  CHECK(samples_of("3") == "3");
  CHECK(samples_of("+5") == "5");
  CHECK(samples_of("0") == "0");
  CHECK(samples_of("1_0") == "10");
  // The reason a signed parser is wrong here: `-1` would convert to a huge
  // unsigned minimum and gate every candidate as `insufficient_data` at
  // exit 0 with a perfectly plausible payload.
  CHECK(samples_of("-1") == "REFUSED:error: invalid --min-samples '-1'\n");
  CHECK(samples_of("x").starts_with("REFUSED:"));
  CHECK(samples_of("0x10").starts_with("REFUSED:"));
  CHECK(samples_of("5e2").starts_with("REFUSED:"));
  CHECK(samples_of("_5").starts_with("REFUSED:"));
}

TEST_CASE("models evals reproduces the oracle's Wilson bounds digit for digit", "[cmd][models]") {
  auto const fx = make_fixture("wilson");
  seed_cohort(fx);

  auto const ranked = evals(fx, {});
  CHECK(ranked.code == 0);
  // The WHOLE payload, byte for byte. `wilson_lower` is printed in its
  // shortest round-trippable form, so the last digit of each bound is a
  // stdout byte — `0.38658150076225317` is the value clang's default
  // `-ffp-contract=on` moves by one ULP to ...312.
  //
  // `cand-3` is the row this module exists for: 3-for-3, raw rate 1, and it
  // ranks NOWHERE. It carries `insufficient_data:true` AND
  // `below_quality_floor:false` — the sample gate short-circuits the
  // quality gate, even though 0.4385 genuinely is under the 0.5 floor.
  // "We declined to judge" is not "we judged it inadequate".
  CHECK(
      ranked.out ==
      R"({"version":"routing-ranking-v1","evidence":"declared_experiment","gates":{"minimum_samples":5,"quality_floor":0.5},"rows":[)"
      R"({"candidate_id":1,"candidate":"cand-1","vendor":"anthropic","fallback_order":1,"samples":20,"successes":16,)"
      R"("gate_failures":4,"excess_attempts":0,"mean_latency_ms":null,"mean_cost_micros":null,"measured_samples":0,)"
      R"("raw_rate":0.8,"wilson_lower":0.5839825677481064,"gate_failure_rate":0.2,"expected_excess_iterations":0,)"
      R"("insufficient_data":false,"below_quality_floor":false,"rank":1},)"
      R"({"candidate_id":2,"candidate":"cand-2","vendor":"anthropic","fallback_order":2,"samples":20,"successes":12,)"
      R"("gate_failures":8,"excess_attempts":0,"mean_latency_ms":null,"mean_cost_micros":null,"measured_samples":0,)"
      R"("raw_rate":0.6,"wilson_lower":0.38658150076225317,"gate_failure_rate":0.4,"expected_excess_iterations":0,)"
      R"("insufficient_data":false,"below_quality_floor":true,"rank":null},)"
      R"({"candidate_id":4,"candidate":"cand-4","vendor":"anthropic","fallback_order":4,"samples":20,"successes":4,)"
      R"("gate_failures":16,"excess_attempts":0,"mean_latency_ms":null,"mean_cost_micros":null,"measured_samples":0,)"
      R"("raw_rate":0.2,"wilson_lower":0.08065766257979808,"gate_failure_rate":0.8,"expected_excess_iterations":0,)"
      R"("insufficient_data":false,"below_quality_floor":true,"rank":null},)"
      R"({"candidate_id":3,"candidate":"cand-3","vendor":"anthropic","fallback_order":3,"samples":3,"successes":3,)"
      R"("gate_failures":0,"excess_attempts":0,"mean_latency_ms":null,"mean_cost_micros":null,"measured_samples":0,)"
      R"("raw_rate":1,"wilson_lower":0.4385029682449545,"gate_failure_rate":0,"expected_excess_iterations":0,)"
      R"("insufficient_data":true,"below_quality_floor":false,"rank":null}],)"
      R"("recommended":"cand-1","no_recommendation_reason":null})"
      "\n");

  // The text form reports the same numbers at three decimals and marks the
  // gated rows by NAME rather than by omission.
  auto       text_args = std::vector<std::string>{"models", "evals"};
  auto const cohort    = cohort_flags();
  text_args.insert(text_args.end(), cohort.begin(), cohort.end());
  auto const text = dispatch(fx, text_args);
  CHECK(text.code == 0);
  CHECK(text.out.find("  1    cand-1                            20       16    0.800    0.584     0.200    0.00") !=
        std::string::npos);
  CHECK(text.out.find("  n/a  cand-3") != std::string::npos);
  CHECK(text.out.find("insufficient_data") != std::string::npos);
  CHECK(text.out.ends_with("recommended: cand-1 (preview only; writes nothing)\n"));
}

TEST_CASE("models evals reports NO RECOMMENDATION as a correct answer, two ways", "[cmd][models]") {
  auto const fx = make_fixture("norec");
  seed_cohort(fx);

  auto const gated = evals(fx, {"--quality-floor", "0.99"});
  CHECK(gated.code == 0);
  // Every row is still SHOWN — the rows are the evidence — but none is
  // ranked and the reason names the gates. It is never softened into "the
  // best of a bad set".
  CHECK(gated.out.find(R"("recommended":null)") != std::string::npos);
  CHECK(gated.out.find(R"("no_recommendation_reason":"no candidate cleared both the minimum-sample and )"
                       R"(quality-floor gates")") != std::string::npos);
  CHECK(gated.out.find(R"("rank":1)") == std::string::npos);
  CHECK(gated.out.find(R"("candidate":"cand-1")") != std::string::npos);

  // A DIFFERENT reason for a genuinely different situation: no evidence at
  // all, rather than evidence that was gated. Collapsing the two would make
  // an unrun cohort look like a failed one.
  auto       args   = std::vector<std::string>{"models", "evals", "--json"};
  auto const cohort = cohort_flags();
  args.insert(args.end(), cohort.begin(), cohort.end() - 1);
  args.emplace_back("bounded");
  auto const empty = dispatch(fx, args);
  CHECK(empty.code == 0);
  CHECK(empty.out == R"({"version":"routing-ranking-v1","evidence":"declared_experiment",)"
                     R"("gates":{"minimum_samples":5,"quality_floor":0.5},"rows":[],"recommended":null,)"
                     R"("no_recommendation_reason":"no cohort-eligible declared-experiment samples"})"
                     "\n");
}

TEST_CASE("models experiments separates recorded from eligible sample counts", "[cmd][models]") {
  // TWO fixtures rather than one mutated in place, because terminal
  // samples are append-only (see `seed_cohort`): the "no exclusions" state
  // cannot be reached from the "four exclusions" state or vice versa.
  {
    auto const clean = make_fixture("experiments_clean");
    seed_cohort(clean);
    auto const json = dispatch(clean, {"models", "experiments", "--json"});
    CHECK(json.code == 0);
    CHECK(json.out == R"({"views_version":"routing-views-v1","experiments":[{"id":1,"experiment_key":"exp-1",)"
                      R"("status":"running","vendor":"anthropic","role":"coder","tier":"medium","work_type":"engine",)"
                      R"("complexity":"standard","validation_policy_version":"vp-1","routing_policy_version":"rp-1",)"
                      R"("manifest_digest":"digest-1","operator_approved_at":"2026-08-01T00:00:00Z",)"
                      R"("population_size":63,"candidate_count":4,"samples":63,"eligible_samples":63}]})"
                      "\n");
    // Both counts are reported even when they AGREE. Reporting only the
    // eligible one would understate what actually ran.
    CHECK(dispatch(clean, {"models", "experiments"}).out.find("samples: 63 recorded, 63 counted (0 excluded)") !=
          std::string::npos);
  }

  auto const fx = make_fixture("experiments");
  seed_cohort(fx, 4);
  auto const json = dispatch(fx, {"models", "experiments", "--json"});
  CHECK(json.code == 0);
  // The two counts DIVERGE: 67 declared and recorded, 63 counted. That gap
  // is the whole reason this leaf reports two numbers, and a single counter
  // could not produce it.
  CHECK(json.out.find(R"("population_size":67,"candidate_count":4,"samples":67,"eligible_samples":63)") != std::string::npos);
  CHECK(dispatch(fx, {"models", "experiments"}).out.find("samples: 67 recorded, 63 counted (4 excluded)") != std::string::npos);

  auto const empty = make_fixture("experiments_empty");
  seed_project(empty);
  CHECK(dispatch(empty, {"models", "experiments"}).out == "no declared routing experiments\n");
  CHECK(dispatch(empty, {"models", "experiments", "--json"}).out == R"({"views_version":"routing-views-v1","experiments":[]})"
                                                                    "\n");
}

TEST_CASE("models outcomes --limit EXCLUDES, and the excluded rows survive", "[cmd][models]") {
  auto const fx = make_fixture("outcomes");
  seed_cohort(fx, 1);

  auto ids_for = [&](std::vector<std::string> extra) {
    std::vector<std::string> args{"models", "outcomes", "--json"};
    args.insert(args.end(), extra.begin(), extra.end());
    auto const outcome = dispatch(fx, args);
    REQUIRE(outcome.code == 0);
    std::vector<int> ids;
    for (std::size_t at = outcome.out.find(R"({"id":)"); at != std::string::npos; at = outcome.out.find(R"({"id":)", at + 1)) {
      ids.push_back(std::stoi(outcome.out.substr(at + 6)));
    }
    return ids;
  };

  // SURVIVAL FIRST: with a limit above the row count, all 63 are present,
  // newest first. That is what makes the two capped answers below into
  // exclusions rather than an engine that returns nothing.
  auto const all = ids_for({"--limit", "1000"});
  CHECK(all.size() == 64);
  CHECK(all.front() == 64);
  CHECK(all.back() == 1);

  auto const two = ids_for({"--limit", "2"});
  CHECK(two == std::vector<int>{64, 63});

  // The DEFAULT is 50, not "everything" — asserting the flag alone would
  // miss a handler that ignored the default and returned all 63.
  auto const defaulted = ids_for({});
  CHECK(defaulted.size() == 50);
  CHECK(defaulted.front() == 64);
  CHECK(defaulted.back() == 15);
  // Row 14 EXISTS (it is in `all`) and is absent here. Both halves matter:
  // the presence is what makes the absence an exclusion.
  CHECK(std::ranges::find(all, 14) != all.end());
  CHECK(std::ranges::find(defaulted, 14) == defaulted.end());

  CHECK(ids_for({"--limit", "1_0"}).size() == 10);

  // Two distinct refusals with different wording, both before SQLite opens.
  auto const unparseable = dispatch(fx, {"models", "outcomes", "--limit", "x"});
  CHECK(unparseable.code == 2);
  CHECK(unparseable.err == "error: invalid --limit 'x': expected integer\n");
  CHECK_FALSE(unparseable.db_open);
  for (auto const* value : {"0", "-1"}) {
    auto const nonpositive = dispatch(fx, {"models", "outcomes", "--limit", value});
    CHECK(nonpositive.code == 2);
    CHECK(nonpositive.err == "error: --limit must be positive\n");
  }

  // Excluded samples are SHOWN, with a NAMED reason — hiding them would
  // make the evidence look thinner than it is, and showing them without a
  // reason would look like a bug.
  auto const with_excluded = dispatch(fx, {"models", "outcomes", "--limit", "1", "--json"});
  CHECK(with_excluded.out.find(R"("counts_toward_recommendation":false)") != std::string::npos);
  CHECK(with_excluded.out.find(R"("exclusion_reason":"outside_declared_population")") != std::string::npos);
  // ...and an eligible row still reports `null` there, so the field is not
  // simply always populated.
  CHECK(dispatch(fx, {"models", "outcomes", "--limit", "2", "--json"}).out.find(R"("exclusion_reason":null)") !=
        std::string::npos);
}

TEST_CASE("models resolve validates role, task, plan and fallback-tier before touching a packet", "[cmd][models]") {
  auto const fx = make_fixture("resolve_validate");
  seed_project(fx);

  auto const bad_role = dispatch(fx, {"models", "resolve", "--role", "nosuch"});
  CHECK(bad_role.code == 2);
  CHECK(bad_role.err == "error: unknown role 'nosuch'\n");

  // A >64-byte role name is a DISTINCT refusal from "unknown role" — the
  // oracle copies `--role` into a fixed 64-byte buffer and refuses before
  // ever attempting the lookup (zig/src/cmd/planar/handlers/models.zig:548).
  // Oracle-verified directly (not copied from the review): 65 bytes ->
  // exit 2, stderr "error: role name too long\n", stdout empty.
  auto const long_role = dispatch(fx, {"models", "resolve", "--role", std::string(65, 'x')});
  CHECK(long_role.code == 2);
  CHECK(long_role.err == "error: role name too long\n");

  // Task-bound role, no --task: refuses BEFORE the database would matter,
  // naming the role it was given verbatim (not its normalized spelling).
  auto const no_task = dispatch(fx, {"models", "resolve", "--role", "coder"});
  CHECK(no_task.code == 2);
  CHECK(no_task.err == "error: --task is required for task-bound role 'coder'\n");

  auto const bad_task = dispatch(fx, {"models", "resolve", "--role", "coder", "--task", "abc"});
  CHECK(bad_task.code == 2);
  CHECK(bad_task.err == "error: invalid --task 'abc'\n");

  auto const bad_plan = dispatch(fx, {"models", "resolve", "--role", "planner", "--plan", "abc"});
  CHECK(bad_plan.code == 2);
  CHECK(bad_plan.err == "error: invalid --plan 'abc'\n");

  auto const bad_fallback = dispatch(fx, {"models", "resolve", "--role", "coder", "--task", "1", "--fallback-tier", "huge"});
  CHECK(bad_fallback.code == 2);
  CHECK(bad_fallback.err == "error: invalid --fallback-tier 'huge'\n");

  auto const no_task_row = dispatch(fx, {"models", "resolve", "--role", "coder", "--task", "999999"});
  CHECK(no_task_row.code == 1);
  CHECK(no_task_row.err == "error: no task with id 999999\n");

  auto const no_plan_row = dispatch(fx, {"models", "resolve", "--role", "planner", "--plan", "999999"});
  CHECK(no_plan_row.code == 1);
  CHECK(no_plan_row.err == "error: assembling planning packet: PlanNotFound\n");
}

TEST_CASE("models resolve --role spec_reviewer (underscored) is an undocumented alias for spec-reviewer", "[cmd][models]") {
  // roles.cppm's header: the oracle's hyphen-to-underscore map is a no-op on
  // an already-underscored name, so BOTH spellings resolve. `--help` and
  // `--role` documents only the hyphenated form.
  auto const fx = make_fixture("resolve_alias");
  seed_project(fx);
  auto const hyphen     = dispatch(fx, {"models", "resolve", "--role", "spec-reviewer", "--json"});
  auto const underscore = dispatch(fx, {"models", "resolve", "--role", "spec_reviewer", "--json"});
  CHECK(hyphen.code == 0);
  CHECK(underscore.code == 0);
  CHECK(hyphen.out == underscore.out);
  CHECK(hyphen.out.find(R"("role":"spec_reviewer")") != std::string::npos);
}

TEST_CASE("models resolve without --plan reports no_packet; a not-ready plan reports packet_not_ready", "[cmd][models]") {
  auto const fx = make_fixture("resolve_planning");
  seed_project(fx);

  auto const no_packet = dispatch(fx, {"models", "resolve", "--role", "planner", "--json"});
  CHECK(no_packet.code == 0);
  CHECK(no_packet.out == "{\"resolution_version\":\"routing-roles-v1\",\"role\":\"planner\",\"packet_class\":\"planning\","
                         "\"source\":\"static_fallback\",\"packet_backed\":false,\"tier\":\"medium\",\"work_type\":null,"
                         "\"complexity\":null,\"fallback_reason\":\"no_packet\",\"first_readiness_reason\":null,"
                         "\"rule_version\":null}\n");

  // A real plan with no linked artifacts: `planner` is NOT ready
  // (`missing_source_artifacts`), so this is the `packet_not_ready` arm
  // rather than `no_packet` — a different reason for a different absence.
  REQUIRE(dispatch(fx, {"plan", "create", "Resolve fixture plan", "--summary", "A goal.", "--scope", "global", "--json"}).code ==
          0);
  auto const not_ready = dispatch(fx, {"models", "resolve", "--role", "planner", "--plan", "1", "--json"});
  CHECK(not_ready.code == 0);
  CHECK(not_ready.out.find(R"("fallback_reason":"packet_not_ready")") != std::string::npos);
  CHECK(not_ready.out.find(R"("first_readiness_reason":"missing_source_artifacts")") != std::string::npos);
  CHECK(not_ready.out.find(R"("packet_backed":false)") != std::string::npos);

  // `orchestrator` needs only scope facts — the same plan resolves READY,
  // packet-backed, proving the wiring rather than only the refusal paths.
  auto const ready = dispatch(fx, {"models", "resolve", "--role", "orchestrator", "--plan", "1", "--json"});
  CHECK(ready.code == 0);
  CHECK(ready.out == "{\"resolution_version\":\"routing-roles-v1\",\"role\":\"orchestrator\",\"packet_class\":\"planning\","
                     "\"source\":\"packet\",\"packet_backed\":true,\"tier\":\"medium\",\"work_type\":null,\"complexity\":null,"
                     "\"fallback_reason\":null,\"first_readiness_reason\":null,\"rule_version\":\"routing-packet-v2\"}\n");

  auto const ready_text = dispatch(fx, {"models", "resolve", "--role", "orchestrator", "--plan", "1"});
  CHECK(ready_text.code == 0);
  CHECK(ready_text.out == "role   : orchestrator (planning packet)\ntier   : medium\nsource : packet (routing-packet-v2)\n");
}

TEST_CASE("models resolve on a READY high-risk task packet reaches complexity_tag, not complexity_to_text", "[cmd][models]") {
  // BLOCKING 1 from task 6343 iteration 2's review: this is the ONLY case in
  // this file that drives `models resolve`'s task-bound branch all the way
  // through a READY packet -- `to_profile_facts`, `prof::compile`,
  // `resolve_task_packet`, and both packet-backed render arms were
  // previously reachable from nothing here. `complexity` is forced to
  // `high_risk` specifically: it is the one band whose `@tagName` spelling
  // (`high_risk`) diverges from `ranking::complexity_to_text`'s schema
  // spelling (`high-risk`), so it is the only band a
  // `complexity_tag` -> `complexity_to_text` substitution would actually
  // kill. `bounded` and `standard` render identically either way and would
  // prove nothing.
  auto const fx = make_fixture("resolve_task_ready");
  seed_project(fx);
  seed_ready_task(fx);

  auto const json = dispatch(fx, {"models", "resolve", "--role", "coder", "--task", "100", "--json"});
  CHECK(json.code == 0);
  CHECK(json.out == "{\"resolution_version\":\"routing-roles-v1\",\"role\":\"coder\",\"packet_class\":\"task\","
                    "\"source\":\"packet\",\"packet_backed\":true,\"tier\":\"large\",\"work_type\":\"feature\","
                    "\"complexity\":\"high_risk\",\"fallback_reason\":null,\"first_readiness_reason\":null,"
                    "\"rule_version\":\"routing-profile-v1\"}\n");
  // The trap, spelled out: the underscored form appears and the hyphenated
  // schema spelling does not.
  CHECK(json.out.find(R"("complexity":"high_risk")") != std::string::npos);
  CHECK(json.out.find("high-risk") == std::string::npos);

  auto const text = dispatch(fx, {"models", "resolve", "--role", "coder", "--task", "100"});
  CHECK(text.code == 0);
  CHECK(text.out == "role   : coder (task packet)\n"
                    "tier   : large\n"
                    "source : packet (routing-profile-v1)\n"
                    "work   : feature\n"
                    "risk   : high_risk\n");
}
