// @file packet.t.cpp
// @brief Engine-level tests for `planar.engine.ingest.packet` (plan 996, task
// 6324).
//
// ## BOTH ARMS, OR THE SUITE PROVES NOTHING
//
// A bare task trips FIFTEEN reasons, and a plan's only task tripped seventeen
// when task 6298 measured it. That is the shape that makes this verb easy to
// fake: an implementation that ALWAYS refuses satisfies every `not_ready`
// assertion anyone would naturally write, and a fixture that is one row short
// of ready looks exactly like a correct implementation refusing correctly.
//
// So the fixture below is built READY FIRST — `assemble_task` on the untouched
// arena returns a packet with an EMPTY reasons list — and every other case is
// that same arena with ONE thing broken. Each degraded case therefore asserts
// two things at once: that the reason fires, and (because the ready case is
// pinned separately) that it fires for the stated reason rather than because
// the fixture never worked.
//
// `ready_fixture_is_actually_ready` is deliberately the first case in the
// file. If it ever goes red, every case after it is meaningless, and reading
// them as "the rules still work" would be the exact false green this file
// exists to prevent.
//
// ## ORACLE PROVENANCE
//
// Every expectation here was measured against `zig/zig-out/bin/planar` built
// at this cycle's base, in a scratch arena under a temp root (`PLANAR_DB`
// pointed at a throwaway file — never the operator's database). Exit codes
// were read from the command itself, never through a pipe.
//
// The identical SQL seed below was then replayed as a 21-arm differential —
// four golden captures (ready/blocked x json/text), two refusals, and fifteen
// one-axis degradations — through BOTH binaries against the SAME database
// file. All 21 agreed on stdout, stderr and exit code byte for byte. The
// shared-file arrangement is deliberate: both trees are at schema v33, so
// migrating once and running both against it removes migration difference
// from the comparison and leaves only the verb's own bytes.
//
// ## THE DIGESTS IN THE SEED ARE NOT DERIVED FROM THE CODE UNDER TEST
//
// A fact is fresh only when its stored `source_digest` equals the digest this
// module recomputes live. Seeding those with `materialize::source_digest`
// would make the freshness assertions circular — the fixture would agree with
// the implementation by construction, including when both are wrong. The six
// hex constants below were produced by `shasum -a 256` over the documented
// preimage, an implementation with no relationship to this tree, and each
// carries its preimage in a comment so the derivation is checkable by hand.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.db.migrate;
import planar.engine.ingest.materialize;
import planar.engine.ingest.packet;
// TEST-ONLY edge (`TEST_DEPENDS engine_planning` in this bucket's
// CMakeLists.txt, never `DEPENDS`): lets ONE test assert the duplicated
// counting queries in `packet.cpp`'s `planning_coverage_evidence` agree with
// `test_spec_status::compute`'s summary on the same fixture, rather than
// each being pinned independently and free to drift apart. See that
// CMakeLists.txt's note beside `TEST_DEPENDS` (task 6343 iteration 2,
// BLOCKING 2).
import planar.engine.planning.test_spec_status;

namespace {

namespace pk = planar::engine::ingest::packet;

/// @brief A scratch database path, removed with its sidecars on destruction.
struct scratch_db_path {
  std::filesystem::path path_;
  scratch_db_path()
      : path_(std::filesystem::temp_directory_path() / std::format("planar_packet_eng_{}_{}",
                                                                   std::chrono::steady_clock::now().time_since_epoch().count(),
                                                                   reinterpret_cast<std::uintptr_t>(this))) {
  }
  scratch_db_path(const scratch_db_path&)            = delete;
  scratch_db_path& operator=(const scratch_db_path&) = delete;
  ~scratch_db_path() {
    std::error_code ec;
    for (auto const* suffix : {"", "-journal", "-wal", "-shm"}) {
      std::filesystem::remove(path_.string() + suffix, ec);
    }
  }
};

auto open_migrated(const scratch_db_path& scratch) -> planar::db::connection {
  auto conn = planar::db::connection::open(scratch.path_.string());
  REQUIRE(conn.has_value());
  auto applied = planar::db::apply_all(*conn);
  REQUIRE(applied.has_value());
  return std::move(*conn);
}

/// @brief Run one statement, asserting it succeeded.
///
/// Guarded rather than fire-and-forget: an oracle probe written alongside this
/// file went vacuous because two seed statements failed silently into
/// /dev/null and the run compared an arena that was never built.
auto exec(planar::db::connection& conn, std::string_view sql) -> void {
  auto ok = conn.execute(sql);
  INFO("sql: " << sql);
  REQUIRE(ok.has_value());
}

// --- Digests, computed OUTSIDE this tree -----------------------------------
//
// `source_digest(kind, id, locator, semantic)` hashes
// `kind \0 id \0 locator \0 semantic`. Each constant below is
// `printf '<kind>\0<id>\0<locator>\0<semantic>' | shasum -a 256`.

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

constexpr std::int64_t ready_task = 100;
constexpr std::int64_t bare_task  = 200;
constexpr std::int64_t plan_id    = 1;

/// @brief Seed the shared arena: one READY task (100), its satisfied
/// dependency (101), and one bare root task (200) on the same active plan.
///
/// This is the byte-identical SQL the differential ran through both binaries.
auto seed(planar::db::connection& conn) -> void {
  exec(conn, "insert into projects (id, slug, name, root_path) values (1, 'pkt', 'pkt', '/tmp/pkt')");
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
  exec(conn, "insert into tasks (id, scope_kind, scope_id, plan_id, title, status, priority, slug) "
             "values (200, 'global', null, 1, 'Bare task', 'todo', 100, 'pkt-bare')");
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
             "values (100, 1, 'src/engine/ingest/packet.cpp')");
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
                         "'artifact:13#Overview','{}','spec-ingest-v1')",
                         d_acceptance, d_next_action, d_product, d_tech, d_roadmap, d_test_spec));
}

/// @brief The reason names of a packet, in emission order.
auto reason_names(const pk::task_packet& packet) -> std::vector<std::string> {
  std::vector<std::string> out;
  out.reserve(packet.reasons.size());
  for (const auto reason : packet.reasons) {
    out.emplace_back(pk::reason_name(reason));
  }
  return out;
}

/// @brief Assemble task 100 after applying `mutation` to the seeded arena.
///
/// Returns `assemble_task`'s own type so every case in this file spells its
/// packet identically whether it came from here or from a direct call.
auto degraded(planar::db::connection& conn, std::string_view mutation) -> std::expected<pk::task_packet, pk::packet_error> {
  exec(conn, mutation);
  auto packet = pk::assemble_task(conn, ready_task);
  REQUIRE(packet.has_value());
  return packet;
}

} // namespace

// ===========================================================================
// The ready arm. Read this first.
// ===========================================================================

TEST_CASE("ready fixture is actually ready", "[packet]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  auto packet = pk::assemble_task(conn, ready_task);
  REQUIRE(packet.has_value());

  // THE load-bearing assertion of this file. Every degraded case below is this
  // arena with one thing broken; if this is red they are all vacuous.
  INFO("reasons: " << std::format("{}", reason_names(*packet)));
  CHECK(packet->reasons.empty());
  CHECK(packet->ready());

  // A ready packet is still a fully populated packet, not an empty one that
  // happens to have no complaints.
  CHECK(packet->input.task_id == ready_task);
  CHECK(packet->input.status == "doing");
  CHECK(packet->input.acceptance_criteria == "The packet compiles with zero readiness reasons.");
  CHECK(packet->input.owning_plans.size() == 1);
  CHECK(packet->input.anchor_plans.size() == 1);
  CHECK(packet->input.citations.size() == 4);
  CHECK(packet->input.decisions.size() == 1);
  CHECK(packet->input.scenarios.size() == 1);
  CHECK(packet->input.dependencies.size() == 1);
  CHECK(packet->input.touches.size() == 1);
  CHECK(packet->input.validation_gates.size() == 1);
  CHECK(packet->input.facts.size() == 6);
  CHECK(packet->digest.size() == 64);
}

TEST_CASE("the ready packet's evidence is current rather than merely present", "[packet]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);
  auto packet = pk::assemble_task(conn, ready_task);
  REQUIRE(packet.has_value());

  // The freshness fields are what the whole `stale_*` family reads. Asserting
  // only "reasons is empty" would pass for an implementation that never
  // computed a live digest at all.
  for (const auto& citation : packet->input.citations) {
    INFO("citation " << citation.locator);
    CHECK(citation.freshness == "current");
    CHECK_FALSE(citation.current_digest.empty());
    CHECK(citation.source_digest == citation.current_digest);
    CHECK(citation.materializer_version == "spec-ingest-v1");
  }
  for (const auto& fact : packet->input.facts) {
    INFO("fact " << fact.kind << " " << fact.locator);
    CHECK(fact.freshness == "current");
    CHECK(fact.source_digest == fact.current_digest);
  }
  // The satisfied dependency's `done` is REWRITTEN to `satisfied` by the
  // loader query, which is what makes the evidence-class walk accept it.
  REQUIRE(packet->input.dependencies.size() == 1);
  CHECK(packet->input.dependencies[0].status == "satisfied");
  // Anchor resolves to the owning plan itself when the plan has no parent.
  CHECK(packet->input.anchor_plans[0].id == plan_id);
  CHECK(packet->input.scenarios[0].covered);
}

// ===========================================================================
// The blocked arm.
// ===========================================================================

TEST_CASE("a bare task reports its reasons in emission order", "[packet]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  auto packet = pk::assemble_task(conn, bare_task);
  REQUIRE(packet.has_value());
  CHECK_FALSE(packet->ready());

  // Captured from the oracle. This is the ORDER OF THE CHECKS in
  // `compile_task`, deduplicated — NOT sorted, and not grouped. Pinned as a
  // sequence rather than a set precisely because a future "tidy up the
  // reasons" change would pass a set comparison.
  const std::vector<std::string> expected = {"missing_body",
                                             "missing_acceptance_section",
                                             "generic_acceptance",
                                             "generic_next_action",
                                             "missing_product_spec",
                                             "missing_tech_spec",
                                             "missing_roadmap",
                                             "missing_test_spec",
                                             "missing_locked_decision",
                                             "missing_touch",
                                             "absent_validation_gates",
                                             "missing_acceptance_fact",
                                             "missing_next_action_fact",
                                             "uncovered_required_scenario"};
  CHECK(reason_names(*packet) == expected);

  // NOT present, and each absence is meaningful because the presence case is
  // pinned elsewhere in this file: the task HAS a title, IS `todo`, and DOES
  // own an active plan.
  const auto names = reason_names(*packet);
  CHECK(std::ranges::find(names, "missing_title") == names.end());
  CHECK(std::ranges::find(names, "invalid_task_status") == names.end());
  CHECK(std::ranges::find(names, "missing_owning_plan") == names.end());
  CHECK(std::ranges::find(names, "missing_anchor_plan") == names.end());
}

TEST_CASE("a root task is not missing a dependency", "[packet]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  // A root task declares no dependency by design. It can still be blocked by
  // every other missing readiness input, but absence of an edge is not itself
  // an error. A declared unfinished dependency remains invalid below.
  auto packet = pk::assemble_task(conn, bare_task);
  REQUIRE(packet.has_value());
  const auto names = reason_names(*packet);
  CHECK(std::ranges::find(names, "missing_dependency") == names.end());
}

// ===========================================================================
// One axis broken at a time, against the arena the first case proved ready.
// ===========================================================================

TEST_CASE("editing a cited artifact strands its citation and its fact", "[packet]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  auto packet = degraded(conn, "update artifacts set body='## Overview\nCHANGED.\n' where id=10");

  // The ordering case. `unresolved_citation` is reachable from BOTH the main
  // block's citation loop and the later evidence-class walk; deduplication
  // keeps the FIRST, so it lands early. `stale_mandatory_evidence` is
  // reachable only from the class walk and lands last.
  const std::vector<std::string> expected = {"missing_product_spec", "unresolved_citation", "stale_fact",
                                             "stale_mandatory_evidence"};
  CHECK(reason_names(*packet) == expected);
}

TEST_CASE("an unfinished dependency is invalid rather than missing", "[packet]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  // The edge still exists, so `missing_dependency` does NOT fire — the packet
  // distinguishes "no dependency declared" from "declared and not done".
  auto       packet = degraded(conn, "update tasks set status='todo' where id=101");
  const auto names  = reason_names(*packet);
  CHECK(names == std::vector<std::string>{"invalid_dependency"});
}

TEST_CASE("an open question blocks and an answered one does not", "[packet]") {
  {
    scratch_db_path scratch;
    auto            conn = open_migrated(scratch);
    seed(conn);
    exec(conn, "insert into questions (id, scope_kind, scope_id, title, body, status) "
               "values (40,'global',null,'Q','Q body.','open')");
    exec(conn, "insert into entity_links (from_kind,from_id,to_kind,to_id,relationship) "
               "values ('task',100,'question',40,'addresses')");
    auto linked = pk::assemble_task(conn, ready_task);
    REQUIRE(linked.has_value());
    CHECK(reason_names(*linked) == std::vector<std::string>{"unresolved_question"});
  }
  {
    // The paired presence case: the same link with an ANSWERED question leaves
    // the packet ready, so the assertion above is about the status and not
    // about linking a question at all.
    scratch_db_path scratch;
    auto            conn = open_migrated(scratch);
    seed(conn);
    exec(conn, "insert into questions (id, scope_kind, scope_id, title, body, status, answer_body, answered_at) "
               "values (40,'global',null,'Q','Q body.','answered','A','2020-01-01T00:00:00.000Z')");
    exec(conn, "insert into entity_links (from_kind,from_id,to_kind,to_id,relationship) "
               "values ('task',100,'question',40,'addresses')");
    auto packet = pk::assemble_task(conn, ready_task);
    REQUIRE(packet.has_value());
    CHECK(packet->reasons.empty());
  }
}

TEST_CASE("a lapsed lease blocks the packet and a live one does not", "[packet]") {
  constexpr std::string_view session = "insert into sessions (id, vendor, started_at) "
                                       "values (1,'claude','2000-01-01T00:00:00.000Z')";
  {
    scratch_db_path scratch;
    auto            conn = open_migrated(scratch);
    seed(conn);
    exec(conn, session);
    auto packet = degraded(conn, "insert into agent_work_claims (id, claim_token, session_id, entity_kind, entity_id, "
                                 "status, vendor, lease_expires_at) "
                                 "values (1,'tok',1,'task',100,'active','claude','2000-01-01T00:00:00.000Z')");
    // The row is `status='active'`; only the LEASE has passed. The loader
    // re-derives `expired` from `lease_expires_at`, so a stale claim is
    // visible evidence rather than an absent row that silently permits.
    CHECK(reason_names(*packet) == std::vector<std::string>{"inactive_claim"});
    REQUIRE(packet->input.claims.size() == 1);
    CHECK(packet->input.claims[0].status == "expired");
  }
  {
    scratch_db_path scratch;
    auto            conn = open_migrated(scratch);
    seed(conn);
    exec(conn, session);
    auto packet = degraded(conn, "insert into agent_work_claims (id, claim_token, session_id, entity_kind, entity_id, "
                                 "status, vendor, lease_expires_at) "
                                 "values (1,'tok',1,'task',100,'active','claude','2999-01-01T00:00:00.000Z')");
    CHECK(packet->reasons.empty());
    REQUIRE(packet->input.claims.size() == 1);
    CHECK(packet->input.claims[0].status == "active");
  }
}

TEST_CASE("a draft plan invalidates both plan classes", "[packet]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  // Owning and anchor are the SAME row here, and both classes report — the
  // walk does not deduplicate across classes, only within the reason list.
  auto packet = degraded(conn, "update plans set status='draft' where id=1");
  CHECK(reason_names(*packet) == std::vector<std::string>{"invalid_owning_plan", "invalid_anchor_plan"});
}

TEST_CASE("a scenario outside the anchor plan stays visible but uncovered", "[packet]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  auto packet = degraded(conn, "delete from entity_links where from_kind='test_scenario' and to_kind='plan'");
  CHECK(reason_names(*packet) == std::vector<std::string>{"uncovered_required_scenario"});
  // The scenario is NOT dropped from the packet. Coverage is a separate fact
  // from linkage, so the operator can see which scenario failed to qualify.
  REQUIRE(packet->input.scenarios.size() == 1);
  CHECK_FALSE(packet->input.scenarios[0].covered);
  CHECK(packet->input.scenarios[0].status == "ready");
}

TEST_CASE("a whole-repo touch satisfies the touch requirement", "[packet]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  {
    // Absence first...
    scratch_db_path inner_scratch;
    auto            inner = open_migrated(inner_scratch);
    seed(inner);
    auto packet = degraded(inner, "delete from task_touch_paths where task_id=100");
    CHECK(reason_names(*packet) == std::vector<std::string>{"missing_touch"});
  }
  // ...then presence via the OTHER of the two touch sources, so the assertion
  // above is about having no touch rather than about the path table
  // specifically.
  exec(conn, "delete from task_touch_paths where task_id=100");
  exec(conn, "insert into entity_links (from_kind,from_id,to_kind,to_id,relationship) "
             "values ('task',100,'repo',1,'touches')");
  auto packet = pk::assemble_task(conn, ready_task);
  REQUIRE(packet.has_value());
  CHECK(packet->reasons.empty());
  REQUIRE(packet->input.touches.size() == 1);
  CHECK(packet->input.touches[0].locator == "repo:1");
}

TEST_CASE("a closed task is an invalid status for dispatch", "[packet]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  auto packet = degraded(conn, "update tasks set status='done' where id=100");
  CHECK(reason_names(*packet) == std::vector<std::string>{"invalid_task_status"});
}

TEST_CASE("an orphaned task loses its plan and its scenario coverage together", "[packet]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  // Dropping `plan_id` takes the anchor with it, and the anchor is what the
  // scenario's coverage subquery joins against — so coverage fails as a
  // CONSEQUENCE, not independently. Pinned because "unlink the plan, get two
  // reasons" is surprising until you see why.
  auto packet = degraded(conn, "update tasks set plan_id=null where id=100");
  CHECK(reason_names(*packet) ==
        std::vector<std::string>{"missing_owning_plan", "missing_anchor_plan", "uncovered_required_scenario"});
}

TEST_CASE("boilerplate acceptance text is rejected as generic", "[packet]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  // `works as expected` is one of three banned phrases, matched
  // case-insensitively. Rewriting the body also strands the acceptance fact,
  // whose digest was taken over the OLD section — which is the honest
  // consequence and is asserted rather than avoided.
  auto packet = degraded(conn, "update tasks set body='Body.\n\n## Acceptance Criteria\nWorks as expected.\n\n"
                               "## Required validation\ncmake --build build/debug\n' where id=100");
  CHECK(reason_names(*packet) == std::vector<std::string>{"generic_acceptance", "missing_acceptance_fact", "stale_fact"});
}

TEST_CASE("a fact written by an older materializer is stale even when its digest matches", "[packet]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  // The digest is untouched here; only `materializer_version` moved. A
  // freshness check that compared digests alone would call this current and
  // ship a packet built on rules that no longer exist.
  auto packet = degraded(conn, "update routing_task_facts set materializer_version='spec-ingest-v0' "
                               "where task_id=100 and fact_kind='next_action_exact'");
  CHECK(reason_names(*packet) == std::vector<std::string>{"missing_next_action_fact", "stale_fact"});
}

TEST_CASE("the evidence-class walk compares digests directly, not freshness", "[packet]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  // ADDED TO CLOSE A BREAK-PROBE SURVIVOR. Swapping the class walk's direct
  // digest comparison for `evidence_current` survived the whole suite,
  // because nothing reached an input where the two predicates disagree. They
  // disagree on exactly this: a CITATION whose stored digest still matches
  // the live one, but whose `materializer_version` has moved. It is `stale`
  // by freshness and `current` by digest.
  //
  // Measured on the oracle: `stale_mandatory_evidence` does NOT fire.
  // `evidence_current` would have fired it, so the difference between the two
  // predicates is operator-visible and is not a stylistic choice.
  //
  // Contrast with `editing a cited artifact strands its citation and its
  // fact` above, where the digests genuinely differ and the reason DOES
  // appear. Absence here is only meaningful next to that presence.
  auto packet = degraded(conn, "update routing_task_facts set materializer_version='spec-ingest-v0' "
                               "where task_id=100 and fact_kind='cited_artifact_section' and source_entity_id=10");
  CHECK(reason_names(*packet) == std::vector<std::string>{"missing_product_spec", "unresolved_citation", "stale_fact"});

  // The citation really is in the disagreeing state — otherwise the absence
  // above would be passing for the wrong reason.
  const auto product =
      std::ranges::find_if(packet->input.citations, [](const pk::evidence& e) { return e.kind == "product_spec"; });
  REQUIRE(product != packet->input.citations.end());
  CHECK(product->source_digest == product->current_digest);
  CHECK(product->freshness == "stale");
  CHECK(product->materializer_version == "spec-ingest-v0");
  CHECK(product->current_materializer_version == "spec-ingest-v1");
}

TEST_CASE("a child plan anchors on its parent", "[packet]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  exec(conn, "insert into plans (id, scope_kind, scope_id, title, slug, status, parent_plan_id) "
             "values (2,'global',null,'Child','child','active',1)");
  exec(conn, "update tasks set plan_id=2 where id=100");
  // Coverage now has to be measured against the PARENT, so re-point the
  // scenario's ownership edge at plan 1 and assert it still qualifies.
  exec(conn, "delete from entity_links where from_kind='test_scenario' and to_kind='plan'");
  exec(conn, "insert into entity_links (from_kind,from_id,to_kind,to_id,relationship) "
             "values ('test_scenario',30,'plan',1,'derives-from')");

  auto packet = pk::assemble_task(conn, ready_task);
  REQUIRE(packet.has_value());
  CHECK(packet->reasons.empty());
  CHECK(packet->input.owning_plans[0].id == 2);
  CHECK(packet->input.anchor_plans[0].id == 1);
}

// ===========================================================================
// Refusal, digest behaviour, and the pure compiler.
// ===========================================================================

TEST_CASE("an unknown task refuses rather than compiling an empty packet", "[packet]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  auto packet = pk::assemble_task(conn, 999999);
  REQUIRE_FALSE(packet.has_value());
  CHECK(packet.error() == pk::packet_error::task_not_found);
}

TEST_CASE("the digest is stable across recompiles and moves with semantic edits", "[packet]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  auto first = pk::assemble_task(conn, ready_task);
  REQUIRE(first.has_value());
  auto again = pk::assemble_task(conn, ready_task);
  REQUIRE(again.has_value());
  // Recompiling unchanged state must reproduce the digest, or it could never
  // certify "this is the packet the dispatch was authorized against".
  CHECK(first->digest == again->digest);

  exec(conn, "update tasks set title='Renamed' where id=100");
  auto edited = pk::assemble_task(conn, ready_task);
  REQUIRE(edited.has_value());
  CHECK(edited->digest != first->digest);
}

TEST_CASE("a display-only rename moves the canonical body but not the digest", "[packet]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  auto before = pk::assemble_task(conn, ready_task);
  REQUIRE(before.has_value());

  // A plan's TITLE reaches the packet only as `display_label`, which the
  // canonical body carries and the digest's preimage omits. This is the whole
  // point of compiling the body twice.
  exec(conn, "update plans set title='Packet plan RENAMED' where id=1");
  auto after = pk::assemble_task(conn, ready_task);
  REQUIRE(after.has_value());
  CHECK(after->digest == before->digest);
  CHECK(after->canonical != before->canonical);
  CHECK(after->canonical.find("Packet plan RENAMED") != std::string::npos);
}

TEST_CASE("the canonical body ignores evidence input order", "[packet]") {
  // Reached through the PURE compiler, because SQL always returns the loaders'
  // `order by` and cannot produce a permuted input at all.
  const auto make = [](std::string_view sd, std::string_view kind, std::int64_t id) {
    pk::evidence e;
    e.kind           = kind;
    e.id             = id;
    e.locator        = std::format("artifact:{}#S", id);
    e.text           = std::string{sd};
    e.source_digest  = std::string{sd};
    e.current_digest = std::string{sd};
    return e;
  };
  const auto a = make("a", "product_spec", 2);
  const auto b = make("b", "tech_spec", 3);
  const auto c = make("c", "roadmap", 4);
  const auto d = make("d", "test_spec", 5);

  pk::task_input forward;
  forward.task_id             = 42;
  forward.status              = "doing";
  forward.title               = R"(Exact "title"; $(still data))";
  forward.body                = "Body.\n## Acceptance Criteria\nReal criteria.\n";
  forward.next_action         = "Do the specific thing.";
  forward.acceptance_criteria = "Real criteria.";
  forward.citations           = {a, b, c, d};

  auto reversed      = forward;
  reversed.citations = {d, c, b, a};

  const auto one = pk::compile_task(forward);
  const auto two = pk::compile_task(reversed);
  CHECK(one.digest == two.digest);
  CHECK(one.canonical == two.canonical);

  // Adversarial text stays DATA: the quote and the command substitution are
  // escaped into the JSON string rather than terminating it.
  CHECK(one.canonical.find(R"(\"title\"; $(still data))") != std::string::npos);
}

TEST_CASE("the canonical order is total for rows sharing an identity key", "[packet]") {
  // Two rows agreeing on kind/id/locator/text and differing only in fields
  // BELOW them in the comparator. Without the lower tiebreakers the sort would
  // be unstable and the canonical body would depend on input order.
  pk::evidence low;
  low.kind           = "product_spec";
  low.id             = 1;
  low.locator        = "same";
  low.text           = "same";
  low.source_digest  = "x";
  low.current_digest = "a";
  low.required       = false;
  low.covered        = false;
  low.status         = "resolved";
  low.provenance     = "artifact:1";

  auto high           = low;
  high.current_digest = "b";
  high.required       = true;
  high.covered        = true;
  high.status         = "ready";
  high.provenance     = "artifact:2";

  pk::task_input forward;
  forward.citations  = {low, high};
  auto reversed      = forward;
  reversed.citations = {high, low};

  CHECK(pk::compile_task(forward).canonical == pk::compile_task(reversed).canonical);
}

TEST_CASE("an empty input names every applicable reason", "[packet]") {
  // The pure compiler on a fully empty input. This is the only way to reach
  // `missing_title` and the two plan-absence reasons at the head of the list,
  // and it pins that the head of the emission order is what it claims to be.
  const auto                     packet   = pk::compile_task(pk::task_input{});
  const std::vector<std::string> expected = {"missing_title",
                                             "missing_body",
                                             "invalid_task_status",
                                             "missing_acceptance_section",
                                             "generic_acceptance",
                                             "generic_next_action",
                                             "missing_owning_plan",
                                             "missing_anchor_plan",
                                             "missing_product_spec",
                                             "missing_tech_spec",
                                             "missing_roadmap",
                                             "missing_test_spec",
                                             "missing_locked_decision",
                                             "missing_touch",
                                             "absent_validation_gates",
                                             "missing_acceptance_fact",
                                             "missing_next_action_fact",
                                             "uncovered_required_scenario"};
  CHECK(reason_names(packet) == expected);
}

TEST_CASE("a mandatory fact that is present but false is invalid rather than missing", "[packet]") {
  // `invalid_mandatory_fact` is unreachable from `assemble_task` with the
  // seed above, because `spec ingest` only ever writes true facts. It is
  // reachable through the pure compiler and is a real rule, so it is pinned
  // here rather than left as dead code nobody has executed.
  pk::evidence fact;
  fact.kind           = "acceptance_complete";
  fact.id             = 1;
  fact.locator        = "body#acceptance-criteria";
  fact.text           = "0";
  fact.source_digest  = "same";
  fact.current_digest = "same";

  pk::task_input input;
  input.facts      = {fact};
  const auto names = reason_names(pk::compile_task(input));
  CHECK(std::ranges::find(names, "invalid_mandatory_fact") != names.end());
  CHECK(std::ranges::find(names, "missing_acceptance_fact") == names.end());
}

TEST_CASE("evidence lacking provenance is refused even though it is otherwise current", "[packet]") {
  // `missing_provenance` (packet.cpp's evidence-class walk) fires when a
  // REQUIRED evidence row's `provenance` is blank. Every real loader stamps
  // a non-empty provenance (see `row_evidence`'s callers), so this rule is
  // unreachable from `assemble_task` and can only be pinned through the pure
  // compiler -- same rationale as `invalid_mandatory_fact` above. Reached
  // through the citation class specifically: `validate_evidence_classes`
  // walks `citations` and, independently of digest freshness or lifecycle
  // status, requires a non-blank provenance on every required row.
  pk::evidence citation;
  citation.kind           = "product_spec";
  citation.id             = 10;
  citation.locator        = "artifact:10#Overview";
  citation.text           = "Spec section body for product_spec.";
  citation.source_digest  = "same";
  citation.current_digest = "same";
  citation.status         = "active";
  citation.required       = true;
  citation.provenance     = ""; // <-- the defect under test

  pk::task_input input;
  input.citations  = {citation};
  const auto names = reason_names(pk::compile_task(input));
  CHECK(std::ranges::find(names, "missing_provenance") != names.end());

  // `evidence_current` ALSO gates on a non-blank provenance, independently of
  // the class-walk check above (see this file's header note on the two not
  // being interchangeable). `has_kind("product_spec")` calls `evidence_current`
  // directly, so a blank-provenance citation must ALSO fail that gate and
  // trip `missing_product_spec` -- proving the provenance clause INSIDE
  // `evidence_current` fires, not merely the class-walk's own direct check.
  CHECK(std::ranges::find(names, "missing_product_spec") != names.end());
}

TEST_CASE("a required scenario lacking provenance is refused even when covered", "[packet]") {
  // The scenario walk in `validate_evidence_classes` duplicates the same
  // provenance check independently of the citation/decision/etc. class loop
  // above (see that function's own comment on why scenarios are handled
  // separately). Pinned as its own case so the two blank-provenance arms are
  // not conflated into one assertion covering only one of them.
  pk::evidence scenario;
  scenario.kind           = "test_scenario";
  scenario.id             = 30;
  scenario.locator        = "scenario:30";
  scenario.text           = "Scenario body.";
  scenario.source_digest  = "same";
  scenario.current_digest = "same";
  scenario.status         = "ready";
  scenario.required       = true;
  scenario.covered        = true;
  scenario.provenance     = ""; // <-- the defect under test

  pk::task_input input;
  input.scenarios  = {scenario};
  const auto names = reason_names(pk::compile_task(input));
  CHECK(std::ranges::find(names, "missing_provenance") != names.end());
}

TEST_CASE("render_text names every reason and ends with the canonical body", "[packet]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  auto blocked = pk::assemble_task(conn, bare_task);
  REQUIRE(blocked.has_value());
  const auto text = pk::render_text(*blocked);
  CHECK(text.starts_with(std::format("task packet {}: not_ready\n", bare_task)));
  CHECK(text.find(std::format("digest: {}\n", blocked->digest)) != std::string::npos);
  CHECK(text.find("reasons:\n- missing_body\n") != std::string::npos);
  CHECK(text.ends_with(blocked->canonical + "\n"));

  auto ready = pk::assemble_task(conn, ready_task);
  REQUIRE(ready.has_value());
  const auto ready_text = pk::render_text(*ready);
  CHECK(ready_text.starts_with(std::format("task packet {}: ready\n", ready_task)));
  // A ready packet emits NO `reasons:` block at all — not an empty one.
  CHECK(ready_text.find("reasons:") == std::string::npos);
}

TEST_CASE("render_json carries the policy version in the envelope", "[packet]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  auto ready = pk::assemble_task(conn, ready_task);
  REQUIRE(ready.has_value());
  const auto json = pk::render_json(*ready);

  // The version rides in the envelope so a consumer can establish that two
  // packets are comparable BEFORE parsing the canonical body — which is the
  // very thing whose format the version describes.
  CHECK(json.starts_with(R"({"policy_version":"routing-packet-v2","ready":true,"input":{)"));
  CHECK(json.find(R"("reasons":[]})") != std::string::npos);
  CHECK(json.ends_with("\n"));
  // `policy` appears again INSIDE the escaped canonical body; the two are not
  // the same field and both are contract.
  CHECK(json.find(R"(\"policy\":\"routing-packet-v2\")") != std::string::npos);

  auto blocked = pk::assemble_task(conn, bare_task);
  REQUIRE(blocked.has_value());
  const auto blocked_json = pk::render_json(*blocked);
  CHECK(blocked_json.find(R"("ready":false)") != std::string::npos);
  CHECK(blocked_json.find(R"("reasons":["missing_body","missing_acceptance_section")") != std::string::npos);
}

// ===========================================================================
// Planning half (plan 996, task 6343).
// ===========================================================================

namespace {

/// @brief The reason names of a planning packet, in emission order.
auto planning_reason_names(const pk::planning_packet& packet) -> std::vector<std::string> {
  std::vector<std::string> out;
  out.reserve(packet.reasons.size());
  for (const auto reason : packet.reasons) {
    out.emplace_back(pk::planning_reason_name(reason));
  }
  return out;
}

/// @brief One current, `draft`-status evidence row of `kind`, required.
auto current_evidence(std::string_view kind, std::string_view status = "draft") -> pk::evidence {
  pk::evidence item;
  item.kind           = std::string{kind};
  item.locator        = std::string{kind};
  item.text           = "body";
  item.source_digest  = "same";
  item.current_digest = "same";
  item.status         = std::string{status};
  item.provenance     = std::string{kind};
  return item;
}

/// @brief The four `required_output`-shaped kinds a planner/spec-reviewer/
/// ingestor packet needs, all current, at `status`.
auto four_specs(std::string_view status = "draft") -> std::vector<pk::evidence> {
  return {current_evidence("product_spec", status), current_evidence("tech_spec", status), current_evidence("roadmap", status),
          current_evidence("test_spec", status)};
}

} // namespace

TEST_CASE("compile_planning: an empty input names every applicable reason, per role", "[packet][planning]") {
  // Mirrors `an empty input names every applicable reason` above, one per
  // planning role, so the switch's four arms are each pinned independently.
  {
    pk::planning_input input;
    input.role = pk::planning_role::planner;
    CHECK(planning_reason_names(pk::compile_planning(input)) == std::vector<std::string>{"missing_goal", "missing_scope_facts",
                                                                                         "missing_source_artifacts",
                                                                                         "missing_required_outputs"});
  }
  {
    pk::planning_input input;
    input.role = pk::planning_role::spec_reviewer;
    CHECK(
        planning_reason_names(pk::compile_planning(input)) ==
        std::vector<std::string>{"missing_goal", "missing_artifact_digests", "missing_strict_preview", "missing_review_rubric"});
  }
  {
    pk::planning_input input;
    input.role = pk::planning_role::ingestor;
    CHECK(planning_reason_names(pk::compile_planning(input)) ==
          std::vector<std::string>{"missing_goal", "missing_artifact_digests", "missing_reviewed_artifacts",
                                   "missing_strict_preview", "missing_coverage", "missing_locked_decisions",
                                   "missing_apply_boundary"});
  }
  {
    pk::planning_input input;
    input.role = pk::planning_role::orchestrator;
    CHECK(planning_reason_names(pk::compile_planning(input)) == std::vector<std::string>{"missing_goal", "missing_scope_facts"});
  }
}

TEST_CASE("compile_planning: planner and orchestrator are ready on minimal evidence", "[packet][planning]") {
  // The orchestrator role needs only a goal and scope facts -- no artifacts,
  // no decisions, no coverage. This is the arm `models resolve --role
  // orchestrator --plan <id>` reaches for any plan with a title.
  pk::planning_input orch;
  orch.role              = pk::planning_role::orchestrator;
  orch.goal              = "Ship the thing.";
  orch.scope_facts       = {current_evidence("scope")};
  const auto orch_packet = pk::compile_planning(orch);
  CHECK(orch_packet.ready());
  CHECK(orch_packet.reasons.empty());

  pk::planning_input planner;
  planner.role             = pk::planning_role::planner;
  planner.goal             = "Ship the thing.";
  planner.scope_facts      = {current_evidence("scope")};
  planner.artifacts        = {current_evidence("product_spec")};
  planner.required_outputs = {current_evidence("required_output")};
  CHECK(pk::compile_planning(planner).ready());
}

TEST_CASE("compile_planning: non_current_artifacts and invalid_locked_decisions fire ahead of the role switch",
          "[packet][planning]") {
  // Both checks run BEFORE the role switch and apply to every role — proven
  // here on `orchestrator`, the role whose own arm checks neither.
  pk::evidence stale_artifact   = current_evidence("product_spec");
  stale_artifact.current_digest = "different";

  pk::planning_input input;
  input.role        = pk::planning_role::orchestrator;
  input.goal        = "Ship the thing.";
  input.scope_facts = {current_evidence("scope")};
  input.artifacts   = {stale_artifact};
  const auto names  = planning_reason_names(pk::compile_planning(input));
  CHECK(std::ranges::find(names, "non_current_artifacts") != names.end());

  pk::evidence rejected_decision = current_evidence("decision");
  rejected_decision.status       = "rejected";
  input.artifacts                = {};
  input.decisions                = {rejected_decision};
  const auto decision_names      = planning_reason_names(pk::compile_planning(input));
  CHECK(std::ranges::find(decision_names, "invalid_locked_decisions") != decision_names.end());
}

TEST_CASE("compile_planning: ingestor's coverage and gate checks distinguish ABSENT from INVALID", "[packet][planning]") {
  pk::planning_input base;
  base.role                  = pk::planning_role::ingestor;
  base.goal                  = "Ship the thing.";
  base.artifacts             = four_specs("active"); // satisfies BOTH the current and reviewed bars
  base.review_rubric_version = "spec-review-v1";
  base.strict_preview        = {current_evidence("gate", "accepted")};
  base.decisions             = {current_evidence("decision", "accepted")};
  base.apply_boundary        = {current_evidence("gate", "accepted")};

  // Coverage ABSENT.
  {
    auto       input = base;
    const auto names = planning_reason_names(pk::compile_planning(input));
    CHECK(std::ranges::find(names, "missing_coverage") != names.end());
    CHECK(std::ranges::find(names, "incomplete_coverage") == names.end());
  }
  // Coverage PRESENT but INVALID -- a required row that is not `complete`.
  {
    auto input       = base;
    input.coverage   = {current_evidence("coverage", "incomplete")};
    const auto names = planning_reason_names(pk::compile_planning(input));
    CHECK(std::ranges::find(names, "missing_coverage") == names.end());
    CHECK(std::ranges::find(names, "incomplete_coverage") != names.end());
  }
  // Coverage PRESENT and VALID -- neither reason fires, and the packet is
  // ready (the coverage row's `covered` flag must also be true).
  {
    auto input           = base;
    auto complete_row    = current_evidence("coverage", "complete");
    complete_row.covered = true;
    input.coverage       = {complete_row};
    const auto packet    = pk::compile_planning(input);
    CHECK(packet.ready());
  }
}

TEST_CASE("compile_planning: the strict-preview and apply-boundary gates require ready evidence", "[packet][planning]") {
  pk::planning_input input;
  input.role                  = pk::planning_role::spec_reviewer;
  input.goal                  = "Ship the thing.";
  input.artifacts             = four_specs("draft");
  input.review_rubric_version = "spec-review-v1";

  // Present but every row unaccepted -- INVALID, not missing.
  input.strict_preview = {current_evidence("gate", "pending")};
  const auto names     = planning_reason_names(pk::compile_planning(input));
  CHECK(std::ranges::find(names, "missing_strict_preview") == names.end());
  CHECK(std::ranges::find(names, "invalid_strict_preview") != names.end());

  // Accepted -- both reasons clear and the packet is ready (spec-reviewer
  // needs no coverage or decisions).
  input.strict_preview = {current_evidence("gate", "accepted")};
  CHECK(pk::compile_planning(input).ready());
}

TEST_CASE("assemble_planning: PlanNotFound for a missing anchor", "[packet][planning]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  const auto missing = pk::assemble_planning(conn, pk::planning_role::orchestrator, 999999);
  REQUIRE_FALSE(missing.has_value());
  CHECK(missing.error() == pk::packet_error::plan_not_found);
}

TEST_CASE("assemble_planning: orchestrator is ready off the plan row alone", "[packet][planning]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  auto packet = pk::assemble_planning(conn, pk::planning_role::orchestrator, plan_id);
  REQUIRE(packet.has_value());
  CHECK(packet->ready());
  CHECK(packet->input.scope_facts.size() == 1);
  CHECK(packet->input.scope_facts[0].locator == "plan:scope");
}

TEST_CASE("assemble_planning: planner is not ready without a derives-from artifact link", "[packet][planning]") {
  // `seed()` links artifacts 10-13 to TASK 100 via `cites`, not to plan 1 via
  // `derives-from` -- so the planning packet's `artifacts` list is empty even
  // though the same rows exist and satisfy the TASK packet's citations.
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  auto packet = pk::assemble_planning(conn, pk::planning_role::planner, plan_id);
  REQUIRE(packet.has_value());
  CHECK_FALSE(packet->ready());
  const auto names = planning_reason_names(*packet);
  CHECK(std::ranges::find(names, "missing_source_artifacts") != names.end());
}

TEST_CASE("assemble_planning: the duplicated coverage counts match the seeded arena", "[packet][planning]") {
  // Plan 1 has three tasks (100 ready, 101 done dependency, 200 bare) and one
  // scenario (30) attached via `derives-from`, verifying only task 100 -- so
  // total_tasks=3, tasks_covered=1, total_scenarios=1, and the packet reports
  // the coverage row `incomplete` (1 != 3) even though a scenario exists.
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  auto packet = pk::assemble_planning(conn, pk::planning_role::ingestor, plan_id);
  REQUIRE(packet.has_value());
  REQUIRE(packet->input.coverage.size() == 1);
  const auto& coverage = packet->input.coverage[0];
  CHECK(coverage.text == "tasks:3;covered:1;scenarios:1");
  CHECK(coverage.status == "incomplete");
  CHECK_FALSE(coverage.covered);
  const auto names = planning_reason_names(*packet);
  CHECK(std::ranges::find(names, "incomplete_coverage") != names.end());
}

TEST_CASE("assemble_planning: constraints and decisions carry the SAME rows, independently", "[packet][planning]") {
  // The oracle's `PlanningInput` reuses one arena slice for both fields; this
  // port copies once and moves once (see packet.cpp's `assemble_planning`).
  // Proven here rather than trusted: both fields must end up with identical
  // content, and mutating the returned struct's copies must not alias.
  //
  // `seed()` links decision 20 to TASK 100 via `cites`, not to plan 1 via
  // `derives-from` -- the planning-visible edge is added here, additively.
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);
  exec(conn, "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values "
             "('decision', 20, 'plan', 1, 'derives-from')");

  auto packet = pk::assemble_planning(conn, pk::planning_role::ingestor, plan_id);
  REQUIRE(packet.has_value());
  REQUIRE(packet->input.decisions.size() == 1);
  REQUIRE(packet->input.constraints.size() == 1);
  CHECK(packet->input.decisions[0].id == packet->input.constraints[0].id);
  CHECK(packet->input.decisions[0].text == packet->input.constraints[0].text);
}

TEST_CASE("compile_planning: canonical body key order and digest/canonical split", "[packet][planning]") {
  pk::planning_input input;
  input.role                         = pk::planning_role::orchestrator;
  input.goal                         = "Ship the thing.";
  input.scope_facts                  = {current_evidence("scope")};
  input.scope_facts[0].display_label = "Renamed Plan";
  const auto packet                  = pk::compile_planning(input);

  CHECK(packet.canonical.starts_with(R"({"policy":"routing-packet-v2","role":"orchestrator","goal":"Ship the thing.")"));
  CHECK(packet.canonical.find(R"("review_rubric_version":"")") != std::string::npos);
  CHECK(packet.canonical.find("display_label") != std::string::npos);

  // The digest is taken over the SEMANTIC form, which omits display_label --
  // a display-only rename must not move it.
  auto renamed                         = input;
  renamed.scope_facts[0].display_label = "A Completely Different Name";
  const auto renamed_packet            = pk::compile_planning(renamed);
  CHECK(renamed_packet.digest == packet.digest);
  CHECK(renamed_packet.canonical != packet.canonical);
}

TEST_CASE("assemble_planning: the milestone-plan walk reaches a CHILD plan's tasks, and matches test_spec_status",
          "[packet][planning]") {
  // Review finding (task 6343 iteration 2, BLOCKING 2): `seed()` puts every
  // task directly on the anchor plan, so `milestone_task_count`'s
  // `or plan_id in (select ... relationship='derives-from')` branch and its
  // twin in `milestone_tasks_covered_count` never executed under any
  // existing case -- a mutated `derives-from` there killed nothing. This
  // fixture adds a genuine CHILD milestone plan the anchor does not own
  // directly, so the walk's `plan -> plan derives-from` arm is the only way
  // to reach it.
  namespace tss = planar::engine::planning::test_spec_status;

  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  // Plan 2: a child milestone of anchor plan 1, with its own two tasks and
  // its own scenario -- none of which `seed()` touches.
  exec(conn, "insert into plans (id, scope_kind, scope_id, title, slug, summary, status) "
             "values (2, 'global', null, 'Milestone one', 'pkt-m1', 'Milestone summary.', 'active')");
  exec(conn, "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values "
             "('plan', 2, 'plan', 1, 'derives-from')");
  exec(conn, "insert into tasks (id, scope_kind, scope_id, plan_id, title, status, priority, slug) values "
             "(300, 'global', null, 2, 'Milestone task, covered', 'todo', 100, 'pkt-m1-a'),"
             "(301, 'global', null, 2, 'Milestone task, uncovered', 'todo', 100, 'pkt-m1-b')");
  exec(conn, "insert into test_scenarios (id, scope_kind, scope_id, title, body, status) "
             "values (31, 'global', null, 'Milestone scenario', 'Scenario body.', 'ready')");
  exec(conn, "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values "
             "('test_scenario', 31, 'task', 300, 'verifies'),"
             "('test_scenario', 31, 'plan', 1, 'derives-from')");

  // Expected, by hand: total_tasks = 3 (plan 1: 100/101/200) + 2 (plan 2:
  // 300/301) = 5. tasks_covered = task 100 (scenario 30) + task 300
  // (scenario 31) = 2 -- task 300 is reachable ONLY through the
  // `derives-from` walk, since its own `plan_id` is 2, not the anchor.
  // total_scenarios = scenarios attached to the ANCHOR (30, 31) = 2.
  auto packet = pk::assemble_planning(conn, pk::planning_role::ingestor, plan_id);
  REQUIRE(packet.has_value());
  REQUIRE(packet->input.coverage.size() == 1);
  const auto& coverage = packet->input.coverage[0];
  CHECK(coverage.text == "tasks:5;covered:2;scenarios:2");
  CHECK(coverage.status == "incomplete");

  // The assertion that actually forbids the two copies from drifting apart:
  // `test_spec_status::compute` is the ORACLE-derived original this bucket
  // cannot call (D15/D18); its summary on the IDENTICAL fixture must agree
  // on all three counts with the duplicated queries above.
  auto oracle_status = tss::compute(conn, plan_id);
  REQUIRE(oracle_status.has_value());
  CHECK(oracle_status->summary.total_tasks == 5);
  CHECK(oracle_status->summary.tasks_covered == 2);
  CHECK(oracle_status->summary.total_scenarios == 2);
  CHECK(coverage.text == std::format("tasks:{};covered:{};scenarios:{}", oracle_status->summary.total_tasks,
                                     oracle_status->summary.tasks_covered, oracle_status->summary.total_scenarios));
}

// ===========================================================================
// Operator-staged routing facts (task 6048, decision 1102).
//
// `materialize::reconcile` rebuilds every task under an anchor plan and is
// reachable only from `spec ingest --apply`. So a hand-filed task could never
// obtain routing facts, and — the case that actually bit — a task whose body
// an operator EDITED could never restage the facts that edit invalidated.
// `stage_one_task` is the single-task, operator-provenance counterpart.
//
// These cases assert the three properties that make it safe rather than
// merely convenient: it restores readiness, it does not reach past the one
// task it was given, and it does NOT buy that by making operator facts
// permanently fresh.
// ===========================================================================

namespace {

namespace mz = planar::engine::ingest::materialize;

/// @brief SQL single-quote escaping for a literal embedded in `exec`.
auto escaped(std::string_view text) -> std::string {
  std::string out;
  out.reserve(text.size());
  for (const char c : text) {
    if (c == '\'') {
      out += "''";
    } else {
      out += c;
    }
  }
  return out;
}

/// @brief Reason names joined for an `INFO` line.
auto join(const std::vector<std::string>& names) -> std::string {
  std::string out;
  for (const auto& name : names) {
    if (!out.empty()) {
      out += ", ";
    }
    out += name;
  }
  return out;
}

/// @brief Every fact the packet considers stale, as "kind@locator".
///
/// A bare `stale_fact` reason names nothing, which makes a failure here
/// unreadable; this is what turns it into a diagnosis.
auto stale_facts(const pk::task_packet& packet) -> std::string {
  std::string out;
  for (const auto& fact : packet.input.facts) {
    if (fact.freshness != "current") {
      if (!out.empty()) {
        out += ", ";
      }
      out += std::format("{}@{} [{}->{}]", fact.kind, fact.locator, fact.source_digest.substr(0, 8),
                         fact.current_digest.empty() ? std::string{"<none>"} : fact.current_digest.substr(0, 8));
    }
  }
  return out;
}

/// @brief Every CITATION the packet considers stale, as "locator".
///
/// `input.citations` and `input.facts` are loaded by two different functions
/// with two different digest comparisons, and a `cited_artifact_section` row
/// appears in BOTH. Asserting on `input.facts` therefore exercises only
/// `fact_evidence` and leaves `citation_evidence` uncovered — which is exactly
/// how a probe that deleted the latter's digest clause survived once.
auto stale_citations(const pk::task_packet& packet) -> std::string {
  std::string out;
  for (const auto& citation : packet.input.citations) {
    if (citation.freshness != "current") {
      if (!out.empty()) {
        out += ", ";
      }
      out += citation.locator;
    }
  }
  return out;
}

/// @brief The `freshness` a packet reports for one fact kind.
///
/// Asserted directly because readiness carries its OWN digest comparison, so
/// `stale_fact` still fires when the freshness FIELD is wrong. A break-probe
/// that deletes the digest clause from `fact_evidence` survives every
/// reason-level assertion and is caught only here — the field is what
/// `task packet --json` shows an operator.
auto freshness_of(const pk::task_packet& packet, std::string_view kind) -> std::string {
  for (const auto& fact : packet.input.facts) {
    if (fact.kind == kind) {
      return fact.freshness;
    }
  }
  return "<absent>";
}

/// @brief One scalar integer from a query that returns exactly one row.
auto row_count(planar::db::connection& conn, std::string_view sql) -> std::int64_t {
  auto stmt = conn.prepare(sql);
  REQUIRE(stmt.has_value());
  auto stepped = stmt->step();
  REQUIRE(stepped.has_value());
  REQUIRE(*stepped == planar::db::step_result::row);
  return stmt->column_int64(0);
}

/// @brief The seeded ready task's body, plus the explicit artifact references
/// `stage_one_task` needs to resolve citation facts.
///
/// The seed links task 100 to four artifacts through `entity_links` but its
/// body names none of them, because ingest staged those citation facts from
/// parsed roadmap provenance rather than from the body. An operator staging
/// the same task has only the body to go on — which is the contract: the verb
/// resolves citations the author wrote, and never invents one.
constexpr std::string_view cited_body = "Implement the packet compiler.\n"
                                        "\n"
                                        "## Acceptance Criteria\n"
                                        "The packet compiles with zero readiness reasons.\n"
                                        "\n"
                                        "## Required validation\n"
                                        "cmake --build build/debug\n"
                                        "\n"
                                        "## Citations\n"
                                        "artifact:10#Overview\n"
                                        "artifact:11#Overview\n"
                                        "artifact:12#Overview\n"
                                        "artifact:13#Overview\n";

/// @brief Every stored materializer version for a task's facts, deduplicated.
auto fact_versions(planar::db::connection& conn, std::int64_t task_id) -> std::set<std::string> {
  auto stmt = conn.prepare("select distinct materializer_version from routing_task_facts where task_id=? order by 1");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->bind_int64(1, task_id).has_value());
  std::set<std::string> out;
  while (true) {
    auto stepped = stmt->step();
    REQUIRE(stepped.has_value());
    if (*stepped != planar::db::step_result::row) {
      break;
    }
    out.insert(stmt->column_text(0));
  }
  return out;
}

} // namespace

TEST_CASE("an operator edit strands a task's facts, and staging restores them", "[packet][6048]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  // Give the body its explicit citations. The acceptance section is byte
  // identical, so its digest does not move and the fixture stays ready —
  // proving the added text is not itself what breaks or fixes anything.
  exec(conn, std::format("update tasks set body='{}' where id={}", escaped(cited_body), ready_task));
  {
    auto packet = pk::assemble_task(conn, ready_task);
    REQUIRE(packet.has_value());
    INFO("reasons: " << join(reason_names(*packet)));
    CHECK(packet->ready());
  }

  // THE MOTIVATING CASE: edit the acceptance criteria. The stored digest no
  // longer matches the live section, so the fact is stale and the task is
  // undispatchable — with no operator-reachable way to restage it.
  exec(conn, std::format("update tasks set body='{}' where id={}",
                         escaped("Implement the packet compiler.\n"
                                 "\n"
                                 "## Acceptance Criteria\n"
                                 "The packet compiles with zero readiness reasons, and says so.\n"
                                 "\n"
                                 "## Required validation\n"
                                 "cmake --build build/debug\n"
                                 "\n"
                                 "## Citations\n"
                                 "artifact:10#Overview\n"
                                 "artifact:11#Overview\n"
                                 "artifact:12#Overview\n"
                                 "artifact:13#Overview\n"),
                         ready_task));
  {
    auto packet = pk::assemble_task(conn, ready_task);
    REQUIRE(packet.has_value());
    CHECK_FALSE(packet->ready());
    auto const names = reason_names(*packet);
    CHECK(std::ranges::find(names, "stale_fact") != names.end());
  }

  // Staging restores readiness, and the facts now carry operator provenance
  // rather than silently claiming to be ingest output.
  auto staged = mz::stage_one_task(conn, ready_task);
  REQUIRE(staged.has_value());
  {
    auto packet = pk::assemble_task(conn, ready_task);
    REQUIRE(packet.has_value());
    INFO("reasons after staging: " << join(reason_names(*packet)));
    INFO("stale: " << stale_facts(*packet));
    CHECK(packet->ready());
    // Both halves of the two-materializer rule: a recognised version AND a
    // matching digest make the fact current. A citation fact travels the
    // separate `citation_evidence` path, so pin one of each.
    CHECK(freshness_of(*packet, "acceptance_complete") == "current");
    CHECK(freshness_of(*packet, "cited_artifact_section") == "current");
  }
  CHECK(fact_versions(conn, ready_task) == std::set<std::string>{"operator-v1"});
}

TEST_CASE("an operator-staged fact still goes stale when its source changes", "[packet][6048]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);
  exec(conn, std::format("update tasks set body='{}' where id={}", escaped(cited_body), ready_task));
  REQUIRE(mz::stage_one_task(conn, ready_task).has_value());
  REQUIRE(pk::assemble_task(conn, ready_task)->ready());

  // The freshness rule accepts `operator-v1` as a version routing understands.
  // It must NOT also stop comparing digests: a fact that can never go stale is
  // a weaker contract than the one ingest facts carry, and would let an
  // operator certify text that has since changed underneath them.
  exec(conn, std::format("update tasks set next_action='{}' where id={}", escaped("Something else entirely."), ready_task));
  auto packet = pk::assemble_task(conn, ready_task);
  REQUIRE(packet.has_value());
  CHECK_FALSE(packet->ready());
  auto const names = reason_names(*packet);
  CHECK(std::ranges::find(names, "stale_fact") != names.end());
  // The FIELD, not only the reason: readiness compares digests itself, so the
  // reason fires either way and would hide a freshness rule that had stopped
  // comparing them.
  CHECK(freshness_of(*packet, "next_action_exact") == "stale");
}

TEST_CASE("staging one task does not disturb a sibling's facts", "[packet][6048]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);
  exec(conn, std::format("update tasks set body='{}' where id={}", escaped(cited_body), ready_task));

  // A sibling under the same plan, carrying ingest facts of its own. A
  // plan-wide reconcile deletes every fact below its anchor; if the
  // single-task path reused that delete, staging task 100 would silently
  // discard task 101's facts and the only symptom would be a sibling that
  // stopped being dispatchable for no visible reason.
  exec(conn, "insert into routing_task_facts (task_id, fact_kind, value_type, value_bool, source_entity_kind, "
             "source_entity_id, source_locator, source_digest, materializer_version) values "
             "(101,'acceptance_complete','bool',1,'task',101,'body#acceptance-criteria','deadbeef','spec-ingest-v1')");

  REQUIRE(mz::stage_one_task(conn, ready_task).has_value());

  CHECK(fact_versions(conn, 101) == std::set<std::string>{"spec-ingest-v1"});
  CHECK(row_count(conn, "select count(*) from routing_task_facts where task_id=101") == 1);
}

TEST_CASE("staging refuses an id no task has", "[packet][6048]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);
  auto staged = mz::stage_one_task(conn, 999999);
  REQUIRE_FALSE(staged.has_value());
  CHECK(staged.error().kind_ == mz::materialize_error_kind::task_not_found);
}

TEST_CASE("staging reports an unresolvable citation and stages nothing", "[packet][6048]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  // The seed's `cites` edges are intact but the body names no artifact, so no
  // locator resolves. The verb must say which task cites which artifact rather
  // than inventing a citation to satisfy the gate — and must leave the stored
  // facts alone, so a failed stage is not also a destructive one.
  auto staged = mz::stage_one_task(conn, ready_task);
  REQUIRE_FALSE(staged.has_value());
  CHECK(staged.error().kind_ == mz::materialize_error_kind::invalid_citation);
  CHECK(staged.error().citations_.size() == 4);
  CHECK(fact_versions(conn, ready_task) == std::set<std::string>{"spec-ingest-v1"});
}

TEST_CASE("the six fact-sourced reasons still apply to a task with no facts", "[packet][6048]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  // Non-vacuity for the whole feature: accepting a second materializer version
  // must not soften grading for a task that has staged nothing at all.
  auto packet = pk::assemble_task(conn, 200);
  REQUIRE(packet.has_value());
  auto const names = reason_names(*packet);
  for (auto const* expected : {"missing_product_spec", "missing_tech_spec", "missing_roadmap", "missing_test_spec",
                               "missing_acceptance_fact", "missing_next_action_fact"}) {
    INFO("expected reason: " << expected);
    CHECK(std::ranges::find(names, expected) != names.end());
  }
}

TEST_CASE("an operator-staged citation goes stale when the cited artifact moves", "[packet][6048]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);
  exec(conn, std::format("update tasks set body='{}' where id={}", escaped(cited_body), ready_task));
  REQUIRE(mz::stage_one_task(conn, ready_task).has_value());
  REQUIRE(pk::assemble_task(conn, ready_task)->ready());

  // Citation facts travel `citation_evidence`, a different loader from the one
  // the sibling case covers, with its own digest comparison. Editing the CITED
  // ARTIFACT (not the task) is the only thing that exercises it: the operator
  // certified a section that has since been rewritten underneath them, and
  // must be told rather than dispatched on stale evidence.
  exec(conn, "update artifacts set body='## Overview\nRewritten after the operator staged it.\n' where id=10");

  auto packet = pk::assemble_task(conn, ready_task);
  REQUIRE(packet.has_value());
  INFO("stale facts: " << stale_facts(*packet));
  INFO("stale citations: " << stale_citations(*packet));
  CHECK_FALSE(packet->ready());
  CHECK(stale_citations(*packet).contains("artifact:10#Overview"));
}

TEST_CASE("compile_planning: an OPTIONAL row never blocks readiness, however bad it is", "[packet][planning]") {
  // `evidence::required` defaults to true and no fixture had ever set it
  // false, so the distinction that gates dispatch was untested at EVERY
  // site: spec sections, decisions, coverage, citations and scenarios each
  // guard their checks with `value.required`, and a sweep (task 6782) found
  // all five clauses droppable with the suite still green.
  //
  // That direction matters more than it looks. `ready: false` is a hard
  // stop in the orchestrator contract, so a required-check that stopped
  // being read would let ADVISORY evidence -- a stale optional citation, an
  // uncovered optional scenario -- hold up work that is genuinely ready.
  const auto optional_but_broken = [](std::string_view kind) {
    pk::evidence item   = current_evidence(kind);
    item.required       = false;
    item.current_digest = "drifted";  // not current
    item.covered        = false;      // not covered
    item.status         = "rejected"; // not an accepted status
    return item;
  };

  pk::planning_input input;
  input.role        = pk::planning_role::orchestrator;
  input.goal        = "Ship the thing.";
  input.scope_facts = {current_evidence("scope")};

  const auto baseline = pk::compile_planning(input);
  REQUIRE(baseline.ready());

  SECTION("an optional stale, rejected artifact does not block") {
    input.artifacts = {optional_but_broken("product_spec")};
    CHECK(pk::compile_planning(input).ready());
  }

  SECTION("an optional stale, rejected decision does not block") {
    input.decisions = {optional_but_broken("decision")};
    CHECK(pk::compile_planning(input).ready());
  }
}

TEST_CASE("compile_task: an OPTIONAL citation or scenario raises no reason", "[packet]") {
  // The task-packet half of the same gap. `citation.required` and
  // `scenario.required` each guard their reason, and neither had a fixture
  // that set `required = false` -- so a mutant dropping either made an
  // ADVISORY row emit `unresolved_citation` / `uncovered_required_scenario`
  // and hold up a task that is genuinely dispatchable.
  pk::task_input input;

  pk::evidence citation   = {};
  citation.kind           = "citation";
  citation.required       = false;
  citation.locator        = "artifact:1"; // no `#Section`, so not a section locator
  citation.source_digest  = "same";
  citation.current_digest = "drifted"; // and not current either
  input.citations         = {citation};

  pk::evidence scenario = {};
  scenario.kind         = "scenario";
  scenario.required     = false;
  scenario.covered      = false;
  input.scenarios       = {scenario};

  const auto names = reason_names(pk::compile_task(input));
  INFO("reasons: " << std::format("{}", names));
  CHECK(std::ranges::find(names, "unresolved_citation") == names.end());
  CHECK(std::ranges::find(names, "uncovered_required_scenario") == names.end());
}
