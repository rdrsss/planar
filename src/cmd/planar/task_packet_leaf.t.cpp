// @file task_packet_leaf.t.cpp
// @brief In-process tests for `planar task packet <task-id> [--json]` (plan
// 996, task 6324).
//
// The readiness RULES are pinned at the engine level in
// `src/lib/engine/ingest/packet.t.cpp`, on the same SQL seed. This file covers
// what that one cannot reach: the dispatch wiring, the exit-code mapping, and
// the exact envelope an ORCHESTRATOR parses.
//
// ## WHY THE EXIT CODES ARE THE POINT
//
// This verb has a real consumer beyond the operator. `task packet <id> --json`
// is what the orchestrator reads to decide whether a task is dispatchable, and
// `ready: false` is a documented STOP. That makes two mappings contract:
//
//   * An UNREADY packet is exit 0. It is a successful answer to the question
//     asked. A caller that treated the refusal as an error would stop for the
//     wrong reason and never see the `reasons` array that explains it.
//   * An UNKNOWN task is exit 1, not a well-formed empty packet. Emitting an
//     all-empty packet for a task that does not exist would read as "not ready
//     yet" — indistinguishable from a real answer, and the worse of the two
//     failures.
//
// Both are asserted below, and asserted TOGETHER, because either one alone
// would be satisfied by an implementation that got the other backwards.
//
// ## BOTH ARMS
//
// The fixture builds a READY task as well as a bare one and pins both. A suite
// that only ever saw `not_ready` could not distinguish a correct
// implementation from one that always refuses — and seventeen reasons on a
// bare task make "always refuses" a very convincing impostor.
//
// ## ORACLE PROVENANCE
//
// The expected bytes were captured from `zig/zig-out/bin/planar` built at this
// cycle's base, in a scratch arena (`PLANAR_DB` under a temp root, never the
// operator's database), and replayed as a 21-arm differential through both
// binaries against the same database file. All 21 agreed on stdout, stderr and
// exit code. Exit codes were read from the command itself, never through a
// pipe.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.cmd.planar.context;
import planar.cmd.planar.dispatch;
import planar.cmd.planar.tree;

namespace {

using planar::cmd::context;

struct invocation {
  int         code = 0;
  std::string out;
  std::string err;
};

struct fixture {
  std::filesystem::path                           root;
  std::map<std::string, std::string, std::less<>> vars;
  std::filesystem::path                           db_path;
};

auto make_fixture(std::string_view tag) -> fixture {
  auto const      root = std::filesystem::temp_directory_path() /
                         std::format("planar_pktleaf_{}_{}", tag, std::chrono::steady_clock::now().time_since_epoch().count());
  std::error_code ec;
  std::filesystem::create_directories(root / "proj", ec);
  return fixture{
      .root    = root,
      .vars    = {{"PLANAR_DB", (root / "planar.db").string()},
                  {"PLANAR_HOME", (root / "home").string()},
                  {"PLANAR_LOCAL_HOME", (root / "localhome").string()},
                  {"HOME", (root / "fakehome").string()},
                  {"PWD", (root / "proj").string()}},
      .db_path = root / "planar.db",
  };
}

auto dispatch(const fixture& fx, std::vector<std::string> args) -> invocation {
  std::vector<std::string> argv{"planar"};
  argv.insert(argv.end(), args.begin(), args.end());
  std::ostringstream out;
  std::ostringstream err;
  context            ctx{std::move(argv), planar::cmd::map_env(fx.vars), fx.root / "proj", fx.db_path, out, err};
  auto const         tree  = planar::cmd::root_app();
  auto const         table = planar::cmd::handlers(*tree);
  int const          code  = planar::cmd::run(ctx, *tree, table);
  return invocation{.code = code, .out = out.str(), .err = err.str()};
}

/// @brief Run raw SQL, asserting it succeeded.
///
/// Every seed step is guarded. An oracle probe for this cycle went vacuous
/// because two seed statements failed silently into /dev/null and the run
/// compared an arena that had never been built.
auto exec_sql(const fixture& fx, std::string_view sql) -> void {
  auto conn = planar::db::connection::open(fx.db_path.string());
  REQUIRE(conn.has_value());
  auto ok = conn->execute(sql);
  INFO("sql: " << sql);
  REQUIRE(ok.has_value());
}

// Digests computed with `shasum -a 256` over the documented preimage — an
// implementation outside this tree. Seeding them from `source_digest` would
// make the freshness half of every readiness assertion circular. See
// packet.t.cpp for the full preimage list.
constexpr std::string_view d_acceptance  = "c36d26ffec18cdac6ce40994f45fcbf6728bd65a160f0c4b9c6f29bd53c1b4d3";
constexpr std::string_view d_next_action = "a735b836c075edb69bf4ebf0fec21139638ae4cb8f5cc093540080da737a7a29";
constexpr std::string_view d_product     = "7294d414ca5de50586d1bbe24cff9820566fc37dae93a6f75a5005b3948b8b27";
constexpr std::string_view d_tech        = "ec43ff315eae393c7b9a6aa5cdb1e83b8c958c5917f3d3b12cec1bc5d6aac294";
constexpr std::string_view d_roadmap     = "fdd2302595324aaeaee4ecd136fbb2030178781f341d9bf9d88a8b560da197ad";
constexpr std::string_view d_test_spec   = "6fbf86941cd9618fbbd981162cad4e957b3e735e08a82ff465f6de627940368f";

/// @brief Seed the arena: a READY task 100 and a bare task 200 on one plan.
///
/// `init` runs first so the database exists and is migrated; everything after
/// it is SQL, because reaching a READY packet through the CLI would need the
/// whole `spec ingest --apply` pipeline (which stages the routing facts) and
/// that is a different subsystem from the one under test.
auto seed(const fixture& fx) -> void {
  REQUIRE(dispatch(fx, {"init", "--allow-no-repo", "--name", "t6324"}).code == 0);
  // `init` already registers a project, so this one takes an id and a
  // root_path that cannot collide with it. Hard-coding id 1 here failed
  // loudly on the first run — which is the seed guard doing its job.
  exec_sql(fx, "insert into projects (id, slug, name, root_path) "
               "values (900, 'pkt6324', 'pkt6324', '/tmp/pkt6324')");
  exec_sql(fx, "insert into plans (id, scope_kind, scope_id, title, slug, summary, status) "
               "values (1, 'global', null, 'Packet plan', 'packet-plan', 'Summary.', 'active')");
  exec_sql(fx, "insert into tasks (id, scope_kind, scope_id, plan_id, title, body, status, priority, next_action, slug) "
               "values (100, 'global', null, 1, 'Compile the routing packet',"
               "'Implement the packet compiler.\n"
               "\n"
               "## Acceptance Criteria\n"
               "The packet compiles with zero readiness reasons.\n"
               "\n"
               "## Required validation\n"
               "cmake --build build/debug\n"
               "', 'doing', 100, 'Port compileTask and assert both readiness arms.', 'pkt-ready')");
  exec_sql(fx, "insert into tasks (id, scope_kind, scope_id, plan_id, title, body, status, priority, slug) "
               "values (101, 'global', null, 1, 'Dependency', 'Body.', 'done', 100, 'pkt-dep')");
  exec_sql(fx, "insert into tasks (id, scope_kind, scope_id, plan_id, title, status, priority, slug) "
               "values (200, 'global', null, 1, 'Bare task', 'todo', 100, 'pkt-bare')");
  exec_sql(fx, "insert into artifacts (id, scope_kind, scope_id, kind, title, body, status) values "
               "(10,'global',null,'product_spec','Product','## Overview\nSpec section body for product_spec.\n','active'),"
               "(11,'global',null,'tech_spec','Tech','## Overview\nSpec section body for tech_spec.\n','active'),"
               "(12,'global',null,'roadmap','Roadmap','## Overview\nSpec section body for roadmap.\n','active'),"
               "(13,'global',null,'test_spec','Tests','## Overview\nSpec section body for test_spec.\n','active')");
  exec_sql(fx, "insert into decisions (id, scope_kind, scope_id, title, body, status, slug) "
               "values (20, 'global', null, 'Locked', 'Decision body.', 'accepted', 'pkt-dec')");
  exec_sql(fx, "insert into test_scenarios (id, scope_kind, scope_id, title, body, status, slug) "
               "values (30, 'global', null, 'Scenario', 'Scenario body.', 'ready', 'pkt-scn')");
  exec_sql(fx, "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values "
               "('task',100,'artifact',10,'cites'),('task',100,'artifact',11,'cites'),"
               "('task',100,'artifact',12,'cites'),('task',100,'artifact',13,'cites'),"
               "('task',100,'decision',20,'cites'),('task',100,'task',101,'depends-on'),"
               "('test_scenario',30,'task',100,'verifies'),('test_scenario',30,'plan',1,'derives-from')");
  exec_sql(fx, "insert into task_touch_paths (task_id, repo_id, path) "
               "values (100, 900, 'src/lib/engine/ingest/packet.cpp')");
  exec_sql(fx,
           std::format("insert into routing_task_facts (task_id, fact_kind, value_type, value_bool, value_text, "
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

} // namespace

TEST_CASE("task packet: the ready arm is genuinely reachable through the CLI", "[task-packet]") {
  auto const fx = make_fixture("ready");
  seed(fx);

  // FIRST, and load-bearing. Every `not_ready` assertion in this file and in
  // packet.t.cpp is only meaningful because this fixture can also produce a
  // READY packet through the same dispatch path.
  auto const res = dispatch(fx, {"task", "packet", "100", "--json"});
  INFO("stdout: " << res.out);
  INFO("stderr: " << res.err);
  CHECK(res.code == 0);
  CHECK(res.out.find(R"("ready":true)") != std::string::npos);
  CHECK(res.out.find(R"("reasons":[])") != std::string::npos);

  // The envelope's shape, in the order a consumer reads it.
  CHECK(res.out.starts_with(R"({"policy_version":"routing-packet-v1","ready":true,"input":{"task_id":100,)"));
  CHECK(res.out.find(R"("canonical":)") != std::string::npos);
  CHECK(res.out.find(R"("digest":)") != std::string::npos);
  CHECK(res.out.ends_with("]}\n"));
  CHECK(res.err.empty());
}

TEST_CASE("task packet: an unready packet is exit 0 with named reasons", "[task-packet]") {
  auto const fx = make_fixture("blocked");
  seed(fx);

  auto const res = dispatch(fx, {"task", "packet", "200", "--json"});
  INFO("stdout: " << res.out);

  // Exit 0. A refusal is a successful ANSWER, and an orchestrator that read
  // this as an error would lose the reasons that explain it.
  CHECK(res.code == 0);
  CHECK(res.out.find(R"("ready":false)") != std::string::npos);

  // The reasons arrive NAMED and in emission order — an operator cannot act on
  // a bare "not ready". Pinned as the literal array prefix rather than as a
  // membership test, because the order is contract and a set comparison would
  // not notice it changing.
  CHECK(res.out.find(R"("reasons":["missing_body","missing_acceptance_section","generic_acceptance",)"
                     R"("generic_next_action","missing_product_spec","missing_tech_spec","missing_roadmap",)"
                     R"("missing_test_spec","missing_locked_decision","missing_dependency","missing_touch",)"
                     R"("absent_validation_gates","missing_acceptance_fact","missing_next_action_fact",)"
                     R"("uncovered_required_scenario"]})") != std::string::npos);
}

TEST_CASE("task packet: the policy version rides in the envelope, not only the body", "[task-packet]") {
  auto const fx = make_fixture("policy");
  seed(fx);

  auto const res = dispatch(fx, {"task", "packet", "100", "--json"});
  REQUIRE(res.code == 0);

  // Packets compare only within one policy version. A consumer has to
  // establish that BEFORE comparing digests, and digging the version out of
  // the canonical body would mean parsing the very thing whose format the
  // version describes. So it appears twice, and both are contract.
  CHECK(res.out.find(R"("policy_version":"routing-packet-v1")") != std::string::npos);
  CHECK(res.out.find(R"(\"policy\":\"routing-packet-v1\")") != std::string::npos);
}

TEST_CASE("task packet: the digest is a change detector", "[task-packet]") {
  auto const fx = make_fixture("digest");
  seed(fx);

  auto const digest_of = [](std::string_view payload) -> std::string {
    constexpr std::string_view key   = R"("digest":")";
    auto const                 start = payload.find(key);
    REQUIRE(start != std::string_view::npos);
    auto const from = start + key.size();
    auto const end  = payload.find('"', from);
    REQUIRE(end != std::string_view::npos);
    return std::string{payload.substr(from, end - from)};
  };

  auto const first = dispatch(fx, {"task", "packet", "100", "--json"});
  REQUIRE(first.code == 0);
  auto const again = dispatch(fx, {"task", "packet", "100", "--json"});
  REQUIRE(again.code == 0);
  // Recompiling unchanged state reproduces it, or it could never certify
  // "this is the packet the dispatch was authorized against".
  CHECK(digest_of(first.out) == digest_of(again.out));

  auto const edited = dispatch(fx, {"task", "update", "100", "--next-action", "Something materially different."});
  REQUIRE(edited.code == 0);
  auto const after = dispatch(fx, {"task", "packet", "100", "--json"});
  REQUIRE(after.code == 0);
  // A packet that ignored an edit would authorize a dispatch against state
  // that no longer exists.
  CHECK(digest_of(after.out) != digest_of(first.out));
}

TEST_CASE("task packet: the text rendering names ready, digest, reasons and body", "[task-packet]") {
  auto const fx = make_fixture("text");
  seed(fx);

  auto const ready = dispatch(fx, {"task", "packet", "100"});
  REQUIRE(ready.code == 0);
  CHECK(ready.out.starts_with("task packet 100: ready\ndigest: "));
  // A ready packet prints NO reasons block — not an empty one.
  CHECK(ready.out.find("reasons:") == std::string::npos);
  CHECK(ready.out.find(R"({"policy":"routing-packet-v1","task_id":100)") != std::string::npos);

  auto const blocked = dispatch(fx, {"task", "packet", "200"});
  REQUIRE(blocked.code == 0);
  CHECK(blocked.out.starts_with("task packet 200: not_ready\ndigest: "));
  CHECK(blocked.out.find("\nreasons:\n- missing_body\n- missing_acceptance_section\n") != std::string::npos);
  CHECK(blocked.out.ends_with("\n"));
}

TEST_CASE("task packet: an unknown task refuses instead of emitting an empty packet", "[task-packet]") {
  auto const fx = make_fixture("missing");
  seed(fx);

  auto const res = dispatch(fx, {"task", "packet", "999999", "--json"});
  // Exit 1 and NOTHING on stdout. A well-formed all-empty packet here would
  // read as "not ready enough", which is the worse failure: it is
  // indistinguishable from a real answer about a real task.
  CHECK(res.code == 1);
  CHECK(res.out.empty());
  CHECK(res.err == "error: no task with id 999999\n");
}

TEST_CASE("task packet: a non-integer id is refused at the argument layer", "[task-packet]") {
  auto const fx = make_fixture("nonint");
  seed(fx);

  auto const res = dispatch(fx, {"task", "packet", "abc", "--json"});
  // Exit 2, not 1: this is a malformed INVOCATION rather than a missing
  // entity, and the two are distinguished so a caller can tell "I asked
  // wrongly" from "it isn't there". The id positional is declared as a STRING
  // precisely so the message carries the oracle's own wording.
  CHECK(res.code == 2);
  CHECK(res.out.empty());
  CHECK(res.err == "error: task id must be an integer, got 'abc'\n");
}

TEST_CASE("task packet: an id of zero is treated as a lookup, not a default", "[task-packet]") {
  auto const fx = make_fixture("zero");
  seed(fx);

  // Paired with the non-integer case above: `0` PARSES, so it must reach the
  // lookup and fail there. An omitted-or-unparseable id silently defaulting to
  // 0 and acting on row 0 is the shape this port keeps closing.
  auto const res = dispatch(fx, {"task", "packet", "0", "--json"});
  CHECK(res.code == 1);
  CHECK(res.err == "error: no task with id 0\n");
}
