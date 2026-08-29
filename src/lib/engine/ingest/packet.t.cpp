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
import planar.engine.ingest.packet;

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
/// dependency (101), and one bare task (200) on the same active plan.
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
  exec(conn, "insert into decisions (id, scope_kind, scope_id, title, body, status, slug) "
             "values (20, 'global', null, 'Locked', 'Decision body.', 'accepted', 'pkt-dec')");
  exec(conn, "insert into test_scenarios (id, scope_kind, scope_id, title, body, status, slug) "
             "values (30, 'global', null, 'Scenario', 'Scenario body.', 'ready', 'pkt-scn')");
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
                                             "missing_dependency",
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

TEST_CASE("a root task cannot satisfy missing_dependency", "[packet]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  // Planar task 6048 is an OPEN QUESTION arguing this is the wrong policy.
  // This case pins the MEASURED behaviour so that changing it is a deliberate
  // act with a test to update, not a silent drift. Do not "fix" this here.
  auto packet = pk::assemble_task(conn, bare_task);
  REQUIRE(packet.has_value());
  const auto names = reason_names(*packet);
  CHECK(std::ranges::find(names, "missing_dependency") != names.end());
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
    exec(conn, "insert into questions (id, scope_kind, scope_id, title, body, status, slug) "
               "values (40,'global',null,'Q','Q body.','open','pkt-q')");
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
    exec(conn, "insert into questions (id, scope_kind, scope_id, title, body, status, answer_body, answered_at, slug) "
               "values (40,'global',null,'Q','Q body.','answered','A','2020-01-01T00:00:00.000Z','pkt-q')");
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
                                             "missing_dependency",
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
  CHECK(json.starts_with(R"({"policy_version":"routing-packet-v1","ready":true,"input":{)"));
  CHECK(json.find(R"("reasons":[]})") != std::string::npos);
  CHECK(json.ends_with("\n"));
  // `policy` appears again INSIDE the escaped canonical body; the two are not
  // the same field and both are contract.
  CHECK(json.find(R"(\"policy\":\"routing-packet-v1\")") != std::string::npos);

  auto blocked = pk::assemble_task(conn, bare_task);
  REQUIRE(blocked.has_value());
  const auto blocked_json = pk::render_json(*blocked);
  CHECK(blocked_json.find(R"("ready":false)") != std::string::npos);
  CHECK(blocked_json.find(R"("reasons":["missing_body","missing_acceptance_section")") != std::string::npos);
}
