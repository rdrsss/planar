// @file agentrender.t.cpp
// @brief Unit tests for `planar.engine.runtime.agentrender` (plan 996,
// task 6038).
//
// The expected bytes below are ORACLE CAPTURES, pasted and then had their
// volatile fields (32-hex tokens, millisecond timestamps, the scratch
// path) substituted with the fixture's own. Two representative captures,
// verbatim from `zig/zig-out/bin/planar-agent` against a scratch database:
//
//   $ planar-agent pull 1 --json
//   {"ok":true,"no_work":false,"claim_token":"14dc6e77…","claim":{"id":2,
//   "claim_token":"14dc6e77…","session_id":1,"entity_kind":"task",
//   "entity_id":2,"claim_scope":"exclusive","status":"active","vendor":
//   "planar-agent","vendor_session_id":null,"role":null,"model":null,
//   "worktree_id":null,"worktree_path":null,"repo_root":"/private/tmp/…",
//   "branch":null,"head_sha_at_claim":null,"dirty_at_claim":"unknown",
//   "purpose":null,"base_ref":null,"claimed_at":"…","last_heartbeat_at":
//   "…","lease_expires_at":"…","released_at":null,"release_reason":null,
//   "failure_category":null,"run_id":null,"stage":null},"task":{…},
//   "action_id":2}
//
//   $ planar-agent reconcile --dry-run --json
//   {"ok":true,"claims_marked_stale":0,"actions_closed":0,
//   "runs_abandoned":0,"candidates":[{"kind":"task","id":4,"claim":{…}}],
//   "run_candidates":[]}
//
// FIELD ORDER IS THE CONTRACT. The Zig originals are hand-rolled `print`
// calls with no serializer to normalise anything, so the emitted key order
// is whatever the source says. Note in particular that `failure_category`
// comes BEFORE `run_id` and `stage` in the claim object even though it was
// added to the table by a LATER migration than either — a reasonable
// reader would put it last and be wrong.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.engine.runtime.agentactivity;
import planar.engine.runtime.agentatomic;
import planar.engine.runtime.agentrender;

namespace {

namespace aa     = planar::engine::runtime::agentactivity;
namespace atomic = planar::engine::runtime::agentatomic;
namespace render = planar::engine::runtime::agentrender;

auto sample_claim() -> aa::claim {
  return aa::claim{
      .id                = 2,
      .claim_token       = "14dc6e775153923eece9afe572bd8e5f",
      .session_id        = 1,
      .kind              = aa::entity_kind::task,
      .entity_id         = 2,
      .scope             = aa::claim_scope::exclusive,
      .status            = aa::claim_status::active,
      .vendor            = "planar-agent",
      .repo_root         = "/private/tmp/orc/proj",
      .dirty_at_claim    = aa::dirty_state::unknown,
      .claimed_at        = "2026-08-23T16:34:47.223Z",
      .last_heartbeat_at = "2026-08-23T16:34:47.223Z",
      .lease_expires_at  = "2026-08-23T16:44:47.223Z",
  };
}

auto sample_task() -> aa::task_row {
  return aa::task_row{
      .id         = 2,
      .scope_kind = "association",
      .scope_id   = 1,
      .plan_id    = 1,
      .title      = "beta task",
      .status     = "doing",
      .priority   = 100,
      .created_at = "2026-08-23T16:34:42.996Z",
      .updated_at = "2026-08-23T16:34:47.223Z",
  };
}

} // namespace

TEST_CASE("the claim object's field order matches the oracle byte-for-byte", "[agentrender]") {
  std::string out;
  render::append_claim(out, sample_claim());
  REQUIRE(out == R"({"id":2,"claim_token":"14dc6e775153923eece9afe572bd8e5f","session_id":1,"entity_kind":"task",)"
                 R"("entity_id":2,"claim_scope":"exclusive","status":"active","vendor":"planar-agent",)"
                 R"("vendor_session_id":null,"role":null,"model":null,"worktree_id":null,"worktree_path":null,)"
                 R"("repo_root":"/private/tmp/orc/proj","branch":null,"head_sha_at_claim":null,)"
                 R"("dirty_at_claim":"unknown","purpose":null,"base_ref":null,)"
                 R"("claimed_at":"2026-08-23T16:34:47.223Z","last_heartbeat_at":"2026-08-23T16:34:47.223Z",)"
                 R"("lease_expires_at":"2026-08-23T16:44:47.223Z","released_at":null,"release_reason":null,)"
                 R"("failure_category":null,"run_id":null,"stage":null})");
  // `entity_scope` and `latest_action` never appear: every planar-agent
  // call site suppresses both. They belong to planar-watch's port.
  REQUIRE(out.find("entity_scope") == std::string::npos);
  REQUIRE(out.find("latest_action") == std::string::npos);
}

TEST_CASE("the task object's field order matches the oracle byte-for-byte", "[agentrender]") {
  std::string out;
  render::append_task(out, sample_task());
  REQUIRE(out == R"({"id":2,"scope_kind":"association","scope_id":1,"plan_id":1,"parent_task_id":null,)"
                 R"("title":"beta task","body":null,"slug":null,"status":"doing","priority":100,)"
                 R"("next_action":null,"due_at":null,"created_at":"2026-08-23T16:34:42.996Z",)"
                 R"("updated_at":"2026-08-23T16:34:47.223Z"})");
}

TEST_CASE("the action object's field order matches the oracle byte-for-byte", "[agentrender]") {
  aa::action row{
      .id         = 6,
      .session_id = 1,
      .claim_id   = 4,
      .kind       = aa::action_kind::coder,
      .entity     = aa::action_entity_kind::decision,
      .entity_id  = 1,
      .vendor     = "planar-agent",
      .started_at = "2026-08-23T16:57:00.000Z",
      .ended_at   = "2026-08-23T16:58:00.000Z",
      .result     = aa::outcome::aborted,
      .summary    = "waiting",
      .metadata   = R"({"a":1})",
  };
  std::string out;
  render::append_action(out, row);
  REQUIRE(out == R"({"id":6,"session_id":1,"session_entry_id":null,"parent_action_id":null,"claim_id":4,)"
                 R"("action_kind":"coder","entity_kind":"decision","entity_id":1,"vendor":"planar-agent",)"
                 R"("vendor_role":null,"model":null,"started_at":"2026-08-23T16:57:00.000Z",)"
                 R"("ended_at":"2026-08-23T16:58:00.000Z","outcome":"aborted","summary":"waiting",)"
                 R"("head_sha":null,"dirty":null,"metadata":"{\"a\":1}"})");
}

TEST_CASE("metadata is emitted as a JSON STRING, not spliced as an object", "[agentrender]") {
  // The column is opaque text. Splicing it would turn a malformed blob
  // into malformed output, and would also change the shape a consumer
  // sees for a value the engine promises never to parse.
  aa::action  row{.id         = 1,
                  .session_id = 1,
                  .kind       = aa::action_kind::other,
                  .vendor     = "v",
                  .started_at = "t",
                  .metadata   = R"({"nested":{"deep":true}})"};
  std::string out;
  render::append_action(out, row);
  REQUIRE(out.find(R"("metadata":"{\"nested\":{\"deep\":true}}")") != std::string::npos);
}

TEST_CASE("every renderer payload ends with exactly one newline", "[agentrender]") {
  // The terminator contract: layer 3 writes these verbatim and appends
  // nothing. A renderer that omitted the newline would produce output
  // running into the shell prompt; one that emitted two would break a
  // line-oriented consumer.
  auto const              claim = sample_claim();
  auto const              task  = sample_task();
  atomic::pull_result     pulled{.no_work = false, .acquired = claim, .task_id = 2, .action_id = 2};
  atomic::terminal_result ended{.released = claim, .task_id = 2};

  std::vector<std::string> payloads{
      render::no_work_json(),
      render::no_work_text(),
      render::pull_json(pulled, task),
      render::pull_text(pulled),
      render::peek_json(task),
      render::peek_text(task),
      render::claim_json(claim),
      render::claim_text(claim),
      render::heartbeat_text(claim),
      render::terminal_json(ended, task),
      render::terminal_text(ended, task),
      render::abort_json(claim, 1),
      render::abort_text(claim, 1),
      render::associate_json(1),
      render::associate_text(1, claim.claim_token),
  };
  for (auto const& payload : payloads) {
    REQUIRE_FALSE(payload.empty());
    REQUIRE(payload.back() == '\n');
    REQUIRE(payload.find('\n') == payload.size() - 1);
  }
}

TEST_CASE("the text one-liners match the oracle captures", "[agentrender]") {
  auto const              claim = sample_claim();
  auto const              task  = sample_task();
  atomic::pull_result     pulled{.no_work = false, .acquired = claim, .task_id = 2, .action_id = 2};
  atomic::terminal_result ended{.released = claim, .task_id = 2};

  REQUIRE(render::pull_text(pulled) == "pulled task:2 claim:14dc6e775153923eece9afe572bd8e5f action:2\n");
  REQUIRE(render::peek_text(task) == "next: task:2 status:doing\n");
  REQUIRE(render::claim_text(claim) == "claim:14dc6e775153923eece9afe572bd8e5f entity:task:2 status:active\n");
  REQUIRE(render::heartbeat_text(claim) == "ok claim:14dc6e775153923eece9afe572bd8e5f expires:2026-08-23T16:44:47.223Z\n");
  REQUIRE(render::terminal_text(ended, task) == "ok task:2 status:doing claim_status:active\n");
  REQUIRE(render::abort_text(claim, 1) == "aborted claim:14dc6e775153923eece9afe572bd8e5f by session:1\n");
  REQUIRE(render::associate_text(3, "tok") == "ok updated:3 claim:tok\n");
  REQUIRE(render::action_end_text(5, aa::outcome::error_) == "ok action:5 outcome:error\n");
  REQUIRE(render::no_work_text() == "no_work\n");
  REQUIRE(render::no_work_json() == "{\"ok\":true,\"no_work\":true}\n");
}

TEST_CASE("the pull and terminal envelopes wrap the shared objects", "[agentrender]") {
  auto const          claim = sample_claim();
  auto const          task  = sample_task();
  atomic::pull_result pulled{.no_work = false, .acquired = claim, .task_id = 2, .action_id = 2};

  auto const json = render::pull_json(pulled, task);
  REQUIRE(json.starts_with(R"({"ok":true,"no_work":false,"claim_token":"14dc6e775153923eece9afe572bd8e5f","claim":{)"));
  REQUIRE(json.ends_with(R"(,"action_id":2})"
                         "\n"));

  atomic::terminal_result ended{.released = claim, .task_id = 2};
  auto const              terminal = render::terminal_json(ended, task);
  REQUIRE(terminal.starts_with(R"({"ok":true,"claim_token":"14dc6e775153923eece9afe572bd8e5f","claim":{)"));
  // NO `action_id` on a terminal payload — the pull envelope carries one
  // and the terminal envelope does not.
  REQUIRE(terminal.find("action_id") == std::string::npos);

  auto const aborted = render::abort_json(claim, 7);
  REQUIRE(aborted.ends_with(R"(,"aborting_session":7})"
                            "\n"));
}

TEST_CASE("reconcile's applied and dry-run payloads are different documents", "[agentrender]") {
  aa::reconcile_result      swept{.claims_marked_stale = 3, .actions_closed = 5};
  aa::reconcile_runs_result runs{.abandoned = 2};

  REQUIRE(render::reconcile_json(swept, runs, false) ==
          "{\"ok\":true,\"claims_marked_stale\":3,\"actions_closed\":5,\"runs_abandoned\":2}\n");
  REQUIRE(render::reconcile_text(swept, runs, false) == "reconciled: 3 claim(s) stale, 5 action(s) closed, 2 run(s) abandoned\n");

  // Dry-run: the counters are ZERO (nothing was written) while the
  // candidate arrays are populated. A reader conflating the two would
  // think the sweep had run.
  aa::reconcile_result      preview{.claims_marked_stale = 0, .actions_closed = 0, .candidates = {sample_claim()}};
  aa::reconcile_runs_result run_preview{
      .abandoned = 0, .candidates = {aa::run_candidate{.id = 9, .run_identifier = "run-1", .pid = 4242, .plan_id = 1}}};

  auto const json = render::reconcile_json(preview, run_preview, true);
  REQUIRE(json.starts_with("{\"ok\":true,\"claims_marked_stale\":0,\"actions_closed\":0,\"runs_abandoned\":0,\"candidates\":["));
  REQUIRE(json.find(R"({"kind":"task","id":2,"claim":{)") != std::string::npos);
  REQUIRE(json.ends_with(R"("run_candidates":[{"id":9,"run_identifier":"run-1","pid":4242,"plan_id":1}]})"
                         "\n"));

  REQUIRE(render::reconcile_text(preview, run_preview, true) == "dry-run: 1 claim candidate(s), 1 run candidate(s)\n"
                                                                "  task:2 token:14dc6e775153923eece9afe572bd8e5f\n"
                                                                "  run:9 pid:4242 identifier:run-1\n");
}

TEST_CASE("run_identifier is emitted UNESCAPED — a reproduced defect", "[agentrender]") {
  // The oracle's format string for this ONE field is a bare `"{s}"` while
  // every other string in the binary goes through the JSON escaper. An
  // identifier containing a quote therefore produces INVALID JSON.
  //
  // Reproduced under D2 rather than fixed, and pinned here so that fixing
  // it is a deliberate contract change with a failing test to point at
  // instead of a silent divergence from the reference binary.
  aa::reconcile_result      preview;
  aa::reconcile_runs_result runs{
      .candidates = {aa::run_candidate{.id = 1, .run_identifier = R"(a"b)", .pid = 1, .plan_id = std::nullopt}}};

  auto const json = render::reconcile_json(preview, runs, true);
  REQUIRE(json.find(R"("run_identifier":"a"b")") != std::string::npos);
  // Absent `plan_id` renders as 0, not null — also the oracle's `{d}`.
  REQUIRE(json.find(R"("plan_id":0)") != std::string::npos);
}

TEST_CASE("strings that need escaping are escaped everywhere else", "[agentrender]") {
  auto claim           = sample_claim();
  claim.purpose        = "quote\" backslash\\ newline\n tab\t";
  claim.release_reason = "ctrl\x01";
  std::string out;
  render::append_claim(out, claim);
  REQUIRE(out.find(R"("purpose":"quote\" backslash\\ newline\n tab\t")") != std::string::npos);
  // A control character with no short form becomes a `\uXXXX` escape rather
  // than being dropped — `planar.json_text`'s shared table (task 6114 swept
  // ten hand-rolled copies into it, three of which had silently lost the
  // `\b` and `\f` short forms).
  REQUIRE(out.find(R"("release_reason":"ctrl\u0001")") != std::string::npos);
}
