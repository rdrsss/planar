// @file task_packet_leaf.t.cpp
// @brief In-process tests for `planar task packet <task-id> [--json]` .
//
// The readiness RULES are pinned at the engine level in
// `src/engine/ingest/packet.t.cpp`, on the same SQL seed. This file covers
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
// ## READINESS IS COORDINATION ONLY
//
// A task created with `task add` and nothing else is ready. The reasons are the
// seven in `packet.cppm`; this file drives them the way an operator reaches
// them, with `task link`, `question add` and `decision add`, and clears each
// with the verb that resolves its cause.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.cmd.planar.context;
import planar.cmd.planar.dispatch;
import planar.cmd.planar.main;

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
  context            ctx{std::move(argv),
                         planar::cmd::map_env(fx.vars),
                         fx.root / "proj",
                         std::make_shared<planar::cmd::database>(fx.db_path, err),
                         out,
                         err};
  auto const         tree  = planar::cmd::root_app();
  auto const         table = planar::cmd::make_handler_table(*tree);
  int const          code  = planar::cmd::run(ctx, *tree, table);
  return invocation{.code = code, .out = out.str(), .err = err.str()};
}

/// @brief Run raw SQL, asserting it succeeded.
///
/// Every seed step is guarded: a statement that failed silently would leave
/// the test comparing an arena that had never been built.
auto exec_sql(const fixture& fx, std::string_view sql) -> void {
  auto conn = planar::db::connection::open(fx.db_path.string());
  REQUIRE(conn.has_value());
  auto ok = conn->execute(sql);
  INFO("sql: " << sql);
  REQUIRE(ok.has_value());
}

/// @brief The `digest` value of a `--json` envelope, read from its key.
auto digest_of(std::string_view payload) -> std::string {
  // The envelope's digest follows `input`; a reference inside `input` carries a `digest` key of its own.
  constexpr std::string_view envelope_key = R"(},"digest":")";
  constexpr std::string_view plain_key    = R"("digest":")";
  auto const                 envelope     = payload.rfind(envelope_key);
  auto const                 start        = envelope != std::string_view::npos ? envelope + 2 : payload.find(plain_key);
  REQUIRE(start != std::string_view::npos);
  auto const from = start + plain_key.size();
  auto const end  = payload.find('"', from);
  REQUIRE(end != std::string_view::npos);
  return std::string{payload.substr(from, end - from)};
}

/// @brief Seed the arena: a READY task 100 and a bare task 200 on one plan.
///
/// `init` runs first so the database exists and is migrated; everything after
/// it is SQL, because reaching a READY packet through the CLI would need the
/// whole `spec ingest --apply` pipeline, a different subsystem from the one
/// under test. The task cites its artifacts through `cites` links, as ingest
/// leaves them, and no fact row exists.
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
  exec_sql(
      fx,
      "insert into artifacts (id, scope_kind, scope_id, kind, title, body, status) values "
      "(10,'global',null,'product_spec','Product','## Overview\nSpec section body for product_spec.\n','active'),"
      "(11,'global',null,'tech_spec','Tech','## Overview\nSpec section body for tech_spec.\n\n## Notes\nTech notes.\n','active'),"
      "(12,'global',null,'roadmap','Roadmap','## Overview\nSpec section body for roadmap.\n','active'),"
      "(13,'global',null,'test_spec','Tests','## Overview\nSpec section body for test_spec.\n','active')");
  exec_sql(fx, "insert into decisions (id, scope_kind, scope_id, title, body, status) "
               "values (20, 'global', null, 'Locked', 'Decision body.', 'accepted')");
  exec_sql(fx, "insert into test_scenarios (id, scope_kind, scope_id, title, body, status) "
               "values (30, 'global', null, 'Scenario', 'Scenario body.', 'ready')");
  exec_sql(fx, "insert into entity_links (from_kind, from_id, to_kind, to_id, relationship) values "
               "('task',100,'artifact',10,'cites'),('task',100,'artifact',11,'cites'),"
               "('task',100,'artifact',12,'cites'),('task',100,'artifact',13,'cites'),"
               "('task',100,'decision',20,'cites'),('task',100,'task',101,'depends-on'),"
               "('test_scenario',30,'task',100,'verifies'),('test_scenario',30,'plan',1,'derives-from')");
  exec_sql(fx, "insert into task_touch_paths (task_id, repo_id, path) "
               "values (100, 900, 'src/engine/ingest/packet.cpp')");
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
  CHECK(res.out.starts_with(R"({"policy_version":"routing-packet-v3","ready":true,"input":{"task_id":100,)"));
  // No `canonical` copy: `input` is the only serialized body of the evidence.
  CHECK(res.out.find(R"("canonical":)") == std::string::npos);
  // The digest sits where `canonical` used to: after `input`, before `reasons`.
  CHECK(res.out.find(R"(},"digest":")") != std::string::npos);
  CHECK(res.out.find(R"(},"digest":")") < res.out.find(R"("reasons":)"));
  CHECK(digest_of(res.out).size() == 64);
  CHECK(digest_of(res.out).find_first_not_of("0123456789abcdef") == std::string::npos);
  CHECK(res.out.ends_with("]}\n"));
  CHECK(res.err.empty());
}

TEST_CASE("task packet: a hand-made task with no artifacts and no scenarios is ready", "[task-packet]") {
  auto const fx = make_fixture("handmade");
  REQUIRE(dispatch(fx, {"init", "--skip-project", "--allow-no-repo", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"plan", "create", "Hand-made plan", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"task", "add", "Do the thing", "--plan", "1", "--json"}).code == 0);

  // No spec, no body, no decision, no scenario, no touch: nothing to prepare.
  auto const res = dispatch(fx, {"task", "packet", "1", "--json"});
  INFO("stdout: " << res.out);
  CHECK(res.code == 0);
  CHECK(res.out.contains(R"("ready":true)"));
  CHECK(res.out.ends_with(R"("reasons":[]})"
                          "\n"));

  auto const text = dispatch(fx, {"task", "packet", "1"});
  CHECK(text.code == 0);
  CHECK(text.out.starts_with("task packet 1: ready\ndigest: "));
  CHECK_FALSE(text.out.contains("reasons:"));
}

TEST_CASE("task packet: a refusal is exit 0 with named reasons in the documented order, each clearing when resolved",
          "[task-packet]") {
  auto const fx = make_fixture("blocked");
  REQUIRE(dispatch(fx, {"init", "--skip-project", "--allow-no-repo", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"plan", "create", "Hand-made plan", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"task", "add", "Blocked task", "--plan", "1", "--json"}).code == 0); // task 1
  REQUIRE(dispatch(fx, {"task", "add", "Prerequisite", "--plan", "1", "--json"}).code == 0); // task 2
  REQUIRE(dispatch(fx, {"question", "add", "Which database?", "--body", "Pick one.", "--json"}).code == 0);
  REQUIRE(dispatch(fx, {"decision", "add", "Pick SQLite", "--body", "SQLite.", "--json"}).code == 0);

  auto const reasons = [&]() {
    auto const res = dispatch(fx, {"task", "packet", "1", "--json"});
    INFO("stdout: " << res.out);
    // Exit 0. A refusal is a successful ANSWER, and an orchestrator that read
    // this as an error would lose the reasons that explain it.
    REQUIRE(res.code == 0);
    auto const at = res.out.rfind(R"("reasons":[)");
    REQUIRE(at != std::string::npos);
    return res.out.substr(at);
  };
  CHECK(reasons() == R"("reasons":[]})"
                     "\n");

  // Link the three causes in the reverse of the documented order, to show the
  // order comes from the packet and not from the order of the links.
  REQUIRE(dispatch(fx, {"task", "link", "1", "decision:1", "--relationship", "cites"}).code == 0);
  CHECK(reasons() == R"("reasons":["proposed_decision_linked"]})"
                     "\n");
  REQUIRE(dispatch(fx, {"task", "link", "1", "question:1", "--relationship", "addresses"}).code == 0);
  CHECK(reasons() == R"("reasons":["open_question_linked","proposed_decision_linked"]})"
                     "\n");
  REQUIRE(dispatch(fx, {"task", "link", "1", "task:2", "--relationship", "depends-on"}).code == 0);
  CHECK(reasons() == R"("reasons":["blocked_by_open_task","open_question_linked","proposed_decision_linked"]})"
                     "\n");

  // The text form names the same reasons in the same order.
  auto const text = dispatch(fx, {"task", "packet", "1"});
  CHECK(text.code == 0);
  CHECK(text.out.starts_with("task packet 1: not_ready\ndigest: "));
  CHECK(text.out.contains("\nreasons:\n- blocked_by_open_task\n- open_question_linked\n- proposed_decision_linked\n"));

  // Each clears when its cause is resolved, by the verb that resolves it.
  REQUIRE(dispatch(fx, {"task", "update", "2", "--status", "doing", "--json"}).code == 0);
  CHECK(reasons().starts_with(R"("reasons":["blocked_by_open_task",)"));
  REQUIRE(dispatch(fx, {"task", "done", "2"}).code == 0);
  CHECK(reasons() == R"("reasons":["open_question_linked","proposed_decision_linked"]})"
                     "\n");
  REQUIRE(dispatch(fx, {"question", "answer", "1", "--answer", "SQLite."}).code == 0);
  CHECK(reasons() == R"("reasons":["proposed_decision_linked"]})"
                     "\n");
  REQUIRE(dispatch(fx, {"decision", "accept", "1"}).code == 0);
  CHECK(reasons() == R"("reasons":[]})"
                     "\n");
}

TEST_CASE("task packet: a task that is already done is not claimable", "[task-packet]") {
  auto const fx = make_fixture("closed");
  seed(fx);

  // The seeded bare task is ready; closing it is the one change.
  CHECK(dispatch(fx, {"task", "packet", "200", "--json"}).out.contains(R"("ready":true)"));
  exec_sql(fx, "update tasks set status='done' where id=200");
  auto const res = dispatch(fx, {"task", "packet", "200", "--json"});
  CHECK(res.code == 0);
  CHECK(res.out.contains(R"("ready":false)"));
  CHECK(res.out.ends_with(R"("reasons":["invalid_task_status"]})"
                          "\n"));
}

TEST_CASE("task packet: the policy version rides in the envelope, not only the body", "[task-packet]") {
  auto const fx = make_fixture("policy");
  seed(fx);

  auto const res = dispatch(fx, {"task", "packet", "100", "--json"});
  REQUIRE(res.code == 0);

  // Packets compare only within one policy version. A consumer has to
  // establish that before comparing packets, so the version sits at the top
  // of the envelope. The envelope has no escaped `canonical` copy of it.
  CHECK(res.out.find(R"("policy_version":"routing-packet-v3")") != std::string::npos);
  CHECK(res.out.find(R"(\"policy\":\"routing-packet-v3\")") == std::string::npos);
}

TEST_CASE("task packet: the digest is a change detector", "[task-packet]") {
  auto const fx = make_fixture("digest");
  seed(fx);

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

TEST_CASE("task packet: a display-only plan rename leaves the digest unchanged", "[task-packet]") {
  auto const fx = make_fixture("rename");
  seed(fx);

  auto const before = dispatch(fx, {"task", "packet", "100", "--json"});
  REQUIRE(before.code == 0);
  // A plan's title reaches the packet only as `display_label`, which the
  // canonical body carries and the digest's preimage omits.
  exec_sql(fx, "update plans set title='Packet plan RENAMED' where id=1");
  auto const after = dispatch(fx, {"task", "packet", "100", "--json"});
  REQUIRE(after.code == 0);
  CHECK(after.out != before.out);
  CHECK(digest_of(after.out) == digest_of(before.out));
}

TEST_CASE("task packet: the JSON drops canonical and is byte-identical across two runs", "[task-packet]") {
  auto const fx = make_fixture("twice");
  seed(fx);

  // Each arm runs twice in the same fixture. A packet whose bytes drifted
  // between runs would make a consumer's comparison of two packets meaningless.
  for (auto const id : {"100", "200"}) {
    INFO("task " << id);
    auto const first = dispatch(fx, {"task", "packet", id, "--json"});
    REQUIRE(first.code == 0);
    auto const second = dispatch(fx, {"task", "packet", id, "--json"});
    REQUIRE(second.code == 0);
    CHECK(second.out == first.out);
    CHECK(first.out.find("\"canonical\":") == std::string::npos);
    CHECK(first.out.find("\"input\":{") != std::string::npos);
    CHECK(digest_of(first.out) == digest_of(second.out));
  }
}

TEST_CASE("task packet: the text rendering names ready, digest, reasons and body", "[task-packet]") {
  auto const fx = make_fixture("text");
  seed(fx);

  auto const ready = dispatch(fx, {"task", "packet", "100"});
  REQUIRE(ready.code == 0);
  CHECK(ready.out.starts_with("task packet 100: ready\ndigest: "));
  // A ready packet prints NO reasons block — not an empty one.
  CHECK(ready.out.find("reasons:") == std::string::npos);
  CHECK(ready.out.find(R"({"policy":"routing-packet-v3","task_id":100)") != std::string::npos);
  // The citations arrive as references, taken from the task's `cites` links,
  // and carry none of the cited text.
  CHECK(ready.out.contains("references (expand one with: planar task packet 100 --expand <ref>):\n"));
  CHECK(ready.out.contains("- artifact:10 4064d6efbf4b Product\n"));
  CHECK(ready.out.contains("- artifact:11 "));
  CHECK(ready.out.contains("- artifact:12 "));
  CHECK(ready.out.contains("- artifact:13 "));
  CHECK_FALSE(ready.out.contains("Spec section body for"));
  CHECK_FALSE(ready.out.contains("\"facts\""));

  exec_sql(fx, "update tasks set status='done' where id=200");
  auto const blocked = dispatch(fx, {"task", "packet", "200"});
  REQUIRE(blocked.code == 0);
  CHECK(blocked.out.starts_with("task packet 200: not_ready\ndigest: "));
  CHECK(blocked.out.find("\nreasons:\n- invalid_task_status\n") != std::string::npos);
  CHECK(blocked.out.ends_with("\n"));
}

TEST_CASE("task packet: an unknown task refuses instead of emitting an empty packet", "[task-packet]") {
  auto const fx = make_fixture("missing");
  seed(fx);

  auto const res = dispatch(fx, {"task", "packet", "999999", "--json"});
  // Exit 1. The pinned stderr line stays byte-identical (task 6844,
  // decision 1145, supersedes D5); the --json envelope lands on stdout
  // instead, the same stream a successful `--json` packet would use.
  CHECK(res.code == 1);
  CHECK(res.err == "error: no task with id 999999\n");
  CHECK(res.out == R"({"error":{"verb":"task packet","tag":"not_found"}})"
                   "\n");
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
  CHECK(res.err == "error: task id must be an integer, got 'abc'\n");
  CHECK(res.out == R"({"error":{"verb":"task packet","tag":"invalid_input"}})"
                   "\n");
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
  CHECK(res.out == R"({"error":{"verb":"task packet","tag":"not_found"}})"
                   "\n");
}

// ===========================================================================
// References and `--expand` (plan 1142).
// ===========================================================================

TEST_CASE("task packet: --json lists references and no facts or cited text", "[task-packet][references]") {
  auto const fx = make_fixture("refs_json");
  seed(fx);

  auto const res = dispatch(fx, {"task", "packet", "100", "--json"});
  REQUIRE(res.code == 0);
  CHECK(res.out.contains(R"("references":[{"ref":"artifact:10","kind":"product_spec","id":10,"locator":"body","digest":")"));
  CHECK(res.out.contains(R"("label":"Product","status":"active"})"));
  CHECK_FALSE(res.out.contains(R"("facts")"));
  CHECK_FALSE(res.out.contains(R"("citations")"));
  CHECK_FALSE(res.out.contains("Spec section body for"));
}

TEST_CASE("task packet: --expand prints exactly the cited section", "[task-packet][references]") {
  auto const fx = make_fixture("expand");
  seed(fx);

  // A section by heading: the text under it and nothing else, exit 0, stderr silent.
  auto const section = dispatch(fx, {"task", "packet", "100", "--expand", "artifact:11#Notes"});
  INFO("stdout: " << section.out << "\nstderr: " << section.err);
  CHECK(section.code == 0);
  CHECK(section.out == "Tech notes.\n");
  CHECK(section.err.empty());

  // The heading is matched case-insensitively and the sibling section is not included.
  auto const overview = dispatch(fx, {"task", "packet", "100", "--expand", "artifact:11#overview"});
  CHECK(overview.code == 0);
  CHECK(overview.out == "Spec section body for tech_spec.\n");

  // No fragment: the whole artifact body.
  auto const whole = dispatch(fx, {"task", "packet", "100", "--expand", "artifact:10"});
  CHECK(whole.code == 0);
  CHECK(whole.out == "## Overview\nSpec section body for product_spec.\n");

  // --json: a stable object with the ref, the digest and the text.
  auto const json = dispatch(fx, {"task", "packet", "100", "--expand", "artifact:11#Notes", "--json"});
  CHECK(json.code == 0);
  CHECK(json.out.starts_with(R"({"ref":"artifact:11#Notes","digest":")"));
  CHECK(json.out.ends_with(R"(","text":"Tech notes."})"
                           "\n"));
  CHECK(digest_of(json.out).size() == 64);
}

TEST_CASE("task packet: --expand refuses a reference the task does not cite and names it", "[task-packet][references]") {
  auto const fx = make_fixture("expand_unknown");
  seed(fx);
  exec_sql(fx, "insert into artifacts (id, scope_kind, scope_id, kind, title, body, status) "
               "values (99,'global',null,'adr','Uncited','## Secret\nNot linked.\n','active')");

  // An artifact that exists but is not cited, and one that does not exist.
  for (std::string_view ref : {"artifact:99", "artifact:98"}) {
    INFO("ref: " << ref);
    auto const res = dispatch(fx, {"task", "packet", "100", "--expand", std::string{ref}});
    CHECK(res.code == 1);
    CHECK(res.out.empty());
    CHECK(res.err == std::format("error: task packet: unknown reference {} for task 100\n", ref));
  }

  // Task 200 cites nothing, so even a real artifact is unknown to it.
  auto const other = dispatch(fx, {"task", "packet", "200", "--expand", "artifact:10"});
  CHECK(other.code == 1);
  CHECK(other.err == "error: task packet: unknown reference artifact:10 for task 200\n");

  auto const bad_section = dispatch(fx, {"task", "packet", "100", "--expand", "artifact:10#Missing"});
  CHECK(bad_section.code == 1);
  CHECK(bad_section.out.empty());
  CHECK(bad_section.err.contains("artifact:10#Missing"));

  auto const no_task = dispatch(fx, {"task", "packet", "999999", "--expand", "artifact:10"});
  CHECK(no_task.code == 1);
  CHECK(no_task.err == "error: no task with id 999999\n");
}

TEST_CASE("task packet: a packet shaped like task 7252 is under 16 KB as text", "[task-packet][references][size]") {
  auto const fx = make_fixture("size");
  seed(fx);

  // Four cited artifacts as large as task 7252's: 7 KB, 12 KB, 29 KB, 42 KB.
  // Sixteen facts restated them and the text packet was 201,645 bytes.
  auto const big = [](std::string_view marker, std::size_t bytes) {
    std::string body = "## Overview\n";
    while (body.size() < bytes) {
      body += std::format("{} line of cited specification text.\n", marker);
    }
    return body;
  };
  exec_sql(fx, std::format("update artifacts set body='{}' where id=10", big("PRODUCTBODY", 7233)));
  exec_sql(fx, std::format("update artifacts set body='{}' where id=12", big("ROADMAPBODY", 12096)));
  exec_sql(fx, std::format("update artifacts set body='{}' where id=11", big("TECHBODY", 28894)));
  exec_sql(fx, std::format("update artifacts set body='{}' where id=13", big("TESTBODY", 41672)));

  auto const text = dispatch(fx, {"task", "packet", "100"});
  REQUIRE(text.code == 0);
  INFO("text bytes: " << text.out.size());
  CHECK(text.out.size() < 16 * 1024);
  CHECK_FALSE(text.out.contains("BODY line of cited"));
  for (std::string_view ref : {"artifact:10", "artifact:11", "artifact:12", "artifact:13"}) {
    CHECK(text.out.contains(std::format("- {} ", ref)));
  }

  // The cited text is still one verb away.
  auto const expanded = dispatch(fx, {"task", "packet", "100", "--expand", "artifact:13"});
  CHECK(expanded.code == 0);
  CHECK(expanded.out.size() >= 41672);
}

TEST_CASE("task facts stage is an unknown subcommand", "[task-packet][references]") {
  auto const fx = make_fixture("facts_gone");
  seed(fx);

  // The verb was removed with the facts it staged; it is refused at the
  // argument layer and writes nothing.
  auto const res = dispatch(fx, {"task", "facts", "stage", "100"});
  CHECK(res.code == 2);
  CHECK(res.out.empty());
  CHECK(res.err.contains("facts"));

  auto conn = planar::db::connection::open(fx.db_path.string());
  REQUIRE(conn.has_value());
  auto stmt = conn->prepare("select count(*) from routing_task_facts");
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->step().has_value());
  CHECK(stmt->column_int64(0) == 0);
}
