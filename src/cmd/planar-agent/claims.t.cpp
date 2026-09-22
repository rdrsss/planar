// @file claims.t.cpp
// @brief Unit tests for the `planar-agent` claim verbs' layer-3 half
// (plan 996, task 6038): the two value parsers every verb runs on operator
// input, the injected task policy, and one whole verb driven through
// `dispatch::run` against a real scratch database.
//
// Oracle captures behind the sharper expectations, all taken by RUNNING
// `zig/zig-out/bin/planar-agent` against a scratch database:
//
//   $Z claim --entity task:abc  -> exit 2, stderr
//        `error: invalid --entity 'task:abc' (InvalidEntityRef); expected
//         task:<id>|plan:<id>|plan_step:<id>`
//   $Z claim --entity bogus:1   -> exit 1, same sentence with
//        `(UnsupportedEntityKind)`
//   $Z claim --entity task:1 --ttl zzz
//                               -> exit 1, stderr
//        `error: invalid --ttl 'zzz': expected bare seconds (e.g. 600) or
//         suffixed duration (e.g. 10m, 1h, 500ms)`
//   $Z pull 1 --metadata '{'    -> exit 2
//   $Z pull 1 --stage code      -> exit 2
//   $Z pull 1 --parent-action 0 -> exit 2
//   $Z pull 1 --parent-action 999 -> exit 1   (NotFound is unmapped)
//
// The last four are the reason `invalid_value_error` and
// `invalid_input_error` are separate helpers: the messages read alike and
// the exit codes differ.
//
// ## Break-probes run against this file
//
//   - Mapped `unsupported_entity_kind` to `invalid_entity_ref`'s exit
//     bucket -> `the two --entity refusals exit differently` FAILS.
//   - Made `parse_ttl_seconds` reject a bare integer's trailing space ->
//     `parse_ttl_seconds trims the unit` FAILS.
//   - Dropped the underscore skip in `parse_int64_zig` -> `entity ids
//     parse exactly as Zig's parseInt does` FAILS.
//   - Made `task_policy().check_transition` always succeed -> `the injected
//     policy is the real task matrix` FAILS.

#include <catch2/catch_test_macros.hpp>

import std;
import cli11;
import planar.cliapp.args;
import planar.db;
import planar.db.migrate;
import planar.engine.runtime.agentactivity;
import planar.engine.routing;
import planar.cmd.planar_agent.args;
import planar.cmd.planar_agent.context;
import planar.cmd.planar_agent.dispatch;
import planar.cmd.planar_agent.policy;
import planar.cmd.planar_agent.tree;

namespace {

namespace agent   = planar::cmd::agent;
namespace aa      = planar::engine::runtime::agentactivity;
namespace routing = planar::engine::routing;

struct scratch_dir {
  std::filesystem::path path_;

  scratch_dir()
      : path_(std::filesystem::temp_directory_path() / std::format("planar_agent_claims_test_{}_{}",
                                                                   std::chrono::steady_clock::now().time_since_epoch().count(),
                                                                   reinterpret_cast<std::uintptr_t>(this))) {
    std::filesystem::create_directories(path_);
  }

  scratch_dir(const scratch_dir&)            = delete;
  scratch_dir& operator=(const scratch_dir&) = delete;

  ~scratch_dir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  [[nodiscard]] auto db_path() const -> std::filesystem::path {
    return path_ / "planar.db";
  }
};

/// One dispatch invocation's observable result.
struct invocation {
  int         code = 0;
  std::string out;
  std::string err;
};

/// Run `argv` through the real tree and the real handler table, against a
/// scratch database. This is the whole binary minus `main`'s process
/// plumbing — the same path an operator's invocation takes.
auto run_verb(const scratch_dir& scratch, std::vector<std::string> argv) -> invocation {
  auto const root  = agent::root_app();
  auto const table = agent::handlers(*root);
  argv.insert(argv.begin(), "planar-agent");

  std::ostringstream out;
  std::ostringstream err;
  agent::context     ctx{argv, agent::map_env({}), scratch.path_, scratch.db_path(), out, err};
  auto const         code = agent::run(ctx, *root, table);
  return invocation{.code = code, .out = out.str(), .err = err.str()};
}

auto seed(const scratch_dir& scratch, int task_count) -> void {
  auto conn = planar::db::connection::open(scratch.db_path().string());
  REQUIRE(conn.has_value());
  REQUIRE(planar::db::apply_all(*conn).has_value());
  REQUIRE(conn->execute("insert into plans (scope_kind, title, slug, status) values ('global','p','test-plan','active')")
              .has_value());
  for (int i = 0; i < task_count; ++i) {
    REQUIRE(conn->execute(std::format("insert into tasks (scope_kind, plan_id, title, status, priority) "
                                      "values ('global', 1, 't{}', 'todo', {})",
                                      i, 100 + i))
                .has_value());
  }
}

auto scalar_text(const scratch_dir& scratch, std::string_view sql) -> std::string {
  auto conn = planar::db::connection::open(scratch.db_path().string());
  REQUIRE(conn.has_value());
  auto stmt = conn->prepare(sql);
  REQUIRE(stmt.has_value());
  auto step = stmt->step();
  REQUIRE(step.has_value());
  REQUIRE(*step == planar::db::step_result::row);
  return stmt->column_text(0);
}

} // namespace

// ===========================================================================
// parse_entity_ref
// ===========================================================================

TEST_CASE("parse_entity_ref accepts exactly the three claimable kinds", "[cmd][agent][args]") {
  auto const task = agent::parse_entity_ref("task:42");
  REQUIRE(task.has_value());
  CHECK(task->kind == aa::entity_kind::task);
  CHECK(task->id == 42);

  CHECK(agent::parse_entity_ref("plan:1")->kind == aa::entity_kind::plan);
  CHECK(agent::parse_entity_ref("plan_step:7")->kind == aa::entity_kind::plan_step);

  // The wider ACTION entity set is NOT accepted here — a claim can only be
  // held on the three. `action start --entity` has its own parser for the
  // wider set; routing both through one would either narrow that verb or
  // widen this one.
  CHECK(agent::parse_entity_ref("decision:1").error() == agent::entity_ref_error::unsupported_entity_kind);
  CHECK(agent::parse_entity_ref("question:1").error() == agent::entity_ref_error::unsupported_entity_kind);
}

TEST_CASE("the two --entity refusals exit differently", "[cmd][agent][args][exitcode]") {
  // Oracle-verified: `task:abc` exits 2 and `bogus:1` exits 1, from the
  // same flag. `planar-agent`'s codeFor maps `InvalidEntityRef` to 2 and
  // has NO arm for `UnsupportedEntityKind`, which falls through to the
  // generic 1.
  CHECK(agent::parse_entity_ref("task:abc").error() == agent::entity_ref_error::invalid_entity_ref);
  CHECK(agent::parse_entity_ref("bogus:1").error() == agent::entity_ref_error::unsupported_entity_kind);
  CHECK(agent::parse_entity_ref("nocolon").error() == agent::entity_ref_error::invalid_entity_ref);
  CHECK(agent::parse_entity_ref("task:").error() == agent::entity_ref_error::invalid_entity_ref);

  // The ORDER matters and is easy to get backwards: the id is parsed
  // BEFORE the kind is recognised, so a doubly-bad ref reports the id
  // failure and exits 2, not the kind failure and 1.
  CHECK(agent::parse_entity_ref("bogus:abc").error() == agent::entity_ref_error::invalid_entity_ref);

  // The tag is interpolated into the operator-visible message.
  CHECK(agent::entity_ref_error_name(agent::entity_ref_error::invalid_entity_ref) == "InvalidEntityRef");
  CHECK(agent::entity_ref_error_name(agent::entity_ref_error::unsupported_entity_kind) == "UnsupportedEntityKind");
}

TEST_CASE("entity ids parse exactly as Zig's parseInt does", "[cmd][agent][args]") {
  // Underscore digit separators and a leading `+` are accepted; a leading
  // or trailing separator is not. Inherited from the operator binary's
  // `parse_int64_zig` and re-checked HERE because this is where an
  // `--entity` value reaches it.
  CHECK(agent::parse_entity_ref("task:1_0")->id == 10);
  CHECK(agent::parse_entity_ref("task:+7")->id == 7);
  CHECK(agent::parse_entity_ref("task:007")->id == 7);
  CHECK(agent::parse_entity_ref("task:_10").error() == agent::entity_ref_error::invalid_entity_ref);
  CHECK(agent::parse_entity_ref("task:10_").error() == agent::entity_ref_error::invalid_entity_ref);
  CHECK(agent::parse_entity_ref("task:0x10").error() == agent::entity_ref_error::invalid_entity_ref);
  CHECK(agent::parse_entity_ref("task: 1").error() == agent::entity_ref_error::invalid_entity_ref);
}

// ===========================================================================
// parse_ttl_seconds
// ===========================================================================

TEST_CASE("parse_ttl_seconds reads a bare integer as SECONDS", "[cmd][agent][args]") {
  // The ergonomic default the flag's own help promises. A port that read
  // it as nanoseconds would give every claim a zero-second lease.
  CHECK(agent::parse_ttl_seconds("600") == 600);
  CHECK(agent::parse_ttl_seconds("1") == 1);
}

TEST_CASE("parse_ttl_seconds refuses a literal zero duration (task 6906)", "[cmd][agent][args]") {
  // `--ttl 0` (in any unit) used to be ACCEPTED and yielded an
  // already-lapsed lease — a claim minted expired on arrival. A negative
  // duration was already unreachable (the parser never accepts a leading
  // `-`, so it falls out through the malformed-input path below), but a
  // literal zero magnitude was not. This is the same "invalid" answer as
  // garbage input, not a value to accept and immediately expire.
  //
  // This is distinct from the SUB-SECOND rounding case (`500ms` -> 0,
  // pinned separately below and still legal under D2): here the parsed
  // MAGNITUDE itself is zero, not merely its truncation to whole seconds.
  CHECK_FALSE(agent::parse_ttl_seconds("0").has_value());
  CHECK_FALSE(agent::parse_ttl_seconds("0s").has_value());
  CHECK_FALSE(agent::parse_ttl_seconds("0m").has_value());
  CHECK_FALSE(agent::parse_ttl_seconds("0h").has_value());
  CHECK_FALSE(agent::parse_ttl_seconds("0ns").has_value());
}

TEST_CASE("parse_ttl_seconds handles every declared unit", "[cmd][agent][args]") {
  CHECK(agent::parse_ttl_seconds("1s") == 1);
  CHECK(agent::parse_ttl_seconds("10m") == 600);
  CHECK(agent::parse_ttl_seconds("1h") == 3600);
  CHECK(agent::parse_ttl_seconds("8h") == 28800);
}

TEST_CASE("parse_ttl_seconds rounds SUB-SECOND values down to zero", "[cmd][agent][args]") {
  // `500ms` is a LEGAL value that yields a ZERO-second lease — a claim
  // already expired the instant it is minted. The flag's help text
  // advertises `500ms` as an example, so this is reachable by following
  // the documentation. Reproduced under D2 rather than corrected, and
  // pinned here so a future fix is deliberate.
  CHECK(agent::parse_ttl_seconds("500ms") == 0);
  CHECK(agent::parse_ttl_seconds("100us") == 0);
  CHECK(agent::parse_ttl_seconds("5ns") == 0);
  CHECK(agent::parse_ttl_seconds("1500ms") == 1);
}

TEST_CASE("parse_ttl_seconds trims the unit", "[cmd][agent][args]") {
  // Zig's `splitNumUnit` runs `trim(text[i..], " \t")` on the unit
  // portion, so trailing whitespace parses and so does a space before the
  // unit. Easy to drop in a port; visible if someone quotes a flag value.
  CHECK(agent::parse_ttl_seconds("600 ") == 600);
  CHECK(agent::parse_ttl_seconds("10 m") == 600);
  CHECK(agent::parse_ttl_seconds("1\th") == 3600);
}

TEST_CASE("parse_ttl_seconds refuses malformed input", "[cmd][agent][args]") {
  CHECK_FALSE(agent::parse_ttl_seconds("").has_value());
  CHECK_FALSE(agent::parse_ttl_seconds("zzz").has_value());
  CHECK_FALSE(agent::parse_ttl_seconds("m").has_value());
  CHECK_FALSE(agent::parse_ttl_seconds("10x").has_value());
  CHECK_FALSE(agent::parse_ttl_seconds("-5").has_value());
  // Overflowing the unsigned nanosecond range is malformed input, not a
  // value to clamp.
  CHECK_FALSE(agent::parse_ttl_seconds("99999999999h").has_value());
}

// ===========================================================================
// The injected task policy
// ===========================================================================

TEST_CASE("the injected policy is the real task matrix", "[cmd][agent][policy]") {
  // The seam `agentatomic` takes as a callable. If this bound to something
  // permissive, every status guard in the agent plane would be a no-op and
  // nothing in the type system would say so.
  auto const policy = agent::task_policy();
  REQUIRE(policy.check_transition);
  REQUIRE(policy.recompute_plan);

  CHECK(policy.check_transition("todo", "doing").has_value());
  CHECK(policy.check_transition("doing", "done").has_value());
  CHECK(policy.check_transition("doing", "todo").has_value());
  CHECK(policy.check_transition("blocked", "done").has_value());

  // The refusals the agent plane actually depends on.
  CHECK(policy.check_transition("todo", "done").error() == aa::agent_error::illegal_transition);
  CHECK(policy.check_transition("done", "doing").error() == aa::agent_error::illegal_transition);
  CHECK(policy.check_transition("cancelled", "todo").error() == aa::agent_error::illegal_transition);

  // Identity is a no-op success — which is why `release` on a task already
  // `todo` succeeds while `complete` on the same task does not.
  CHECK(policy.check_transition("todo", "todo").has_value());

  // A status outside the matrix is a DIFFERENT error from an illegal move.
  CHECK(policy.check_transition("nonsense", "done").error() == aa::agent_error::unknown_status);
}

// ===========================================================================
// A whole verb, end to end through dispatch
// ===========================================================================

TEST_CASE("pull runs end to end and writes the claim ritual's three rows", "[cmd][agent][handlers]") {
  scratch_dir scratch;
  seed(scratch, 2);

  auto const pulled = run_verb(scratch, {"pull", "1", "--no-locality-probe"});
  CHECK(pulled.code == 0);
  CHECK(pulled.err.empty());
  CHECK(pulled.out.starts_with("pulled task:1 claim:"));
  CHECK(pulled.out.ends_with(" action:1\n"));

  // A session was minted (the FK every claim needs), the task moved, and
  // the dispatch action is OPEN.
  CHECK(scalar_text(scratch, "select status from tasks where id = 1") == "doing");
  CHECK(scalar_text(scratch, "select count(*) from agent_work_claims where status = 'active'") == "1");
  CHECK(scalar_text(scratch, "select count(*) from sessions") == "1");
  CHECK(scalar_text(scratch, "select count(*) from agent_actions where ended_at is null") == "1");
  // `--vendor` defaulted, and the default is the binary's own name.
  CHECK(scalar_text(scratch, "select vendor from agent_work_claims where id = 1") == "planar-agent");
}

TEST_CASE("complete runs end to end and closes the ritual", "[cmd][agent][handlers]") {
  scratch_dir scratch;
  seed(scratch, 1);
  auto const pulled = run_verb(scratch, {"pull", "1", "--no-locality-probe", "--json"});
  REQUIRE(pulled.code == 0);

  auto const token = scalar_text(scratch, "select claim_token from agent_work_claims where id = 1");
  auto const done  = run_verb(scratch, {"complete", "--claim", token, "--summary", "did it"});
  CHECK(done.code == 0);
  CHECK(done.out == "ok task:1 status:done claim_status:completed\n");
  CHECK(scalar_text(scratch, "select status from tasks where id = 1") == "done");
  CHECK(scalar_text(scratch, "select status from agent_work_claims where id = 1") == "completed");
  CHECK(scalar_text(scratch, "select count(*) from agent_actions where ended_at is null") == "0");
  // The plan roll-up ran, and its answer for an ANCHOR plan is `active`,
  // not `done`: plan-304's aggregate matrix never auto-promotes a plan
  // with no parent to a terminal status, however complete its tasks are.
  // Written as `done` first and corrected by the failure — the seeded plan
  // has `parent_plan_id IS NULL`. `pull runs a CHILD plan's roll-up to
  // done` below is the case that exercises the flip.
  CHECK(scalar_text(scratch, "select status from plans where id = 1") == "active");
}

TEST_CASE("the handler-level refusals carry the oracle's exact messages and codes", "[cmd][agent][handlers][exitcode]") {
  scratch_dir scratch;
  seed(scratch, 2);

  struct expectation {
    std::vector<std::string> argv;
    int                      code;
    std::string              err;
  };

  // Every string and every code below was captured from the reference
  // binary; see this file's header.
  std::vector<expectation> const cases{
      {{"claim", "--entity", "task:abc"},
       2,
       "error: invalid --entity 'task:abc' (InvalidEntityRef); expected task:<id>|plan:<id>|plan_step:<id>\n"},
      {{"claim", "--entity", "bogus:1"},
       1,
       "error: invalid --entity 'bogus:1' (UnsupportedEntityKind); expected task:<id>|plan:<id>|plan_step:<id>\n"},
      {{"claim", "--entity", "task:1", "--ttl", "zzz"},
       1,
       "error: invalid --ttl 'zzz': expected bare seconds (e.g. 600) or suffixed duration (e.g. 10m, 1h, 500ms)\n"},
      {{"reconcile", "--stale-after", "zzz"},
       1,
       "error: invalid --stale-after 'zzz': expected bare seconds (e.g. 0) or suffixed duration (e.g. 10m, 1h, 500ms)\n"},
      {{"pull", "1", "--stage", "code"}, 2, "error: --stage requires --run: provide a workflow_runs.id via --run <id>\n"},
      {{"pull", "1", "--metadata", "{"}, 2, "error: --metadata is not valid JSON: {\n"},
      {{"pull", "1", "--parent-action", "0"}, 2, "error: --parent-action must be a positive integer (got 0)\n"},
      {{"pull", "1", "--parent-action", "999"}, 1, "error: --parent-action 999: action not found\n"},
      {{"complete", "--claim", "deadbeef"}, 1, "error: complete: ClaimNotFound\n"},
      {{"heartbeat", "--claim", "deadbeef"}, 1, "error: heartbeat: ClaimNotFound\n"},
      {{"abort", "--claim", "deadbeef"}, 1, "error: abort: ClaimNotFound\n"},
      {{"claim", "--entity", "task:99"}, 1, "error: claim: TaskNotFound\n"},
      {{"action", "start", "--claim", "deadbeef", "--kind", "coder"}, 1, "error: claim lookup: ClaimNotFound\n"},
  };

  for (auto const& [argv, code, err] : cases) {
    auto args = argv;
    args.push_back("--no-locality-probe");
    // `--no-locality-probe` is not declared on every verb here, so only
    // append it where it is legal; the parser would refuse it otherwise.
    auto const legal  = argv.front() == "pull" || argv.front() == "claim" || argv.front() == "complete";
    auto const result = run_verb(scratch, legal ? args : argv);
    INFO("argv[0]: " << argv.front() << " argv[1]: " << (argv.size() > 1 ? argv[1] : ""));
    CHECK(result.code == code);
    CHECK(result.err == err);
    CHECK(result.out.empty());
  }
}

TEST_CASE("json flag adds a one-line error envelope on stdout; without it there is none", "[cmd][agent][handlers][6844]") {
  scratch_dir scratch;
  seed(scratch, 1);

  // Task 6844 (decision 1145, supersedes D5): additive on STDOUT -- the
  // same stream a successful `--json` result would use -- so the pinned
  // stderr text stays byte-identical. The envelope's `tag` is the SAME
  // CamelCase Zig-style tag the pinned line already carries
  // (`derive_tag`'s primary path), not this binary's own
  // `domain_error_kind` (which would collapse `ClaimNotFound` into the
  // useless `generic_failure` bucket).
  auto const with_json = run_verb(scratch, {"complete", "--claim", "deadbeef", "--json"});
  CHECK(with_json.code == 1);
  CHECK(with_json.err == "error: complete: ClaimNotFound\n");
  CHECK(with_json.out == R"({"error":{"verb":"complete","tag":"ClaimNotFound"}})"
                         "\n");

  // Negative control: the exact same failure, no --json, no envelope --
  // the pinned line is untouched (this row is already covered in the
  // table-driven case above; repeated here so the pair sits together).
  auto const without_json = run_verb(scratch, {"complete", "--claim", "deadbeef"});
  CHECK(without_json.code == 1);
  CHECK(without_json.out.empty());
  CHECK(without_json.err == "error: complete: ClaimNotFound\n");
}

// NOTE ON THE NAME: it deliberately does not begin with `--`.
// `catch_discover_tests` registers each Catch2 case with CTest by passing
// its NAME as an argument, and a name starting with `--` is parsed as a
// command-line flag ("Unrecognised token: --status") — the case then FAILS
// under `ctest` while passing when the binary is filtered by hand. Caught
// by the full gate run; the same rename applies to
// engine/runtime/agentactivity.t.cpp's worktree case.
TEST_CASE("an oversized heartbeat status is refused and the heartbeat rolls back", "[cmd][agent][handlers]") {
  scratch_dir scratch;
  seed(scratch, 1);
  REQUIRE(run_verb(scratch, {"pull", "1", "--no-locality-probe"}).code == 0);
  auto const token  = scalar_text(scratch, "select claim_token from agent_work_claims where id = 1");
  auto const before = scalar_text(scratch, "select lease_expires_at from agent_work_claims where id = 1");

  std::string const oversized(257, 'x');
  auto const        refused = run_verb(scratch, {"heartbeat", "--claim", token, "--status", oversized});
  CHECK(refused.code == 2);
  CHECK(refused.err == "error: --status payload is 257 bytes; the cap is 256. Shorten the status string.\n");
  // The whole heartbeat rolled back — the lease was NOT refreshed. A
  // handler that opened the transaction, refreshed, and only then checked
  // the cap would leave the lease moved.
  CHECK(scalar_text(scratch, "select lease_expires_at from agent_work_claims where id = 1") == before);
  CHECK(scalar_text(scratch, "select count(*) from agent_actions where action_kind = 'heartbeat'") == "0");

  // Exactly at the cap is accepted, and writes the action row.
  std::string const at_cap(256, 'x');
  CHECK(run_verb(scratch, {"heartbeat", "--claim", token, "--status", at_cap}).code == 0);
  CHECK(scalar_text(scratch, "select count(*) from agent_actions where action_kind = 'heartbeat'") == "1");
}

TEST_CASE("an omitted --status writes no action row; an EMPTY one does", "[cmd][agent][handlers]") {
  // Present-and-empty is not absent. `flag_string` distinguishes them and
  // the handler depends on the distinction.
  scratch_dir scratch;
  seed(scratch, 1);
  REQUIRE(run_verb(scratch, {"pull", "1", "--no-locality-probe"}).code == 0);
  auto const token = scalar_text(scratch, "select claim_token from agent_work_claims where id = 1");

  REQUIRE(run_verb(scratch, {"heartbeat", "--claim", token}).code == 0);
  CHECK(scalar_text(scratch, "select count(*) from agent_actions where action_kind = 'heartbeat'") == "0");

  REQUIRE(run_verb(scratch, {"heartbeat", "--claim", token, "--status", ""}).code == 0);
  CHECK(scalar_text(scratch, "select count(*) from agent_actions where action_kind = 'heartbeat'") == "1");
  CHECK(scalar_text(scratch, "select summary from agent_actions where action_kind = 'heartbeat'").empty());
}

TEST_CASE("peek opens the database but writes nothing", "[cmd][agent][handlers]") {
  scratch_dir scratch;
  seed(scratch, 2);

  auto const peeked = run_verb(scratch, {"peek", "1"});
  CHECK(peeked.code == 0);
  CHECK(peeked.out == "next: task:1 status:todo\n");
  CHECK(scalar_text(scratch, "select count(*) from agent_work_claims") == "0");
  CHECK(scalar_text(scratch, "select count(*) from agent_actions") == "0");
  // ...and not even a session, which every WRITING verb mints.
  CHECK(scalar_text(scratch, "select count(*) from sessions") == "0");
  CHECK(scalar_text(scratch, "select status from tasks where id = 1") == "todo");
}

TEST_CASE("run start reports duplicate workflow identifiers with the oracle tag", "[cmd][agent][runs]") {
  scratch_dir scratch;
  seed(scratch, 1);
  auto const first = run_verb(
      scratch, {"run", "start", "--plan", "1", "--workflow", "wf", "--run-id", "same", "--pid", "1", "--repo-root", "/tmp"});
  REQUIRE(first.code == 0);
  auto const duplicate = run_verb(
      scratch, {"run", "start", "--plan", "1", "--workflow", "wf", "--run-id", "same", "--pid", "2", "--repo-root", "/tmp"});
  CHECK(duplicate.code == 1);
  CHECK(duplicate.err == "error: insert workflow_runs: StepFailed\n");
  CHECK(scalar_text(scratch, "select count(*) from workflow_runs where run_identifier = 'same'") == "1");
}

TEST_CASE("run start accepts a pid-less run with --ttl and sets expires_at", "[cmd][agent][runs][6847]") {
  scratch_dir scratch;
  seed(scratch, 1);
  auto const started = run_verb(
      scratch, {"run", "start", "--plan", "1", "--workflow", "wf", "--run-id", "lease-a", "--ttl", "8h", "--repo-root", "/tmp"});
  REQUIRE(started.code == 0);
  CHECK(scalar_text(scratch, "select pid is null from workflow_runs where run_identifier = 'lease-a'") == "1");
  CHECK(scalar_text(scratch, "select expires_at is not null from workflow_runs where run_identifier = 'lease-a'") == "1");
  CHECK(scalar_text(scratch, "select status from workflow_runs where run_identifier = 'lease-a'") == "running");

  // Roughly 8h out from `started_at` — a bounds check, not exact-string.
  CHECK(scalar_text(scratch, "select (julianday(expires_at) - julianday(started_at)) * 24 > 7.9 "
                             "from workflow_runs where run_identifier = 'lease-a'") == "1");
  CHECK(scalar_text(scratch, "select (julianday(expires_at) - julianday(started_at)) * 24 < 8.1 "
                             "from workflow_runs where run_identifier = 'lease-a'") == "1");
}

TEST_CASE("run start refuses when neither --pid nor --ttl is given, writing no row", "[cmd][agent][runs][6847]") {
  scratch_dir scratch;
  seed(scratch, 1);
  auto const refused =
      run_verb(scratch, {"run", "start", "--plan", "1", "--workflow", "wf", "--run-id", "neither", "--repo-root", "/tmp"});
  CHECK(refused.code != 0);
  CHECK(refused.err.contains("--pid"));
  CHECK(refused.err.contains("--ttl"));
  CHECK(scalar_text(scratch, "select count(*) from workflow_runs") == "0");
}

TEST_CASE("run start refuses --pid and --ttl together, writing no row", "[cmd][agent][runs][6847]") {
  scratch_dir scratch;
  seed(scratch, 1);
  // Both given is a contradiction. Before task 6847's reviewer caught it,
  // --pid silently won and --ttl was discarded, so a caller asking for a
  // lease got a pid-supervised run whose every later `run heartbeat`
  // refused with `pid-supervised`.
  auto const refused = run_verb(scratch, {"run", "start", "--plan", "1", "--workflow", "wf", "--run-id", "both", "--pid", "4242",
                                          "--ttl", "600", "--repo-root", "/tmp"});
  CHECK(refused.code != 0);
  CHECK(refused.err.contains("--pid"));
  CHECK(refused.err.contains("--ttl"));
  CHECK(refused.err.contains("not both"));
  CHECK(scalar_text(scratch, "select count(*) from workflow_runs") == "0");
}

TEST_CASE("run heartbeat extends a pid-less run's lease forward", "[cmd][agent][runs][6847]") {
  scratch_dir scratch;
  seed(scratch, 1);
  REQUIRE(run_verb(scratch, {"run", "start", "--plan", "1", "--workflow", "wf", "--run-id", "lease-b", "--ttl", "600",
                             "--repo-root", "/tmp"})
              .code == 0);
  auto const before = scalar_text(scratch, "select expires_at from workflow_runs where run_identifier = 'lease-b'");

  auto const beat = run_verb(scratch, {"run", "heartbeat", "--run-id", "lease-b", "--ttl", "3600"});
  CHECK(beat.code == 0);
  auto const after = scalar_text(scratch, "select expires_at from workflow_runs where run_identifier = 'lease-b'");
  CHECK(after > before);
}

TEST_CASE("run heartbeat refuses a pid-bound run and a non-running run, each with its own named error",
          "[cmd][agent][runs][6847]") {
  scratch_dir scratch;
  seed(scratch, 1);
  REQUIRE(run_verb(scratch, {"run", "start", "--plan", "1", "--workflow", "wf", "--run-id", "pid-run", "--pid", "4242",
                             "--repo-root", "/tmp"})
              .code == 0);
  auto const on_pid_bound = run_verb(scratch, {"run", "heartbeat", "--run-id", "pid-run", "--ttl", "600"});
  CHECK(on_pid_bound.code != 0);
  CHECK(on_pid_bound.err.contains("pid-supervised"));
  CHECK(scalar_text(scratch, "select expires_at is null from workflow_runs where run_identifier = 'pid-run'") == "1");

  REQUIRE(run_verb(scratch, {"run", "start", "--plan", "1", "--workflow", "wf", "--run-id", "lease-c", "--ttl", "600",
                             "--repo-root", "/tmp"})
              .code == 0);
  REQUIRE(run_verb(scratch, {"run", "end", "--run-id", "lease-c", "--status", "completed"}).code == 0);
  auto const on_terminal = run_verb(scratch, {"run", "heartbeat", "--run-id", "lease-c", "--ttl", "600"});
  CHECK(on_terminal.code != 0);
  CHECK(on_terminal.err.contains("terminal status"));
}

TEST_CASE("reconcile abandons a pid-less run only after its lease has lapsed", "[cmd][agent][runs][6847]") {
  scratch_dir scratch;
  seed(scratch, 1);
  {
    // Seeded PAST expires_at, deterministic rather than sleeping.
    auto conn = planar::db::connection::open(scratch.db_path().string());
    REQUIRE(conn.has_value());
    REQUIRE(conn->execute("insert into workflow_runs (plan_id, workflow_name, run_identifier, expires_at, repo_root) "
                          "values (1, 'wf', 'lapsed', '2000-01-01T00:00:00.000Z', '/tmp')")
                .has_value());
  }
  REQUIRE(run_verb(scratch, {"run", "start", "--plan", "1", "--workflow", "wf", "--run-id", "still-leased", "--ttl", "8h",
                             "--repo-root", "/tmp"})
              .code == 0);

  auto const swept = run_verb(scratch, {"reconcile"});
  CHECK(swept.code == 0);
  CHECK(scalar_text(scratch, "select status from workflow_runs where run_identifier = 'lapsed'") == "abandoned");
  CHECK(scalar_text(scratch, "select status from workflow_runs where run_identifier = 'still-leased'") == "running");
}

TEST_CASE("reconcile --dry-run opens the database and writes nothing", "[cmd][agent][handlers]") {
  scratch_dir scratch;
  seed(scratch, 1);
  REQUIRE(run_verb(scratch, {"pull", "1", "--no-locality-probe"}).code == 0);
  // Expire the lease by hand — the CLI cannot mint a negative TTL.
  {
    auto conn = planar::db::connection::open(scratch.db_path().string());
    REQUIRE(conn.has_value());
    REQUIRE(conn->execute("update agent_work_claims set lease_expires_at = '2000-01-01T00:00:00.000Z'").has_value());
  }

  auto const preview = run_verb(scratch, {"reconcile", "--dry-run"});
  CHECK(preview.code == 0);
  CHECK(preview.out.starts_with("dry-run: 1 claim candidate(s), 0 run candidate(s)\n"));
  CHECK(scalar_text(scratch, "select status from agent_work_claims where id = 1") == "active");
  CHECK(scalar_text(scratch, "select status from tasks where id = 1") == "doing");

  auto const applied = run_verb(scratch, {"reconcile"});
  CHECK(applied.code == 0);
  // ZERO actions closed, and the zero is informative: the sweep's action
  // close targets `claim_check` rows (the direct-claim marker), and a
  // PULLED claim's action is a `coder` row instead. The orphan sweep does
  // not fire either, because the owning session is still live. Written as
  // 1 first and corrected by the failure.
  CHECK(applied.out == "reconciled: 1 claim(s) stale, 0 action(s) closed, 0 run(s) abandoned\n");
  CHECK(scalar_text(scratch, "select status from agent_work_claims where id = 1") == "stale");
  CHECK(scalar_text(scratch, "select status from tasks where id = 1") == "todo");
}

TEST_CASE("completing a CHILD plan's last task rolls the plan up to done", "[cmd][agent][handlers]") {
  // The other half of the roll-up contract. A child plan (one with a
  // parent) DOES auto-promote to `done` when every task is terminal, and
  // the recompute runs inside the terminal transaction through the
  // injected policy — the engine bucket cannot reach `engine_planning`
  // itself, so a `done` here can only have come through that seam.
  scratch_dir scratch;
  seed(scratch, 1);
  {
    auto conn = planar::db::connection::open(scratch.db_path().string());
    REQUIRE(conn.has_value());
    REQUIRE(conn->execute("insert into plans (scope_kind, title, slug, parent_plan_id) "
                          "values ('global','child','child-plan',1)")
                .has_value());
    REQUIRE(
        conn->execute("insert into tasks (scope_kind, plan_id, title, status) values ('global',2,'only','todo')").has_value());
  }

  REQUIRE(run_verb(scratch, {"pull", "2", "--no-locality-probe"}).code == 0);
  auto const token = scalar_text(scratch, "select claim_token from agent_work_claims where id = 1");
  REQUIRE(run_verb(scratch, {"complete", "--claim", token}).code == 0);

  CHECK(scalar_text(scratch, "select status from plans where id = 2") == "done");
  // The anchor above it is untouched — `recompute_status` is documented as
  // single-plan and never walks `parent_plan_id`.
  CHECK(scalar_text(scratch, "select status from plans where id = 1") == "active");
}

TEST_CASE("dispatch preview freezes state and confirm spends it once", "[cmd][agent][routing]") {
  scratch_dir scratch;
  seed(scratch, 1);
  {
    auto conn = planar::db::connection::open(scratch.db_path().string());
    REQUIRE(conn.has_value());
    REQUIRE(conn->execute("insert into projects (slug,name) values ('routing-project','Routing Project')").has_value());
    REQUIRE(conn->execute("insert into routing_candidates (vendor,candidate_id,fallback_order) values ('codex','candidate',0)")
                .has_value());
  }
  auto const preview = run_verb(scratch, {"dispatch",
                                          "preview",
                                          "--task",
                                          "1",
                                          "--work-item",
                                          "w-1",
                                          "--project",
                                          "1",
                                          "--validation-policy",
                                          "v1",
                                          "--routing-policy",
                                          "r1",
                                          "--profile-rule",
                                          "p1",
                                          "--vendor",
                                          "codex",
                                          "--role",
                                          "coder",
                                          "--tier",
                                          "medium",
                                          "--work-type",
                                          "feature",
                                          "--complexity",
                                          "high-risk",
                                          "--packet-digest",
                                          "packet",
                                          "--profile-digest",
                                          "profile",
                                          "--policy-digest",
                                          "policy",
                                          "--capability-digest",
                                          "capability",
                                          "--candidate",
                                          "1",
                                          "--host",
                                          "host",
                                          "--class",
                                          "default",
                                          "--evidence-state",
                                          "evidential",
                                          "--expires-at",
                                          "2099-01-01T00:00:00Z",
                                          "--json"});
  REQUIRE(preview.code == 0);
  auto const token   = scalar_text(scratch, "select preview_token from routing_dispatch_previews where id = 1");
  auto const confirm = run_verb(scratch, {"dispatch",
                                          "confirm",
                                          "--token",
                                          token,
                                          "--dispatch-key",
                                          "dispatch-1",
                                          "--now",
                                          "2026-01-01T00:00:00Z",
                                          "--packet-digest",
                                          "packet",
                                          "--profile-digest",
                                          "profile",
                                          "--policy-digest",
                                          "policy",
                                          "--capability-digest",
                                          "capability",
                                          "--candidate",
                                          "1",
                                          "--vendor",
                                          "codex",
                                          "--role",
                                          "coder",
                                          "--tier",
                                          "medium",
                                          "--work-type",
                                          "feature",
                                          "--complexity",
                                          "high-risk",
                                          "--validation-policy",
                                          "v1",
                                          "--routing-policy",
                                          "r1",
                                          "--json"});
  INFO(confirm.err);
  REQUIRE(confirm.code == 0);
  CHECK(scalar_text(scratch, "select count(*) from routing_dispatch_snapshots") == "1");
  CHECK(scalar_text(scratch, "select consumed_dispatch_id from routing_dispatch_previews where id = 1") == "1");
  auto const replay = run_verb(scratch, {"dispatch",
                                         "confirm",
                                         "--token",
                                         token,
                                         "--dispatch-key",
                                         "dispatch-2",
                                         "--now",
                                         "2026-01-01T00:00:00Z",
                                         "--packet-digest",
                                         "packet",
                                         "--profile-digest",
                                         "profile",
                                         "--policy-digest",
                                         "policy",
                                         "--capability-digest",
                                         "capability",
                                         "--candidate",
                                         "1",
                                         "--vendor",
                                         "codex",
                                         "--role",
                                         "coder",
                                         "--tier",
                                         "medium",
                                         "--work-type",
                                         "feature",
                                         "--complexity",
                                         "standard",
                                         "--validation-policy",
                                         "v1",
                                         "--routing-policy",
                                         "r1"});
  CHECK(replay.code == 1);
  CHECK(replay.err == "error: stale_preview: already_consumed\n");
  CHECK(scalar_text(scratch, "select count(*) from routing_dispatch_snapshots") == "1");
  // Wire spelling is hyphenated; accepting the internal enum spelling would
  // leak an implementation detail into the public CLI contract.
  auto const internal_spelling = run_verb(scratch, {"dispatch",
                                                    "confirm",
                                                    "--token",
                                                    token,
                                                    "--dispatch-key",
                                                    "bad",
                                                    "--now",
                                                    "2026-01-01T00:00:00Z",
                                                    "--packet-digest",
                                                    "packet",
                                                    "--profile-digest",
                                                    "profile",
                                                    "--policy-digest",
                                                    "policy",
                                                    "--capability-digest",
                                                    "capability",
                                                    "--candidate",
                                                    "1",
                                                    "--vendor",
                                                    "codex",
                                                    "--role",
                                                    "coder",
                                                    "--tier",
                                                    "medium",
                                                    "--work-type",
                                                    "feature",
                                                    "--complexity",
                                                    "high_risk",
                                                    "--validation-policy",
                                                    "v1",
                                                    "--routing-policy",
                                                    "r1"});
  CHECK(internal_spelling.code == 2);
  routing::binding replay_binding{};
  CHECK(routing::classify_stale(replay_binding, replay_binding, "2099-01-01T00:00:00Z", true, "2026-01-01T00:00:00Z") ==
        routing::stale_reason::already_consumed);
}

TEST_CASE("dispatch confirm names every frozen binding drift and writes nothing", "[cmd][agent][routing]") {
  struct mutation {
    std::string_view                                     name;
    routing::stale_reason                                reason;
    std::function<void(routing::binding&, std::string&)> apply;
  };
  std::vector<mutation> cases{
      {"expired", routing::stale_reason::expired, [](auto&, auto& now) { now = "2100-01-01T00:00:00Z"; }},
      {"packet", routing::stale_reason::packet_changed, [](auto& b, auto&) { b.packet_digest = "other"; }},
      {"profile", routing::stale_reason::profile_changed, [](auto& b, auto&) { b.profile_digest = "other"; }},
      {"policy", routing::stale_reason::policy_changed, [](auto& b, auto&) { b.policy_digest = "other"; }},
      {"capability", routing::stale_reason::capability_changed, [](auto& b, auto&) { b.capability_digest = "other"; }},
      {"vendor", routing::stale_reason::cohort_changed, [](auto& b, auto&) { b.vendor = "other"; }},
      {"role", routing::stale_reason::cohort_changed, [](auto& b, auto&) { b.role = "reviewer"; }},
      {"tier", routing::stale_reason::cohort_changed, [](auto& b, auto&) { b.tier = "large"; }},
      {"work type", routing::stale_reason::cohort_changed, [](auto& b, auto&) { b.work_type = "engine"; }},
      {"complexity", routing::stale_reason::cohort_changed, [](auto& b, auto&) { b.complexity = "bounded"; }},
      {"candidate", routing::stale_reason::candidate_changed, [](auto& b, auto&) { b.candidate_id = 2; }},
      {"claim token", routing::stale_reason::claim_changed, [](auto& b, auto&) { b.claim = "other"; }},
      {"claim status", routing::stale_reason::claim_changed, [](auto& b, auto&) { b.claim_status = "released"; }},
  };
  for (auto const& item : cases) {
    scratch_dir scratch;
    auto        conn = planar::db::connection::open(scratch.db_path().string());
    REQUIRE(conn);
    REQUIRE(planar::db::apply_all(*conn));
    REQUIRE(conn->execute("insert into projects (slug,name) values ('p','p')"));
    REQUIRE(conn->execute(
        "insert into routing_candidates (vendor,candidate_id,fallback_order) values ('codex','one',0),('codex','two',1)"));
    routing::binding bound{.work_item         = "w",
                           .validation_policy = "v",
                           .routing_policy    = "r",
                           .profile_rule      = "p",
                           .vendor            = "codex",
                           .role              = "coder",
                           .tier              = "medium",
                           .work_type         = "feature",
                           .complexity        = "standard",
                           .project_id        = 1,
                           .candidate_id      = 1,
                           .packet_digest     = "packet",
                           .profile_digest    = "profile",
                           .policy_digest     = "policy",
                           .capability_digest = "capability",
                           .host              = "host",
                           .assignment_class  = "default",
                           .evidence_state    = "evidential",
                           .claim             = "claim",
                           .claim_status      = "active"};
    auto             preview = routing::preview(*conn, bound, "2099-01-01T00:00:00Z");
    REQUIRE(preview);
    if (item.name == "expired")
      CHECK(routing::classify_stale(bound, bound, "2099-01-01T00:00:00Z", true, "2026-01-01T00:00:00Z") ==
            routing::stale_reason::already_consumed);
    auto        current = bound;
    std::string now{"2026-01-01T00:00:00Z"};
    item.apply(current, now);
    routing::stale_reason why{};
    auto confirmed = routing::confirm(*conn, preview->token, "dispatch", current, now, "required", "confirmed", &why);
    INFO(item.name);
    REQUIRE_FALSE(confirmed);
    CHECK(confirmed.error() == routing::error::stale_preview);
    CHECK(why == item.reason);
    auto stmt = conn->prepare(
        "select consumed_at is null, (select count(*) from routing_dispatch_snapshots) from routing_dispatch_previews");
    REQUIRE(stmt);
    REQUIRE(stmt->step());
    CHECK(stmt->column_int64(0) == 1);
    CHECK(stmt->column_int64(1) == 0);
  }
}

// ===========================================================================
// Engine supervision (plan 1033 M0, task 6488; tech-spec D3/D4)
// ===========================================================================
//
// The test-spec scenarios, walked through the real tree and handler table.
// Every assertion is on post-state — claim row, task row, lease, action rows
// — not on the exit code alone.

namespace {

/// @brief Seed one plan with `task_count` todo tasks, pull task 1, and
/// return its claim token.
auto pulled(scratch_dir const& scratch, int task_count) -> std::string {
  seed(scratch, task_count);
  REQUIRE(run_verb(scratch, {"pull", "1", "--no-locality-probe"}).code == 0);
  return scalar_text(scratch, "select claim_token from agent_work_claims where id = 1");
}

/// @brief Count the claim-1 action rows of `kind`.
auto actions_of(scratch_dir const& scratch, std::string_view kind) -> std::string {
  return scalar_text(scratch, std::format("select count(*) from agent_actions where claim_id = 1 and action_kind = '{}'", kind));
}

} // namespace

TEST_CASE("a caller-supervised claim takes the ritual exactly as before", "[cmd][agent][supervision][6488]") {
  scratch_dir scratch;
  auto const  token = pulled(scratch, 1);
  CHECK(scalar_text(scratch, "select supervisor || ':' || ifnull(attempt_id, 'null') from agent_work_claims where id = 1") ==
        "caller:null");
  CHECK(run_verb(scratch, {"heartbeat", "--claim", token}).code == 0);
  CHECK(run_verb(scratch, {"complete", "--claim", token, "--no-locality-probe"}).code == 0);
  CHECK(scalar_text(scratch, "select status from agent_work_claims where id = 1") == "completed");
  CHECK(scalar_text(scratch, "select status from tasks where id = 1") == "done");
  // No supervision rows on a claim nobody supervised.
  CHECK(scalar_text(scratch, "select count(*) from agent_actions where action_kind in ('claim_associate', 'claim_terminal', "
                             "'supervisor_override')") == "0");
}

TEST_CASE("claim-associate hands a claim to the engine one way, idempotently by attempt", "[cmd][agent][supervision][6488]") {
  scratch_dir scratch;
  auto const  token = pulled(scratch, 1);

  auto const first =
      run_verb(scratch, {"claim-associate", "--claim", token, "--supervisor", "engine", "--attempt", "A1", "--json"});
  REQUIRE(first.code == 0);
  CHECK(first.out == std::format(R"({{"ok":true,"claim_token":"{}","supervisor":"engine","attempt_id":"A1","changed":true}})"
                                 "\n",
                                 token));
  CHECK(scalar_text(scratch, "select supervisor || ':' || attempt_id from agent_work_claims where id = 1") == "engine:A1");
  CHECK(actions_of(scratch, "claim_associate") == "1");

  // Same attempt again: success, nothing written.
  auto const repeat =
      run_verb(scratch, {"claim-associate", "--claim", token, "--supervisor", "engine", "--attempt", "A1", "--json"});
  REQUIRE(repeat.code == 0);
  CHECK(repeat.out.contains(R"("changed":false)"));
  CHECK(actions_of(scratch, "claim_associate") == "1");

  // A new attempt moves the claim and logs exactly one more row.
  REQUIRE(run_verb(scratch, {"claim-associate", "--claim", token, "--supervisor", "engine", "--attempt", "A2"}).code == 0);
  CHECK(scalar_text(scratch, "select attempt_id from agent_work_claims where id = 1") == "A2");
  CHECK(actions_of(scratch, "claim_associate") == "2");

  // One way: never back to the caller.
  auto const back = run_verb(scratch, {"claim-associate", "--claim", token, "--supervisor", "caller"});
  CHECK(back.code == 1);
  CHECK(back.err == "error: claim-associate: SupervisorMismatch\n");
  CHECK(scalar_text(scratch, "select supervisor from agent_work_claims where id = 1") == "engine");

  // The first attempt is now a stranger to this claim.
  auto const stale =
      run_verb(scratch, {"complete", "--claim", token, "--as", "engine", "--attempt", "A1", "--no-locality-probe"});
  CHECK(stale.code == 1);
  CHECK(stale.err == "error: complete: AttemptMismatch\n");
  CHECK(scalar_text(scratch, "select status from agent_work_claims where id = 1") == "active");
}

TEST_CASE("on an engine claim the caller may report status but not extend the lease or terminate",
          "[cmd][agent][supervision][6488]") {
  scratch_dir scratch;
  auto const  token = pulled(scratch, 1);
  REQUIRE(run_verb(scratch, {"claim-associate", "--claim", token, "--supervisor", "engine", "--attempt", "A1"}).code == 0);
  auto const lease = scalar_text(scratch, "select lease_expires_at from agent_work_claims where id = 1");

  for (auto const& verb : std::vector<std::vector<std::string>>{
           {"complete", "--claim", token, "--no-locality-probe"},
           {"fail", "--claim", token, "--reason", "r", "--no-locality-probe"},
           {"release", "--claim", token, "--no-locality-probe"},
           {"block", "--claim", token, "--blocker", "1", "--no-locality-probe"},
           {"heartbeat", "--claim", token},
           {"heartbeat", "--claim", token, "--status", "s", "--ttl", "1h"},
       }) {
    INFO(verb.front());
    auto const refused = run_verb(scratch, verb);
    CHECK(refused.code == 1);
    CHECK(refused.err == std::format("error: {}: SupervisorMismatch\n", verb.front()));
  }
  // Nothing moved: claim live, task doing, lease untouched.
  CHECK(scalar_text(scratch, "select status from agent_work_claims where id = 1") == "active");
  CHECK(scalar_text(scratch, "select status from tasks where id = 1") == "doing");
  CHECK(scalar_text(scratch, "select lease_expires_at from agent_work_claims where id = 1") == lease);

  // A status report lands, and still leaves the lease alone.
  REQUIRE(run_verb(scratch, {"heartbeat", "--claim", token, "--status", "editing foo"}).code == 0);
  CHECK(scalar_text(scratch, "select summary from agent_actions where claim_id = 1 and action_kind = 'heartbeat'") ==
        "editing foo");
  CHECK(scalar_text(scratch, "select lease_expires_at from agent_work_claims where id = 1") == lease);

  // The engine, under its attempt, does extend it.
  CHECK(run_verb(scratch, {"heartbeat", "--claim", token, "--as", "engine", "--attempt", "A9"}).err ==
        "error: heartbeat: AttemptMismatch\n");
  REQUIRE(run_verb(scratch, {"heartbeat", "--claim", token, "--as", "engine", "--attempt", "A1", "--ttl", "2h"}).code == 0);
  CHECK(scalar_text(scratch, "select lease_expires_at > '" + lease + "' from agent_work_claims where id = 1") == "1");
}

TEST_CASE("the engine's terminal verb lands once and replays as a no-op under the same attempt",
          "[cmd][agent][supervision][6488]") {
  scratch_dir scratch;
  auto const  token = pulled(scratch, 1);
  REQUIRE(run_verb(scratch, {"claim-associate", "--claim", token, "--supervisor", "engine", "--attempt", "A1"}).code == 0);

  auto const done = run_verb(scratch, {"complete", "--claim", token, "--as", "engine", "--attempt", "A1", "--no-locality-probe"});
  REQUIRE(done.code == 0);
  CHECK(scalar_text(scratch, "select status from agent_work_claims where id = 1") == "completed");
  CHECK(scalar_text(scratch, "select status from tasks where id = 1") == "done");
  CHECK(actions_of(scratch, "claim_terminal") == "1");
  auto const rows = scalar_text(scratch, "select count(*) from agent_actions");

  // Replay (post-crash reconciliation re-issuing the verb): exit 0, the same
  // envelope, and not one row written.
  auto const replay =
      run_verb(scratch, {"complete", "--claim", token, "--as", "engine", "--attempt", "A1", "--no-locality-probe"});
  CHECK(replay.code == 0);
  CHECK(replay.out == done.out);
  CHECK(scalar_text(scratch, "select count(*) from agent_actions") == rows);
  CHECK(actions_of(scratch, "claim_terminal") == "1");

  // Another attempt is refused, terminal claim or not.
  auto const other =
      run_verb(scratch, {"complete", "--claim", token, "--as", "engine", "--attempt", "A2", "--no-locality-probe"});
  CHECK(other.code == 1);
  CHECK(other.err == "error: complete: AttemptMismatch\n");
}

TEST_CASE("an engine verb on a caller claim is refused, and an operator override is logged", "[cmd][agent][supervision][6488]") {
  scratch_dir scratch;
  auto const  token = pulled(scratch, 2);

  // The engine cannot act on a claim it was never handed.
  auto const foreign =
      run_verb(scratch, {"release", "--claim", token, "--as", "engine", "--attempt", "A1", "--no-locality-probe"});
  CHECK(foreign.code == 1);
  CHECK(foreign.err == "error: release: SupervisorMismatch\n");
  CHECK(run_verb(scratch, {"heartbeat", "--claim", token, "--as", "engine", "--attempt", "A1"}).err ==
        "error: heartbeat: SupervisorMismatch\n");

  REQUIRE(run_verb(scratch, {"claim-associate", "--claim", token, "--supervisor", "engine", "--attempt", "A1"}).code == 0);
  REQUIRE(run_verb(scratch, {"release", "--claim", token, "--override-supervisor", "--no-locality-probe"}).code == 0);
  CHECK(scalar_text(scratch, "select status from agent_work_claims where id = 1") == "released");
  CHECK(scalar_text(scratch, "select status from tasks where id = 1") == "todo");
  CHECK(actions_of(scratch, "supervisor_override") == "1");
  CHECK(scalar_text(scratch, "select summary from agent_actions where action_kind = 'supervisor_override'") ==
        "release by caller overriding the engine supervisor (attempt A1)");
  CHECK(actions_of(scratch, "claim_terminal") == "0");
}

TEST_CASE("supervision flags are cross-checked before anything is written", "[cmd][agent][supervision][6488]") {
  scratch_dir scratch;
  auto const  token = pulled(scratch, 1);
  struct refusal {
    std::vector<std::string> argv;
    std::string              err;
  };
  for (auto const& r : std::vector<refusal>{
           {{"complete", "--claim", token, "--as", "engine"}, "error: --as engine requires --attempt <id>\n"},
           {{"complete", "--claim", token, "--attempt", "A1"}, "error: --attempt applies only with --as engine\n"},
           {{"complete", "--claim", token, "--as", "engine", "--attempt", "A1", "--override-supervisor"},
            "error: --override-supervisor is for the caller; it cannot be combined with --as engine\n"},
           {{"claim-associate", "--claim", token},
            "error: claim-associate needs --run <id>, --supervisor <caller|engine>, or both\n"},
           {{"claim-associate", "--claim", token, "--supervisor", "engine"},
            "error: --supervisor engine requires --attempt <id>\n"},
           {{"claim-associate", "--claim", token, "--supervisor", "caller", "--attempt", "A1"},
            "error: --attempt applies only with --supervisor engine\n"},
           {{"action", "start", "--claim", token, "--kind", "claim_terminal"},
            "error: action kind 'claim_terminal' is written only by the supervised claim verbs, not by action start\n"},
       }) {
    INFO(r.argv.front());
    auto const got = run_verb(scratch, r.argv);
    CHECK(got.code == 2);
    CHECK(got.err == r.err);
  }
  CHECK(scalar_text(scratch, "select supervisor || ':' || status from agent_work_claims where id = 1") == "caller:active");
  // `pull --role` never mints a supervision row either: an unrecognised or
  // supervision role degrades to `coder`.
  scratch_dir other;
  seed(other, 1);
  REQUIRE(run_verb(other, {"pull", "1", "--role", "claim_terminal", "--no-locality-probe"}).code == 0);
  CHECK(scalar_text(other, "select action_kind from agent_actions where id = 1") == "coder");
}

// ===========================================================================
// Reconcile and abort leave engine-supervised work alone (task 6489)
// ===========================================================================

namespace {

/// @brief Run one write against the scratch database.
auto write_sql(scratch_dir const& scratch, std::string_view sql) -> void {
  auto conn = planar::db::connection::open(scratch.db_path().string());
  REQUIRE(conn.has_value());
  REQUIRE(conn->execute(sql).has_value());
}

/// @brief Two pulled tasks, claim 1 handed to the engine under A1, both
/// leases expired; two running workflow runs with dead pids, one per
/// engine. No CLI verb can yet expire a lease on demand or mint a
/// `centurion` run (the host lands in M2), so those two facts are set in SQL.
auto lapsed_pair(scratch_dir const& scratch) -> void {
  seed(scratch, 2);
  REQUIRE(run_verb(scratch, {"pull", "1", "--no-locality-probe"}).code == 0);
  REQUIRE(run_verb(scratch, {"pull", "1", "--no-locality-probe"}).code == 0);
  auto const engine_token = scalar_text(scratch, "select claim_token from agent_work_claims where id = 1");
  REQUIRE(run_verb(scratch, {"claim-associate", "--claim", engine_token, "--supervisor", "engine", "--attempt", "A1"}).code == 0);
  write_sql(scratch, "update agent_work_claims set lease_expires_at = '2000-01-01T00:00:00.000Z'");
  write_sql(scratch, "insert into workflow_runs (plan_id, workflow_name, run_identifier, pid, repo_root, engine) "
                     "values (1, 'w', 'emb', -1, '/r', 'embedded'), (1, 'w', 'cent', -1, '/r', 'centurion')");
}

} // namespace

TEST_CASE("reconcile skips engine claims and centurion runs, and still reconciles the caller's",
          "[cmd][agent][supervision][6489]") {
  scratch_dir scratch;
  lapsed_pair(scratch);

  auto const swept = run_verb(scratch, {"reconcile", "--json"});
  REQUIRE(swept.code == 0);
  auto const engine_token = scalar_text(scratch, "select claim_token from agent_work_claims where id = 1");
  CHECK(swept.out == std::format(R"({{"ok":true,"claims_marked_stale":1,"actions_closed":0,"runs_abandoned":1,)"
                                 R"("skipped_engine_claims":[{{"kind":"task","id":1,"claim_token":"{}"}}],)"
                                 R"("skipped_engine_runs":[{{"id":2,"run_identifier":"cent","pid":-1}}]}})"
                                 "\n",
                                 engine_token));
  // The engine's claim, task and run are untouched...
  CHECK(scalar_text(scratch, "select status from agent_work_claims where id = 1") == "active");
  CHECK(scalar_text(scratch, "select status from tasks where id = 1") == "doing");
  CHECK(scalar_text(scratch, "select status from workflow_runs where run_identifier = 'cent'") == "running");
  // ...while the caller's claim and the embedded run are reconciled as before.
  CHECK(scalar_text(scratch, "select status from agent_work_claims where id = 2") == "stale");
  CHECK(scalar_text(scratch, "select status from tasks where id = 2") == "todo");
  CHECK(scalar_text(scratch, "select status from workflow_runs where run_identifier = 'emb'") == "abandoned");

  // --override-supervisor takes them over, and says so.
  auto const taken = run_verb(scratch, {"reconcile", "--override-supervisor"});
  REQUIRE(taken.code == 0);
  CHECK(taken.out == "reconciled: 1 claim(s) stale, 0 action(s) closed, 1 run(s) abandoned\n");
  CHECK(scalar_text(scratch, "select status from agent_work_claims where id = 1") == "stale");
  CHECK(scalar_text(scratch, "select status from tasks where id = 1") == "todo");
  CHECK(scalar_text(scratch, "select status from workflow_runs where run_identifier = 'cent'") == "abandoned");
  CHECK(scalar_text(scratch, "select summary from agent_actions where action_kind = 'supervisor_override'") ==
        "reconcile by operator overriding the engine supervisor (attempt A1)");
}

TEST_CASE("a caller-only reconcile prints exactly what it always did", "[cmd][agent][supervision][6489]") {
  scratch_dir scratch;
  seed(scratch, 1);
  REQUIRE(run_verb(scratch, {"pull", "1", "--no-locality-probe"}).code == 0);
  write_sql(scratch, "update agent_work_claims set lease_expires_at = '2000-01-01T00:00:00.000Z'");
  auto const swept = run_verb(scratch, {"reconcile", "--json"});
  REQUIRE(swept.code == 0);
  CHECK(swept.out == R"({"ok":true,"claims_marked_stale":1,"actions_closed":0,"runs_abandoned":0})"
                     "\n");
  CHECK(run_verb(scratch, {"reconcile", "--dry-run"}).out == "dry-run: 0 claim candidate(s), 0 run candidate(s)\n");
}

TEST_CASE("abort refuses an engine claim unless overridden, and logs the override", "[cmd][agent][supervision][6489]") {
  scratch_dir scratch;
  auto const  token = pulled(scratch, 1);
  REQUIRE(run_verb(scratch, {"claim-associate", "--claim", token, "--supervisor", "engine", "--attempt", "A1"}).code == 0);

  auto const refused = run_verb(scratch, {"abort", "--claim", token});
  CHECK(refused.code == 1);
  CHECK(refused.err == "error: abort: SupervisorMismatch\n");
  CHECK(scalar_text(scratch, "select status from agent_work_claims where id = 1") == "active");
  CHECK(scalar_text(scratch, "select status from tasks where id = 1") == "doing");

  REQUIRE(run_verb(scratch, {"abort", "--claim", token, "--override-supervisor"}).code == 0);
  CHECK(scalar_text(scratch, "select status from agent_work_claims where id = 1") == "aborted");
  CHECK(actions_of(scratch, "supervisor_override") == "1");
  CHECK(scalar_text(scratch, "select summary from agent_actions where action_kind = 'supervisor_override'") ==
        "abort by operator overriding the engine supervisor (attempt A1)");
}
