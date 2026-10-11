// @file packet.t.cpp
// @brief Engine-level tests for `planar.engine.ingest.packet`.
//
// ## BOTH ARMS, OR THE SUITE PROVES NOTHING
//
// An implementation that ALWAYS refuses satisfies every `not_ready` assertion
// anyone would naturally write, and a fixture that is one row short of ready
// looks exactly like a correct implementation refusing correctly.
//
// So the fixture below is built READY FIRST. `assemble_task` on the untouched
// arena returns a packet with an EMPTY reasons list, and every other case is
// that same arena with ONE thing broken, usually paired with the repair that
// clears it. Each degraded case therefore asserts two things at once: that the
// reason fires, and (because the ready case is pinned separately) that it
// fires for the stated reason rather than because the fixture never worked.
//
// `ready_fixture_is_actually_ready` is deliberately the first case in the
// file. If it ever goes red, every case after it is meaningless.
//
// ## READINESS IS COORDINATION ONLY (plan 1142)
//
// The arena still carries cited specs, a scenario, a touch and an accepted
// decision, and a case strips every one of them: none affects `ready`. The
// reasons are the seven in packet.cppm, and one case pins their order.
//
// ## THE DIGESTS IN THE SEED ARE NOT DERIVED FROM THE CODE UNDER TEST
//
// A reference carries `source_digest("artifact", id, "body", body)`. Seeding the
// expected digests with `materialize::source_digest` would make the assertions
// circular. The four hex constants below were produced by Python's
// `hashlib.sha256` over the documented preimage, an implementation with no
// relationship to this tree, and each carries its preimage in a comment.
//
// ## REFERENCES, NOT FACTS
//
// The packet reads no `routing_task_facts` row. Its citations are references
// derived from the task's `cites` entity links, and `expand_reference` resolves
// one of them to text. Cases here seed links only, and a case asserts the
// seeded database holds no fact row at all.

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
/// Guarded rather than fire-and-forget: a seed statement that failed silently
/// would leave a test comparing an arena that was never built.
auto exec(planar::db::connection& conn, std::string_view sql) -> void {
  auto ok = conn.execute(sql);
  INFO("sql: " << sql);
  REQUIRE(ok.has_value());
}

// --- Digests, computed OUTSIDE this tree -----------------------------------
//
// Each constant is `sha256(kind \0 id \0 locator \0 body)` with the kind
// `artifact` and the locator `body`.

/// preimage: `artifact \0 10 \0 body \0 ## Overview\nSpec section body for product_spec.\n`
constexpr std::string_view d_product = "4064d6efbf4b7c5b0a2dfdf2ce7380106a0cf44c81e6267c2b107b46dc0a9dda";
/// preimage: `artifact \0 11 \0 body \0 ## Overview\nSpec section body for tech_spec.\n`
constexpr std::string_view d_tech = "159dd44bde29040d7a0a61595787420933250607cc3e5fc05d21e4344fcbc341";
/// preimage: `artifact \0 12 \0 body \0 ## Overview\nSpec section body for roadmap.\n`
constexpr std::string_view d_roadmap = "0d12c280ed11571d776b46510977e0bcdef2be7d789a1e28e7a398b04d901f2c";
/// preimage: `artifact \0 13 \0 body \0 ## Overview\nSpec section body for test_spec.\n`
constexpr std::string_view d_test_spec = "1d4e00864889660879350b33de5ef1ea9fd1f6de33c6bf3095364ac06e0fd34c";

constexpr std::int64_t ready_task = 100;
constexpr std::int64_t bare_task  = 200;
constexpr std::int64_t plan_id    = 1;

/// @brief Seed the shared arena: one READY task (100), its satisfied
/// dependency (101), and one bare root task (200) on the same active plan.
///
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
  CHECK(packet->input.owning_plans.size() == 1);
  CHECK(packet->input.references.size() == 4);
  CHECK(packet->input.dependencies.size() == 1);
  CHECK(packet->input.open_questions.empty());
  CHECK(packet->input.proposed_decisions.empty());
  CHECK(packet->input.claims.empty());
  CHECK(packet->digest.size() == 64);
}

TEST_CASE("a task created with nothing else is ready", "[packet]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  // Task 200 is what `task add 'Bare task'` leaves: no body, no next action, no
  // artifact, no decision, no scenario, no touch, no validation section.
  auto packet = pk::assemble_task(conn, bare_task);
  REQUIRE(packet.has_value());
  INFO("reasons: " << std::format("{}", reason_names(*packet)));
  CHECK(packet->reasons.empty());
  CHECK(packet->ready());
  CHECK(packet->input.references.empty());
}

TEST_CASE("spec documents, facts, scenarios and touches never affect readiness", "[packet]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  // Strip from the ready task everything the old readiness rules demanded: the
  // body and its sections, the next action, every cited spec, the decision, the
  // scenario links and the touch. Readiness does not depend on any of it.
  exec(conn, "update tasks set body=null, next_action=null where id=100");
  exec(conn, "delete from entity_links where (from_kind='task' and from_id=100 and to_kind in ('artifact','decision')) "
             "or (from_kind='test_scenario' and to_kind in ('task','plan'))");
  exec(conn, "delete from task_touch_paths where task_id=100");
  auto packet = pk::assemble_task(conn, ready_task);
  REQUIRE(packet.has_value());
  CHECK(packet->reasons.empty());
  CHECK(packet->input.references.empty());
}

TEST_CASE("the ready packet's references are derived from the cites links with independent digests", "[packet]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);
  auto packet = pk::assemble_task(conn, ready_task);
  REQUIRE(packet.has_value());

  // Ordered by artifact kind then id: product_spec, roadmap, tech_spec,
  // test_spec. Each carries the digest of the WHOLE body, computed outside
  // this tree, and the artifact's title as its one-line label.
  struct expected_ref {
    std::string_view ref;
    std::string_view kind;
    std::string_view digest;
    std::string_view label;
  };
  const std::array<expected_ref, 4> expected = {{{"artifact:10", "product_spec", d_product, "Product"},
                                                 {"artifact:12", "roadmap", d_roadmap, "Roadmap"},
                                                 {"artifact:11", "tech_spec", d_tech, "Tech"},
                                                 {"artifact:13", "test_spec", d_test_spec, "Tests"}}};
  REQUIRE(packet->input.references.size() == expected.size());
  for (std::size_t i = 0; i < expected.size(); ++i) {
    const auto& got = packet->input.references[i];
    INFO("reference " << got.ref);
    CHECK(got.ref == expected[i].ref);
    CHECK(got.kind == expected[i].kind);
    CHECK(got.locator == "body");
    CHECK(got.digest == expected[i].digest);
    CHECK(got.label == expected[i].label);
    CHECK(got.status == "active");
  }

  // No fact is stored, and none is read: the arena holds no fact row.
  auto stmt = conn.prepare("select count(*) from routing_task_facts");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->step().has_value());
  CHECK(stmt->column_int64(0) == 0);

  // The dependency is listed with the dependency task's own status.
  REQUIRE(packet->input.dependencies.size() == 1);
  CHECK(packet->input.dependencies[0].id == 101);
  CHECK(packet->input.dependencies[0].status == "done");
}

// ===========================================================================
// One axis broken at a time, against the arena the first case proved ready.
// ===========================================================================

TEST_CASE("editing a cited artifact moves its reference digest and the packet digest, not readiness", "[packet]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  auto before = pk::assemble_task(conn, ready_task);
  REQUIRE(before.has_value());
  auto packet = degraded(conn, "update artifacts set body='## Overview\nCHANGED.\n' where id=10");

  // A reference is read live, so there is nothing to go stale: the packet
  // stays ready and the change shows as a different digest.
  CHECK(packet->reasons.empty());
  CHECK(packet->digest != before->digest);
  REQUIRE(packet->input.references.size() == 4);
  CHECK(packet->input.references[0].ref == "artifact:10");
  CHECK(packet->input.references[0].digest != before->input.references[0].digest);
  CHECK(packet->input.references[1].digest == before->input.references[1].digest);
}

TEST_CASE("a retired cited artifact is an unresolved citation until the citation resolves", "[packet]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  auto packet = degraded(conn, "update artifacts set status='retired' where id=10");
  CHECK(reason_names(*packet) == std::vector<std::string>{"unresolved_citation"});

  // Repair: the citation resolves again and the reason clears.
  auto repaired = degraded(conn, "update artifacts set status='active' where id=10");
  CHECK(repaired->reasons.empty());
}

TEST_CASE("dropping a cites link drops the reference and raises nothing", "[packet]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  auto packet =
      degraded(conn, "delete from entity_links where from_kind='task' and from_id=100 and to_kind='artifact' and to_id=11");
  CHECK(packet->reasons.empty());
  CHECK(packet->input.references.size() == 3);
}

TEST_CASE("an unfinished dependency blocks the task until it is done or cancelled", "[packet]") {
  for (const std::string_view unfinished : {"todo", "doing", "blocked"}) {
    INFO("dependency status: " << unfinished);
    scratch_db_path scratch;
    auto            conn = open_migrated(scratch);
    seed(conn);

    auto packet = degraded(conn, std::format("update tasks set status='{}' where id=101", unfinished));
    CHECK(reason_names(*packet) == std::vector<std::string>{"blocked_by_open_task"});
    REQUIRE(packet->input.dependencies.size() == 1);
    CHECK(packet->input.dependencies[0].status == unfinished);

    // Each clears when the cause is resolved.
    CHECK(degraded(conn, "update tasks set status='done' where id=101")->reasons.empty());
    CHECK(degraded(conn, std::format("update tasks set status='{}' where id=101", unfinished))->reasons.size() == 1);
    CHECK(degraded(conn, "update tasks set status='cancelled' where id=101")->reasons.empty());
  }
}

TEST_CASE("a task that depends on itself is not blocked by it", "[packet]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  auto packet = degraded(conn, "insert into entity_links (from_kind,from_id,to_kind,to_id,relationship) values "
                               "('task',100,'task',100,'depends-on')");
  CHECK(packet->reasons.empty());
  CHECK(packet->input.blocking_dependencies.empty());
  CHECK(packet->input.dependencies.size() == 1);
}

TEST_CASE("only a direct depends-on link from the task blocks it", "[packet]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  // An unfinished task that depends ON the ready task, and one it is merely
  // related to, hold nothing back: the ready task's own dependency is `done`.
  exec(conn, "insert into tasks (id, scope_kind, scope_id, plan_id, title, status, priority, slug) values "
             "(102, 'global', null, 1, 'Dependent', 'todo', 100, 'pkt-dependent'),"
             "(103, 'global', null, 1, 'Related', 'todo', 100, 'pkt-related')");
  exec(conn, "insert into entity_links (from_kind,from_id,to_kind,to_id,relationship) values "
             "('task',102,'task',100,'depends-on'),"
             "('task',100,'task',103,'derives-from')");
  auto packet = pk::assemble_task(conn, ready_task);
  REQUIRE(packet.has_value());
  CHECK(packet->reasons.empty());

  // The dependent itself is blocked by the ready task only while that is open.
  auto dependent = pk::assemble_task(conn, 102);
  REQUIRE(dependent.has_value());
  CHECK(reason_names(*dependent) == std::vector<std::string>{"blocked_by_open_task"});
}

TEST_CASE("an open question linked to the task blocks it until the question is answered", "[packet]") {
  for (const auto* link : {"('task',100,'question',40,'addresses')", "('question',40,'task',100,'addresses')"}) {
    INFO("link: " << link);
    scratch_db_path scratch;
    auto            conn = open_migrated(scratch);
    seed(conn);
    exec(conn, "insert into questions (id, scope_kind, scope_id, title, body, status) "
               "values (40,'global',null,'Q','Q body.','open')");
    exec(conn, std::format("insert into entity_links (from_kind,from_id,to_kind,to_id,relationship) values {}", link));
    auto linked = pk::assemble_task(conn, ready_task);
    REQUIRE(linked.has_value());
    CHECK(reason_names(*linked) == std::vector<std::string>{"open_question_linked"});
    REQUIRE(linked->input.open_questions.size() == 1);
    CHECK(linked->input.open_questions[0].id == 40);

    // The paired presence case: the same link with an ANSWERED question leaves
    // the packet ready, so the assertion above is about the status and not
    // about linking a question at all.
    auto answered = degraded(conn, "update questions set status='answered', answer_body='A', "
                                   "answered_at='2020-01-01T00:00:00.000Z' where id=40");
    CHECK(answered->reasons.empty());
    CHECK(answered->input.open_questions.empty());
  }
}

TEST_CASE("a proposed decision linked to the task blocks it until the decision is accepted", "[packet]") {
  for (const auto* link : {"('task',100,'decision',41,'cites')", "('decision',41,'task',100,'cites')"}) {
    INFO("link: " << link);
    scratch_db_path scratch;
    auto            conn = open_migrated(scratch);
    seed(conn);
    exec(conn, "insert into decisions (id, scope_kind, scope_id, title, body, status) "
               "values (41,'global',null,'Pending','Pending body.','proposed')");
    exec(conn, std::format("insert into entity_links (from_kind,from_id,to_kind,to_id,relationship) values {}", link));
    auto linked = pk::assemble_task(conn, ready_task);
    REQUIRE(linked.has_value());
    CHECK(reason_names(*linked) == std::vector<std::string>{"proposed_decision_linked"});
    REQUIRE(linked->input.proposed_decisions.size() == 1);
    CHECK(linked->input.proposed_decisions[0].id == 41);

    auto accepted = degraded(conn, "update decisions set status='accepted' where id=41");
    CHECK(accepted->reasons.empty());
    CHECK(accepted->input.proposed_decisions.empty());
  }
}

TEST_CASE("a live claim blocks a doing task and a lapsed or stale one does not", "[packet]") {
  constexpr std::string_view session = "insert into sessions (id, vendor, started_at) "
                                       "values (1,'claude','2000-01-01T00:00:00.000Z')";
  const auto                 insert  = [](std::string_view status, std::string_view lease) {
    return std::format("insert into agent_work_claims (id, claim_token, session_id, entity_kind, entity_id, status, vendor, "
                       "lease_expires_at) values (1,'tok',1,'task',100,'{}','claude','{}')",
                       status, lease);
  };
  {
    scratch_db_path scratch;
    auto            conn = open_migrated(scratch);
    seed(conn);
    exec(conn, session);
    auto packet = degraded(conn, insert("active", "2999-01-01T00:00:00.000Z"));
    // A live lease always holds the task, whoever asks: `task packet` takes no
    // caller to compare the claim's holder against.
    CHECK(reason_names(*packet) == std::vector<std::string>{"invalid_task_status"});
    REQUIRE(packet->input.claims.size() == 1);
    CHECK(packet->input.claims[0].status == "active");
  }
  {
    scratch_db_path scratch;
    auto            conn = open_migrated(scratch);
    seed(conn);
    exec(conn, session);
    auto packet = degraded(conn, insert("active", "2000-01-01T00:00:00.000Z"));
    // The row is `status='active'`; only the LEASE has passed. The loader
    // re-derives `expired` from `lease_expires_at`, so the task is recoverable
    // and the evidence stays visible.
    CHECK(packet->reasons.empty());
    REQUIRE(packet->input.claims.size() == 1);
    CHECK(packet->input.claims[0].status == "expired");
  }
  {
    scratch_db_path scratch;
    auto            conn = open_migrated(scratch);
    seed(conn);
    exec(conn, session);
    // A reconcile-marked `stale` claim is not live whatever its lease says.
    auto packet = degraded(conn, insert("stale", "2999-01-01T00:00:00.000Z"));
    CHECK(packet->reasons.empty());
  }
  {
    // A doing task nobody holds (an operator set it by hand) is recoverable.
    scratch_db_path scratch;
    auto            conn = open_migrated(scratch);
    seed(conn);
    auto packet = pk::assemble_task(conn, ready_task);
    REQUIRE(packet.has_value());
    CHECK(packet->input.status == "doing");
    CHECK(packet->reasons.empty());
  }
}

TEST_CASE("a task that is not todo or doing is an invalid status for dispatch", "[packet]") {
  for (const std::string_view closed : {"done", "cancelled", "blocked"}) {
    INFO("status: " << closed);
    scratch_db_path scratch;
    auto            conn = open_migrated(scratch);
    seed(conn);

    auto packet = degraded(conn, std::format("update tasks set status='{}' where id=100", closed));
    CHECK(reason_names(*packet) == std::vector<std::string>{"invalid_task_status"});
    // Reopened, it is ready again.
    CHECK(degraded(conn, "update tasks set status='todo' where id=100")->reasons.empty());
  }
}

TEST_CASE("an orphaned task is missing its owning plan and nothing else", "[packet]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  auto packet = degraded(conn, "update tasks set plan_id=null where id=100");
  CHECK(reason_names(*packet) == std::vector<std::string>{"missing_owning_plan"});
}

TEST_CASE("a draft owning plan does not hold the task back", "[packet]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  CHECK(degraded(conn, "update plans set status='draft' where id=1")->reasons.empty());
}

TEST_CASE("every reason a task trips is reported once, in the documented order", "[packet]") {
  // Trip all seven through SQL on one task: no title cannot be seeded through
  // `tasks.title not null`-style SQL without being blank, so a space stands in.
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);
  exec(conn, "insert into sessions (id, vendor, started_at) values (1,'claude','2000-01-01T00:00:00.000Z')");
  exec(conn, "insert into agent_work_claims (id, claim_token, session_id, entity_kind, entity_id, status, vendor, "
             "lease_expires_at) values (1,'tok',1,'task',100,'active','claude','2999-01-01T00:00:00.000Z')");
  exec(conn, "update tasks set title=' ', plan_id=null where id=100");
  exec(conn, "update artifacts set status='retired' where id=10");
  exec(conn, "update tasks set status='doing' where id=101");
  exec(conn, "insert into questions (id, scope_kind, scope_id, title, body, status) values (40,'global',null,'Q','B','open')");
  exec(conn,
       "insert into decisions (id, scope_kind, scope_id, title, body, status) values (41,'global',null,'D','B','proposed')");
  exec(conn, "insert into entity_links (from_kind,from_id,to_kind,to_id,relationship) values "
             "('task',100,'question',40,'addresses'),('task',100,'decision',41,'cites')");

  auto packet = pk::assemble_task(conn, ready_task);
  REQUIRE(packet.has_value());
  const std::vector<std::string> expected = {"missing_title",           "invalid_task_status",  "missing_owning_plan",
                                             "unresolved_citation",     "blocked_by_open_task", "open_question_linked",
                                             "proposed_decision_linked"};
  CHECK(reason_names(*packet) == expected);
}

TEST_CASE("the reason vocabulary is exactly the seven coordination reasons", "[packet]") {
  // The removed spec-quality, fact, evidence-class and claim-evidence reasons
  // have no enumerator, so there is no spelling left to emit.
  const std::vector<std::string> expected = {"missing_title",           "invalid_task_status",  "missing_owning_plan",
                                             "unresolved_citation",     "blocked_by_open_task", "open_question_linked",
                                             "proposed_decision_linked"};
  std::vector<std::string>       names;
  for (std::uint8_t i = 0; i < 16; ++i) {
    const auto name = pk::reason_name(static_cast<pk::readiness_reason>(i));
    if (name != "unknown") {
      names.emplace_back(name);
    }
  }
  CHECK(names == expected);
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

TEST_CASE("the envelope carries the digest and no canonical body", "[packet][digest]") {
  const auto packet = pk::compile_task(pk::task_input{});
  CHECK(packet.canonical.starts_with("{\"policy\":\"routing-packet-v3\",\"task_id\":"));
  const auto json = pk::render_json(packet);
  CHECK(json.find("\"digest\":\"") != std::string::npos);
  CHECK(json.find("\"canonical\":") == std::string::npos);
}

TEST_CASE("the canonical body ignores evidence input order", "[packet]") {
  // Reached through the PURE compiler, because SQL always returns the loaders'
  // `order by` and cannot produce a permuted input at all.
  const auto make = [](std::string_view sd, std::string_view kind, std::int64_t id) {
    pk::reference r;
    r.ref    = std::format("artifact:{}", id);
    r.kind   = kind;
    r.id     = id;
    r.digest = std::string{sd};
    r.label  = std::format("label {}", id);
    return r;
  };
  const auto a = make("a", "product_spec", 2);
  const auto b = make("b", "tech_spec", 3);
  const auto c = make("c", "roadmap", 4);
  const auto d = make("d", "test_spec", 5);

  pk::task_input forward;
  forward.task_id     = 42;
  forward.status      = "doing";
  forward.title       = R"(Exact "title"; $(still data))";
  forward.body        = "Body.\n## Acceptance Criteria\nReal criteria.\n";
  forward.next_action = "Do the specific thing.";
  forward.references  = {a, b, c, d};

  auto reversed       = forward;
  reversed.references = {d, c, b, a};

  const auto one = pk::compile_task(forward);
  const auto two = pk::compile_task(reversed);
  CHECK(one.digest == two.digest);
  CHECK(one.canonical == two.canonical);

  // Adversarial text stays DATA: the quote and the command substitution are
  // escaped into the JSON string rather than terminating it.
  CHECK(one.canonical.find(R"(\"title\"; $(still data))") != std::string::npos);
}

TEST_CASE("a reference's label is display-only: it moves the canonical body, not the digest", "[packet]") {
  pk::reference ref;
  ref.ref    = "artifact:2";
  ref.kind   = "tech_spec";
  ref.id     = 2;
  ref.digest = "d";
  ref.label  = "Old title";

  pk::task_input first;
  first.references            = {ref};
  auto renamed                = first;
  renamed.references[0].label = "New title";

  const auto one = pk::compile_task(first);
  const auto two = pk::compile_task(renamed);
  CHECK(one.digest == two.digest);
  CHECK(one.canonical != two.canonical);

  auto changed                 = first;
  changed.references[0].digest = "e";
  CHECK(pk::compile_task(changed).digest != one.digest);
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
  low.status         = "resolved";
  low.provenance     = "artifact:1";

  auto high           = low;
  high.current_digest = "b";
  high.status         = "ready";
  high.provenance     = "artifact:2";

  pk::task_input forward;
  forward.proposed_decisions  = {low, high};
  auto reversed               = forward;
  reversed.proposed_decisions = {high, low};

  CHECK(pk::compile_task(forward).canonical == pk::compile_task(reversed).canonical);
}

TEST_CASE("an empty input names every reason that applies to it", "[packet]") {
  // The pure compiler on a fully empty input: no title, a status that is
  // neither todo nor doing, no plan. It pins that the head of the emission
  // order is what packet.cppm documents.
  const auto                     packet   = pk::compile_task(pk::task_input{});
  const std::vector<std::string> expected = {"missing_title", "invalid_task_status", "missing_owning_plan"};
  CHECK(reason_names(packet) == expected);
}

TEST_CASE("compile_task decides the coordination reasons from the input alone", "[packet]") {
  pk::evidence plan;
  plan.kind   = "plan";
  plan.id     = 1;
  plan.status = "active";

  pk::task_input input;
  input.task_id      = 7;
  input.status       = "todo";
  input.title        = "Title";
  input.owning_plans = {plan};
  CHECK(pk::compile_task(input).ready());

  // The blocking verdict is the coordination predicate's list; the display
  // evidence alone decides nothing.
  pk::evidence dependency;
  dependency.kind    = "dependency";
  dependency.id      = 8;
  dependency.status  = "doing";
  input.dependencies = {dependency};
  CHECK(pk::compile_task(input).ready());
  input.blocking_dependencies = {8};
  CHECK(reason_names(pk::compile_task(input)) == std::vector<std::string>{"blocked_by_open_task"});
  input.dependencies.clear();
  input.blocking_dependencies.clear();

  // A live claim holds a doing task; an expired one does not.
  pk::evidence claim;
  claim.kind   = "claim";
  claim.id     = 1;
  claim.status = "active";
  input.status = "doing";
  input.claims = {claim};
  CHECK(reason_names(pk::compile_task(input)) == std::vector<std::string>{"invalid_task_status"});
  input.claims[0].status = "expired";
  CHECK(pk::compile_task(input).ready());
}

TEST_CASE("render_text names every reason and ends with the canonical body", "[packet]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);
  exec(conn, "update tasks set status='done' where id=200");

  auto blocked = pk::assemble_task(conn, bare_task);
  REQUIRE(blocked.has_value());
  const auto text = pk::render_text(*blocked);
  CHECK(text.starts_with(std::format("task packet {}: not_ready\n", bare_task)));
  CHECK(text.find(std::format("digest: {}\n", blocked->digest)) != std::string::npos);
  CHECK(text.find("reasons:\n- invalid_task_status\n") != std::string::npos);
  CHECK(text.ends_with(blocked->canonical + "\n"));
  // A task citing nothing prints no references block.
  CHECK(text.find("references (expand") == std::string::npos);

  auto ready = pk::assemble_task(conn, ready_task);
  REQUIRE(ready.has_value());
  const auto ready_text = pk::render_text(*ready);
  CHECK(ready_text.starts_with(std::format("task packet {}: ready\n", ready_task)));
  // A ready packet emits NO `reasons:` block at all, not an empty one.
  CHECK(ready_text.find("reasons:") == std::string::npos);
  // The references block lists ref, a 12-character digest prefix and the label
  // per citation, and says how to expand one. It carries no cited text.
  CHECK(ready_text.find(std::format("references (expand one with: planar task packet {} --expand <ref>):\n", ready_task)) !=
        std::string::npos);
  CHECK(ready_text.find(std::format("- artifact:10 {} Product\n", std::string{d_product.substr(0, 12)})) != std::string::npos);
  CHECK(ready_text.find(std::format("- artifact:13 {} Tests\n", std::string{d_test_spec.substr(0, 12)})) != std::string::npos);
  CHECK(ready_text.find("Spec section body for") == std::string::npos);
  CHECK(ready_text.find("routing_task_facts") == std::string::npos);
}

TEST_CASE("render_json carries the policy version in the envelope", "[packet]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  auto ready = pk::assemble_task(conn, ready_task);
  REQUIRE(ready.has_value());
  const auto json = pk::render_json(*ready);

  // The version rides in the envelope so a consumer can establish that two
  // packets are comparable before reading `input`.
  CHECK(json.starts_with(R"({"policy_version":"routing-packet-v3","ready":true,"input":{)"));
  CHECK(json.find(R"("reasons":[]})") != std::string::npos);
  CHECK(json.ends_with("\n"));
  // The envelope carries no escaped `canonical` copy of the policy.
  CHECK(json.find(R"(\"policy\":\"routing-packet-v3\")") == std::string::npos);

  exec(conn, "update tasks set status='todo' where id=101");
  exec(conn, "insert into questions (id, scope_kind, scope_id, title, body, status) values (40,'global',null,'Q','B','open')");
  exec(conn,
       "insert into entity_links (from_kind,from_id,to_kind,to_id,relationship) values ('task',100,'question',40,'addresses')");
  auto blocked = pk::assemble_task(conn, ready_task);
  REQUIRE(blocked.has_value());
  const auto blocked_json = pk::render_json(*blocked);
  CHECK(blocked_json.find(R"("ready":false)") != std::string::npos);
  CHECK(blocked_json.find(R"("reasons":["blocked_by_open_task","open_question_linked"]})") != std::string::npos);
  // None of the removed fields is part of the input any more.
  for (std::string_view gone :
       {"acceptance_criteria", "anchor_plans", "\"decisions\"", "\"scenarios\"", "\"touches\"", "validation_gates"}) {
    INFO("field: " << gone);
    CHECK(blocked_json.find(gone) == std::string::npos);
  }
}

// ===========================================================================
// JSON shape, references, expansion and size (plan 1142).
// ===========================================================================

TEST_CASE("render_json lists references in place of citations and facts", "[packet][references]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  auto ready = pk::assemble_task(conn, ready_task);
  REQUIRE(ready.has_value());
  const auto json = pk::render_json(*ready);
  CHECK(json.find(R"("citations":)") == std::string::npos);
  CHECK(json.find(R"("facts":)") == std::string::npos);
  CHECK(json.find(R"("text":"## Overview)") == std::string::npos);
  // The key order of one reference is contract: ref, kind, id, locator,
  // digest, label, status.
  const auto first = std::format(
      R"({{"ref":"artifact:10","kind":"product_spec","id":10,"locator":"body","digest":"{}","label":"Product","status":"active"}})",
      d_product);
  CHECK(json.find(R"("references":[)" + first + ",") != std::string::npos);
  // `references` sits after the owning plan and before the dependencies.
  CHECK(json.find(R"("owning_plans":)") < json.find(R"("references":)"));
  CHECK(json.find(R"("references":)") < json.find(R"("dependencies":)"));
}

TEST_CASE("an evidence row carries no fact-era fields, and the digest ignores its display label", "[packet]") {
  pk::evidence dependency;
  dependency.kind          = "dependency";
  dependency.id            = 8;
  dependency.locator       = "task:8";
  dependency.text          = "T";
  dependency.display_label = "Eight";
  dependency.status        = "done";

  pk::task_input input;
  input.dependencies = {dependency};
  const auto packet  = pk::compile_task(input);

  const std::string row =
      R"({"kind":"dependency","id":8,"locator":"task:8","text":"T","display_label":"Eight","source_digest":"","current_digest":"","status":"done","provenance":"planar"})";
  CHECK(packet.canonical.find(R"("dependencies":[)" + row + "]") != std::string::npos);
  CHECK(pk::render_json(packet).find(R"("dependencies":[)" + row + "]") != std::string::npos);
  for (const std::string_view gone : {"\"required\"", "\"covered\"", "materializer_version", "\"freshness\""}) {
    INFO("field: " << gone);
    CHECK(packet.canonical.find(gone) == std::string::npos);
    CHECK(pk::render_json(packet).find(gone) == std::string::npos);
  }

  // The digest is over the semantic body: the label is display-only.
  auto renamed                          = input;
  renamed.dependencies[0].display_label = "Renamed";
  const auto other                      = pk::compile_task(renamed);
  CHECK(other.digest == packet.digest);
  CHECK(other.canonical != packet.canonical);
  CHECK(packet.canonical.find("display_label") != std::string::npos);
}

TEST_CASE("expand_reference prints exactly the cited section or the whole body", "[packet][references]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);
  exec(conn, "update artifacts set body='## Overview\nOverview text.\n\n### Detail\nNested detail.\n\n## Other\nOther text.\n' "
             "where id=11");

  // The section under a heading, nested subsections included, and nothing past
  // the next heading of the same or shallower level.
  auto section = pk::expand_reference(conn, ready_task, "artifact:11#Overview");
  REQUIRE(section.has_value());
  CHECK(section->text == "Overview text.\n\n### Detail\nNested detail.");
  CHECK(section->ref == "artifact:11#Overview");
  CHECK(pk::render_expansion_text(*section) == "Overview text.\n\n### Detail\nNested detail.\n");
  CHECK(
      pk::render_expansion_json(*section) ==
      std::format(
          "{{\"ref\":\"artifact:11#Overview\",\"digest\":\"{}\",\"text\":\"Overview text.\\n\\n### Detail\\nNested detail.\"}}\n",
          section->digest));

  // The heading match is case-insensitive.
  auto lowered = pk::expand_reference(conn, ready_task, "artifact:11#other");
  REQUIRE(lowered.has_value());
  CHECK(lowered->text == "Other text.");

  // No fragment: the whole body, and the digest equals the packet's reference digest.
  auto whole = pk::expand_reference(conn, ready_task, "artifact:10");
  REQUIRE(whole.has_value());
  CHECK(whole->text == "## Overview\nSpec section body for product_spec.\n");
  CHECK(whole->digest == d_product);
  CHECK(pk::render_expansion_text(*whole) == whole->text);
}

TEST_CASE("expand_reference refuses what the task does not cite", "[packet][references]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);
  exec(conn, "insert into artifacts (id, scope_kind, scope_id, kind, title, body, status) "
             "values (99, 'global', null, 'adr', 'Uncited', '## Secret\nNot linked.\n', 'active')");

  // An artifact that exists but is not among this task's `cites` links.
  auto uncited = pk::expand_reference(conn, ready_task, "artifact:99");
  REQUIRE_FALSE(uncited.has_value());
  CHECK(uncited.error() == pk::expand_error::unknown_reference);
  // The same artifact is cited by no link of the OTHER task either.
  auto other = pk::expand_reference(conn, bare_task, "artifact:10");
  REQUIRE_FALSE(other.has_value());
  CHECK(other.error() == pk::expand_error::unknown_reference);

  // Malformed references are unknown references too.
  for (std::string_view bad :
       {"", "artifact:", "artifact:abc", "artifact:10x", "artifact:0", "decision:20", "10", "artifact:10#"}) {
    INFO("ref: " << bad);
    auto got = pk::expand_reference(conn, ready_task, bad);
    REQUIRE_FALSE(got.has_value());
    CHECK(got.error() == pk::expand_error::unknown_reference);
  }

  auto no_section = pk::expand_reference(conn, ready_task, "artifact:10#Absent");
  REQUIRE_FALSE(no_section.has_value());
  CHECK(no_section.error() == pk::expand_error::section_not_found);

  auto no_task = pk::expand_reference(conn, 999999, "artifact:10");
  REQUIRE_FALSE(no_task.has_value());
  CHECK(no_task.error() == pk::expand_error::task_not_found);
}

TEST_CASE("a packet shaped like task 7252 renders under 16 KB as text and lists its citations as references",
          "[packet][references][size]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  seed(conn);

  // Task 7252's four cited artifacts weighed 7 KB, 12 KB, 29 KB and 42 KB, and
  // its sixteen facts restated them: 201,645 bytes as text. The cited bodies
  // here are the same size. None of their text may reach the packet.
  const auto big = [](std::string_view marker, std::size_t bytes) {
    std::string body = "## Overview\n";
    while (body.size() < bytes) {
      body += std::format("{} line of cited specification text.\n", marker);
    }
    return body;
  };
  exec(conn, std::format("update artifacts set body='{}' where id=10", big("PRODUCTBODY", 7233)));
  exec(conn, std::format("update artifacts set body='{}' where id=12", big("ROADMAPBODY", 12096)));
  exec(conn, std::format("update artifacts set body='{}' where id=11", big("TECHBODY", 28894)));
  exec(conn, std::format("update artifacts set body='{}' where id=13", big("TESTBODY", 41672)));

  auto packet = pk::assemble_task(conn, ready_task);
  REQUIRE(packet.has_value());
  CHECK(packet->ready());

  const auto text = pk::render_text(*packet);
  INFO("text bytes: " << text.size());
  CHECK(text.size() < 16 * 1024);
  const auto json = pk::render_json(*packet);
  INFO("json bytes: " << json.size());
  CHECK(json.size() < 16 * 1024);

  for (std::string_view marker : {"PRODUCTBODY", "ROADMAPBODY", "TECHBODY", "TESTBODY"}) {
    INFO("cited text leaked: " << marker);
    CHECK(text.find(marker) == std::string::npos);
    CHECK(json.find(marker) == std::string::npos);
  }
  for (std::string_view ref : {"artifact:10", "artifact:11", "artifact:12", "artifact:13"}) {
    INFO("reference: " << ref);
    CHECK(text.find(std::format("- {} ", ref)) != std::string::npos);
  }
  // A cited body is still reachable on demand.
  auto expanded = pk::expand_reference(conn, ready_task, "artifact:13");
  REQUIRE(expanded.has_value());
  CHECK(expanded->text.size() >= 41672);
}
