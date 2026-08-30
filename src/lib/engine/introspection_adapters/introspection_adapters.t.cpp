// @file introspection_adapters.t.cpp
// @brief Unit tests for `planar.engine.introspection_adapters` (plan 996,
// task 6102).
//
// Ported from the in-memory-collector subset of
// zig/src/engine/introspection_adapters.zig's colocated `test` blocks (the
// disk-discovery tests are out of scope here — see the module header).
// Fixtures for the "current vendor" cases are transcribed inline from
// zig/integration_tests/fixtures/introspection_transcripts/*.jsonl rather
// than read from disk, so these tests have no filesystem dependency and no
// working-directory assumption.
//
// What is pinned:
//
//   * A source past enabled/available contributes a coverage row and
//     nothing else — no signals, no scanned count.
//   * Every seeded PRIVATE_*_SENTINEL / secret / private-argument value is
//     asserted ABSENT from every signal's verb_path/first_seen/last_seen —
//     the redaction invariant is checked directly, not via a would-be JSON
//     encode (this module does not own JSON rendering; introspect.zig's
//     port does).
//   * `cli_log` is authoritative: a transcript-vendor signal that collides
//     on (verb_path, category, hour bucket) with a `cli_log` signal is
//     removed, never the other way around.
//   * `coverage.scanned == normalized + ignored + malformed + capped`
//     exactly, on every coverage row this file inspects.
//   * The evidence cap (`k_max_evidence_buckets`) is hard: signal count
//     never exceeds it, and the excess is counted `capped`, not silently
//     dropped or silently over-counted.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.engine.introspection_adapters;

namespace {

namespace ia = planar::engine::introspection_adapters;

auto has_warning(const std::vector<ia::warning_row>& warnings, ia::vendor v, ia::warning_kind kind) -> bool {
  for (auto const& w : warnings) {
    if (w.v == v && w.kind == kind) {
      return true;
    }
  }
  return false;
}

auto coverage_accounted(const ia::coverage_row& c) -> bool {
  return static_cast<std::uint64_t>(c.scanned) ==
         static_cast<std::uint64_t>(c.normalized) + static_cast<std::uint64_t>(c.ignored) +
             static_cast<std::uint64_t>(c.malformed) + static_cast<std::uint64_t>(c.capped);
}

/// @brief Whether `sentinel` appears in any signal's redacted string
/// fields. The direct analogue of the oracle's "encode the preview and
/// search the bytes" check, without a JSON encoder in this module.
auto preview_leaks(const ia::preview& p, std::string_view sentinel) -> bool {
  for (auto const& s : p.signals) {
    if (s.verb_path.find(sentinel) != std::string::npos || s.first_seen.find(sentinel) != std::string::npos ||
        s.last_seen.find(sentinel) != std::string::npos) {
      return true;
    }
  }
  return false;
}

} // namespace

TEST_CASE("raw vendor fixtures redact, aggregate, count malformed, and report coverage", "[engine][introspection_adapters]") {
  std::vector<ia::raw_source> sources{
      ia::raw_source{
          .v     = ia::vendor::claude,
          .jsonl = "{\"version\":1,\"type\":\"tool_result\",\"timestamp\":\"2026-07-12T12:05:00Z\","
                   "\"tool\":{\"name\":\"planar task add\",\"input\":{\"body\":\"private prose\",\"token\":\"sk-fixture\"}},"
                   "\"exit_code\":1,\"retry\":false,\"prompt\":\"secret prompt\"}\n"
                   "{\"version\":9,\"type\":\"tool_result\",\"prompt\":\"must not leak\"}",
      },
      ia::raw_source{
          .v     = ia::vendor::codex,
          .jsonl = "{\"schema_version\":1,\"event\":\"command_execution\",\"timestamp\":\"2026-07-12T13:01:00Z\","
                   "\"command_name\":\"planar plan show\",\"arguments\":[\"private-scope\",\"sk-secret\"],"
                   "\"exit_code\":1,\"retry_of_previous\":true,\"message\":\"prose\"}\n"
                   "not-json",
      },
      ia::raw_source{
          .v     = ia::vendor::copilot,
          .jsonl = "{\"version\":\"1\",\"kind\":\"shell_result\",\"time\":\"2026-07-12T14:01:00Z\","
                   "\"command\":{\"name\":\"planar sync pull\",\"args\":[\"--scope\",\"private\"]},"
                   "\"exit_code\":0,\"retry\":false,\"status\":\"abandoned\",\"transcript\":\"secret prose\"}",
      },
      ia::raw_source{.v = ia::vendor::cli_log, .available = false},
      ia::raw_source{.v = ia::vendor::cli_log, .enabled = false},
  };

  auto const preview = ia::collect_preview(sources);
  CHECK(preview.signals.size() == 3);
  CHECK(preview.signals.size() <= ia::k_max_evidence_buckets);
  CHECK(preview.coverage[0].malformed == 1);
  CHECK(preview.coverage[1].malformed == 1);
  CHECK(preview.coverage[3].state == ia::coverage_state::unavailable);
  CHECK(preview.coverage[4].state == ia::coverage_state::disabled);
  for (auto const& signal : preview.signals) {
    CHECK(signal.verb_path.size() <= 96);
  }
  CHECK_FALSE(preview_leaks(preview, "secret"));
  CHECK_FALSE(preview_leaks(preview, "private"));
}

TEST_CASE("deduplication is deterministic and cli log is authoritative", "[engine][introspection_adapters]") {
  std::string_view const raw =
      R"({"schema":1,"kind":"cli_invocation","recorded_at":"2026-07-12T12:02:00Z","verb_path":"planar task add","exit_code":1})";
  std::string const           transcript = "{\"version\":1,\"type\":\"tool_result\",\"timestamp\":\"2026-07-12T12:05:00Z\","
                                           "\"tool\":{\"name\":\"planar task add\",\"input\":{\"body\":\"secret\"}},\"exit_code\":1}\n"
                                           "{\"version\":1,\"type\":\"tool_result\",\"timestamp\":\"2026-07-12T12:06:00Z\","
                                           "\"tool\":{\"name\":\"planar task add\"},\"exit_code\":1}";
  std::vector<ia::raw_source> sources{
      ia::raw_source{.v = ia::vendor::claude, .jsonl = transcript},
      ia::raw_source{.v = ia::vendor::cli_log, .jsonl = std::string{raw}},
  };
  auto const preview = ia::collect_preview(sources);
  REQUIRE(preview.signals.size() == 1);
  CHECK(preview.signals[0].v == ia::vendor::cli_log);
  CHECK(preview.coverage[0].normalized == 2);
  CHECK(preview.coverage[0].ignored == 0);
  for (auto const& c : preview.coverage) {
    CHECK(coverage_accounted(c));
  }
}

TEST_CASE("ordinary success is observed without becoming gap while explicit usage evidence is gap",
          "[engine][introspection_adapters]") {
  std::string const           jsonl = "{\"schema\":1,\"kind\":\"cli_invocation\",\"recorded_at\":\"2026-07-12T12:01:00Z\","
                                      "\"verb_path\":\"planar task list\",\"exit_code\":0,\"error_category\":\"\"}\n"
                                      "{\"schema\":1,\"kind\":\"cli_invocation\",\"recorded_at\":\"2026-07-12T12:02:00Z\","
                                      "\"verb_path\":\"planar task add\",\"exit_code\":2,\"error_category\":\"usage\"}";
  std::vector<ia::raw_source> sources{ia::raw_source{.v = ia::vendor::cli_log, .jsonl = jsonl}};
  auto const                  preview = ia::collect_preview(sources);
  CHECK(preview.coverage[0].scanned == 2);
  CHECK(preview.coverage[0].normalized == 1);
  CHECK(preview.coverage[0].ignored == 1);
  CHECK(coverage_accounted(preview.coverage[0]));
  REQUIRE(preview.signals.size() == 1);
  CHECK(preview.signals[0].cat == ia::category::gap);
  CHECK(preview.signals[0].verb_path == "planar task add");
}

TEST_CASE("distinct evidence cap is explicit and counted", "[engine][introspection_adapters]") {
  std::string jsonl;
  for (std::size_t i = 0; i < ia::k_max_evidence_buckets + 1; ++i) {
    jsonl += std::format(
        R"({{"schema_version":1,"event":"command_execution","timestamp":"2026-07-12T12:00:00Z","command_name":"planar verb {}","exit_code":1}})"
        "\n",
        i);
  }
  std::vector<ia::raw_source> sources{ia::raw_source{.v = ia::vendor::codex, .jsonl = jsonl}};
  auto const                  preview = ia::collect_preview(sources);
  CHECK(preview.signals.size() == ia::k_max_evidence_buckets);
  CHECK(preview.coverage[0].capped == 1);
  CHECK(preview.coverage[0].normalized == ia::k_max_evidence_buckets);
  CHECK(preview.coverage[0].ignored == 0);
  CHECK(coverage_accounted(preview.coverage[0]));
  CHECK(has_warning(preview.warnings, ia::vendor::codex, ia::warning_kind::evidence_cap));
}

TEST_CASE("mixed coverage remains exactly accounted at the evidence cap boundary", "[engine][introspection_adapters]") {
  std::string jsonl;
  for (std::size_t i = 0; i < ia::k_max_evidence_buckets; ++i) {
    jsonl += std::format(
        R"({{"schema_version":1,"event":"command_execution","timestamp":"2026-07-12T12:00:00Z","command_name":"planar verb {}","exit_code":1}})"
        "\n",
        i);
  }
  jsonl += "{\"schema_version\":1,\"event\":\"command_execution\",\"timestamp\":\"2026-07-12T12:30:00Z\","
           "\"command_name\":\"planar verb 0\",\"exit_code\":1}\n"
           "{\"schema_version\":1,\"event\":\"command_execution\",\"timestamp\":\"2026-07-12T13:00:01Z\","
           "\"command_name\":\"planar ignored success\",\"exit_code\":0}\n"
           "not-json\n"
           "{\"schema_version\":1,\"event\":\"command_execution\",\"timestamp\":\"2026-07-12T13:00:02Z\","
           "\"command_name\":\"planar beyond cap\",\"exit_code\":1}";

  std::vector<ia::raw_source> sources{ia::raw_source{.v = ia::vendor::codex, .jsonl = jsonl}};
  auto const                  preview  = ia::collect_preview(sources);
  auto const&                 coverage = preview.coverage[0];

  CHECK(coverage.scanned == ia::k_max_evidence_buckets + 4);
  CHECK(coverage.normalized == ia::k_max_evidence_buckets + 1);
  CHECK(coverage.ignored == 1);
  CHECK(coverage.malformed == 1);
  CHECK(coverage.capped == 1);
  CHECK(coverage_accounted(coverage));
  REQUIRE(preview.signals.size() == ia::k_max_evidence_buckets);
  CHECK(preview.signals[0].verb_path == "planar verb 0");
  CHECK(preview.signals[0].count == 2);
  CHECK(has_warning(preview.warnings, ia::vendor::codex, ia::warning_kind::malformed));
  CHECK(has_warning(preview.warnings, ia::vendor::codex, ia::warning_kind::evidence_cap));
}

TEST_CASE("preview collector has no persistence dependency and discovery precedence is explicit",
          "[engine][introspection_adapters]") {
  CHECK(*ia::discover(true, "/override", "/builtin") == "/override");
  CHECK(*ia::discover(true, "", "/builtin") == "/builtin");
  CHECK_FALSE(ia::discover(false, "/override", "/builtin").has_value());
  auto const empty_preview = ia::collect_preview({});
  CHECK(empty_preview.signals.empty());
}

// ---------------------------------------------------------------------------
// "Current" vendor fixtures — transcribed from
// zig/integration_tests/fixtures/introspection_transcripts/*.jsonl.
// ---------------------------------------------------------------------------

constexpr std::string_view k_claude_fixture =
    R"({"parentUuid":null,"isSidechain":false,"userType":"external","cwd":"/private/raw/path/sentinel","sessionId":"fixture-claude-session","version":"2.1.209","gitBranch":"fixture","type":"assistant","message":{"id":"fixture-claude-message-1","type":"message","role":"assistant","model":"fixture-model","content":[{"type":"text","text":"PRIVATE_TRANSCRIPT_PROSE_SENTINEL"},{"type":"tool_use","id":"fixture-claude-tool-1","name":"Bash","input":{"command":"planar task show 4242 --scope PRIVATE_ARGUMENT_VALUE_SENTINEL","description":"PRIVATE_ENTITY_TEXT_SENTINEL"}}],"stop_reason":"tool_use","stop_sequence":null,"usage":{"input_tokens":1,"output_tokens":1}},"uuid":"fixture-claude-record-1","timestamp":"2026-07-12T12:00:00.000Z"}
{"parentUuid":"fixture-claude-record-1","isSidechain":false,"userType":"external","cwd":"/private/raw/path/sentinel","sessionId":"fixture-claude-session","version":"2.1.209","gitBranch":"fixture","type":"user","message":{"role":"user","content":[{"tool_use_id":"fixture-claude-tool-1","type":"tool_result","content":"PRIVATE_TRANSCRIPT_PROSE_SENTINEL PRIVATE_ARGUMENT_VALUE_SENTINEL PRIVATE_ENTITY_TEXT_SENTINEL /private/raw/path/sentinel","is_error":true}]},"uuid":"fixture-claude-record-2","timestamp":"2026-07-12T12:00:01.000Z"}
{"parentUuid":"fixture-claude-record-2","isSidechain":false,"userType":"external","cwd":"/private/raw/path/sentinel","sessionId":"fixture-claude-session","version":"2.1.209","gitBranch":"fixture","type":"user","message":{"role":"user","content":"PRIVATE_ENTITY_TEXT_SENTINEL PRIVATE_ARGUMENT_VALUE_SENTINEL"},"uuid":"fixture-claude-record-3","timestamp":"2026-07-12T12:00:02.000Z"}
{"parentUuid":"fixture-claude-record-3","isSidechain":false,"userType":"external","cwd":"/private/raw/path/sentinel","sessionId":"fixture-claude-session","version":"2.1.209","gitBranch":"fixture","type":"assistant","message":{"id":"fixture-claude-message-2","type":"message","role":"assistant","model":"fixture-model","content":[{"type":"text","text":"PRIVATE_TRANSCRIPT_PROSE_SENTINEL"}],"stop_reason":"end_turn","stop_sequence":null,"usage":{"input_tokens":1,"output_tokens":1}},"uuid":"fixture-claude-record-4","timestamp":"2026-07-12T12:00:03.000Z"}
{"parentUuid":"fixture-claude-record-4","isSidechain":false,"userType":"external","cwd":"/private/raw/path/sentinel","sessionId":"fixture-claude-session","version":"2.1.209","gitBranch":"fixture","type":"assistant","message":{"id":"fixture-claude-message-3","type":"message","role":"assistant","model":"fixture-model","content":[{"type":"tool_use","id":"fixture-claude-read-1","name":"Read","input":{"file_path":"/private/raw/path/sentinel"}}],"stop_reason":"tool_use","stop_sequence":null,"usage":{"input_tokens":1,"output_tokens":1}},"uuid":"fixture-claude-record-5","timestamp":"2026-07-12T12:00:04.000Z"}
{"parentUuid":"fixture-claude-record-5","isSidechain":false,"userType":"external","cwd":"/private/raw/path/sentinel","sessionId":"fixture-claude-session","version":"2.1.209","gitBranch":"fixture","type":"user","message":{"role":"user","content":[{"tool_use_id":"fixture-claude-read-1","type":"tool_result","content":"PRIVATE_TRANSCRIPT_PROSE_SENTINEL PRIVATE_ARGUMENT_VALUE_SENTINEL PRIVATE_ENTITY_TEXT_SENTINEL /private/raw/path/sentinel"}]},"uuid":"fixture-claude-record-6","timestamp":"2026-07-12T12:00:05.000Z"}
{"type":"future_record","timestamp":"2026-07-12T12:00:06.000Z","payload":{"prose":"PRIVATE_TRANSCRIPT_PROSE_SENTINEL","argument":"PRIVATE_ARGUMENT_VALUE_SENTINEL","entity":"PRIVATE_ENTITY_TEXT_SENTINEL","path":"/private/raw/path/sentinel"}}
{"parentUuid":"fixture-claude-record-6","isSidechain":false,"userType":"external","cwd":"/private/raw/path/sentinel","sessionId":"fixture-claude-session","version":"2.1.209","gitBranch":"fixture","type":"assistant","message":{"role":"assistant","content":{"type":"tool_use","id":"fixture-claude-malformed-tool","name":"Bash","input":"PRIVATE_ARGUMENT_VALUE_SENTINEL PRIVATE_ENTITY_TEXT_SENTINEL PRIVATE_TRANSCRIPT_PROSE_SENTINEL"}},"uuid":"fixture-claude-record-7","timestamp":"2026-07-12T12:00:07.000Z"})";

constexpr std::string_view k_codex_fixture =
    R"({"timestamp":"2026-07-12T13:00:00.000Z","type":"session_meta","payload":{"id":"fixture-codex-session","timestamp":"2026-07-12T13:00:00.000Z","cwd":"/private/raw/path/sentinel","originator":"codex_cli_rs","cli_version":"0.144.4","source":"cli","model_provider":"fixture","base_instructions":"PRIVATE_TRANSCRIPT_PROSE_SENTINEL","git":{"branch":"PRIVATE_ENTITY_TEXT_SENTINEL"}}}
{"timestamp":"2026-07-12T13:00:01.000Z","type":"response_item","payload":{"type":"function_call","name":"exec_command","arguments":"{\"cmd\":\"planar task show 4242 --scope PRIVATE_ARGUMENT_VALUE_SENTINEL\",\"workdir\":\"/private/raw/path/sentinel\",\"note\":\"PRIVATE_ENTITY_TEXT_SENTINEL PRIVATE_TRANSCRIPT_PROSE_SENTINEL\"}","call_id":"fixture-codex-call-1"}}
{"timestamp":"2026-07-12T13:00:02.000Z","type":"response_item","payload":{"type":"function_call_output","call_id":"fixture-codex-call-1","output":"PRIVATE_TRANSCRIPT_PROSE_SENTINEL PRIVATE_ARGUMENT_VALUE_SENTINEL PRIVATE_ENTITY_TEXT_SENTINEL /private/raw/path/sentinel process exited 1"}}
{"timestamp":"2026-07-12T13:00:03.000Z","type":"event_msg","payload":{"type":"user_message","message":"PRIVATE_ENTITY_TEXT_SENTINEL PRIVATE_ARGUMENT_VALUE_SENTINEL","images":[],"local_images":[],"text_elements":[]}}
{"timestamp":"2026-07-12T13:00:04.000Z","type":"event_msg","payload":{"type":"agent_message","message":"PRIVATE_TRANSCRIPT_PROSE_SENTINEL","phase":"final_answer"}}
{"timestamp":"2026-07-12T13:00:05.000Z","type":"future_rollout_item","payload":{"prose":"PRIVATE_TRANSCRIPT_PROSE_SENTINEL","argument":"PRIVATE_ARGUMENT_VALUE_SENTINEL","entity":"PRIVATE_ENTITY_TEXT_SENTINEL","path":"/private/raw/path/sentinel"}}
{"timestamp":"2026-07-12T13:00:06.000Z","type":"response_item","payload":{"type":"function_call","name":7,"arguments":["PRIVATE_ARGUMENT_VALUE_SENTINEL","PRIVATE_ENTITY_TEXT_SENTINEL","PRIVATE_TRANSCRIPT_PROSE_SENTINEL","/private/raw/path/sentinel"],"call_id":null}})";

constexpr std::string_view k_copilot_fixture =
    R"({"type":"session.start","data":{"sessionId":"fixture-copilot-session","cwd":"/private/raw/path/sentinel","repository":"PRIVATE_ENTITY_TEXT_SENTINEL"},"id":"fixture-copilot-event-1","timestamp":"2026-07-12T14:00:00.000Z","parentId":null}
{"type":"tool.execution_start","data":{"toolCallId":"fixture-copilot-tool-1","toolName":"shell","arguments":{"command":"planar task show 4242 --scope PRIVATE_ARGUMENT_VALUE_SENTINEL","cwd":"/private/raw/path/sentinel","description":"PRIVATE_ENTITY_TEXT_SENTINEL PRIVATE_TRANSCRIPT_PROSE_SENTINEL"}},"id":"fixture-copilot-event-2","timestamp":"2026-07-12T14:00:01.000Z","parentId":"fixture-copilot-event-1"}
{"type":"tool.execution_complete","data":{"toolCallId":"fixture-copilot-tool-1","model":"fixture-model","success":false,"result":{"content":"PRIVATE_TRANSCRIPT_PROSE_SENTINEL PRIVATE_ARGUMENT_VALUE_SENTINEL PRIVATE_ENTITY_TEXT_SENTINEL /private/raw/path/sentinel","exitCode":1}},"id":"fixture-copilot-event-3","timestamp":"2026-07-12T14:00:02.000Z","parentId":"fixture-copilot-event-2"}
{"type":"user.message","data":{"content":"PRIVATE_ENTITY_TEXT_SENTINEL PRIVATE_ARGUMENT_VALUE_SENTINEL","transformedContent":"PRIVATE_TRANSCRIPT_PROSE_SENTINEL /private/raw/path/sentinel"},"id":"fixture-copilot-event-4","timestamp":"2026-07-12T14:00:03.000Z","parentId":null}
{"type":"assistant.message","data":{"content":"PRIVATE_TRANSCRIPT_PROSE_SENTINEL","references":[{"path":"/private/raw/path/sentinel","label":"PRIVATE_ENTITY_TEXT_SENTINEL"}]},"id":"fixture-copilot-event-5","timestamp":"2026-07-12T14:00:04.000Z","parentId":null}
{"type":"future.event","data":{"prose":"PRIVATE_TRANSCRIPT_PROSE_SENTINEL","argument":"PRIVATE_ARGUMENT_VALUE_SENTINEL","entity":"PRIVATE_ENTITY_TEXT_SENTINEL","path":"/private/raw/path/sentinel"},"id":"fixture-copilot-event-6","timestamp":"2026-07-12T14:00:05.000Z","parentId":null}
{"type":"tool.execution_complete","data":{"success":"PRIVATE_TRANSCRIPT_PROSE_SENTINEL","result":["PRIVATE_ARGUMENT_VALUE_SENTINEL","PRIVATE_ENTITY_TEXT_SENTINEL","/private/raw/path/sentinel"]},"id":"fixture-copilot-event-7","timestamp":"2026-07-12T14:00:06.000Z","parentId":null})";

TEST_CASE("current Claude fixture pairs tool use and result without retaining private fields",
          "[engine][introspection_adapters]") {
  std::vector<ia::raw_source> sources{ia::raw_source{.v = ia::vendor::claude, .jsonl = std::string{k_claude_fixture}}};
  auto const                  preview = ia::collect_preview(sources);

  CHECK(preview.coverage[0].scanned == 8);
  CHECK(preview.coverage[0].normalized == 1);
  CHECK(preview.coverage[0].ignored == 6);
  CHECK(preview.coverage[0].malformed == 1);
  CHECK(coverage_accounted(preview.coverage[0]));
  REQUIRE(preview.signals.size() == 1);
  CHECK(preview.signals[0].v == ia::vendor::claude);
  CHECK(preview.signals[0].cat == ia::category::failure);
  CHECK(preview.signals[0].verb_path == "planar task show");
  CHECK(preview.signals[0].first_seen == "2026-07-12T12:00:01.000Z");

  for (auto const* sentinel : {"PRIVATE_TRANSCRIPT_PROSE_SENTINEL", "PRIVATE_ARGUMENT_VALUE_SENTINEL",
                               "PRIVATE_ENTITY_TEXT_SENTINEL", "/private/raw/path/sentinel"}) {
    CHECK_FALSE(preview_leaks(preview, sentinel));
  }
}

TEST_CASE("current Claude ordinary conversation and unknown records are ignored", "[engine][introspection_adapters]") {
  std::string const jsonl =
      "{\"type\":\"user\",\"message\":{\"role\":\"user\",\"content\":\"private user "
      "prose\"},\"timestamp\":\"2026-07-12T12:00:00Z\"}\n"
      "{\"type\":\"assistant\",\"message\":{\"role\":\"assistant\",\"content\":[{\"type\":\"text\",\"text\":\"private assistant "
      "prose\"}]},\"timestamp\":\"2026-07-12T12:00:01Z\"}\n"
      "{\"type\":\"assistant\",\"message\":{\"role\":\"assistant\",\"content\":[{\"type\":\"tool_use\",\"id\":\"read-1\","
      "\"name\":\"Read\",\"input\":{\"file_path\":\"/private/path\"}}]},\"timestamp\":\"2026-07-12T12:00:02Z\"}\n"
      "{\"type\":\"future_record\",\"payload\":{\"private\":\"content\"},\"timestamp\":\"2026-07-12T12:00:03Z\"}";
  std::vector<ia::raw_source> sources{ia::raw_source{.v = ia::vendor::claude, .jsonl = jsonl}};
  auto const                  preview = ia::collect_preview(sources);

  CHECK(preview.coverage[0].scanned == 4);
  CHECK(preview.coverage[0].normalized == 0);
  CHECK(preview.coverage[0].ignored == 4);
  CHECK(preview.coverage[0].malformed == 0);
  CHECK(coverage_accounted(preview.coverage[0]));
  CHECK(preview.signals.empty());
}

TEST_CASE("current Claude optional is_error defaults false but rejects a present non-boolean",
          "[engine][introspection_adapters]") {
  std::string const jsonl =
      "{\"type\":\"assistant\",\"message\":{\"role\":\"assistant\",\"content\":[{\"type\":\"tool_use\",\"id\":\"read-success\","
      "\"name\":\"Read\",\"input\":{\"file_path\":\"/private/raw/path/sentinel\"}}]},\"timestamp\":\"2026-07-12T12:00:00Z\"}\n"
      "{\"type\":\"user\",\"message\":{\"role\":\"user\",\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"read-success\","
      "\"content\":\"PRIVATE_TRANSCRIPT_PROSE_SENTINEL\"}]},\"timestamp\":\"2026-07-12T12:00:01Z\"}\n"
      "{\"type\":\"user\",\"message\":{\"role\":\"user\",\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"unknown\",\"is_"
      "error\":\"false\"}]},\"timestamp\":\"2026-07-12T12:00:02Z\"}";
  std::vector<ia::raw_source> sources{ia::raw_source{.v = ia::vendor::claude, .jsonl = jsonl}};
  auto const                  preview = ia::collect_preview(sources);

  CHECK(preview.coverage[0].scanned == 3);
  CHECK(preview.coverage[0].normalized == 0);
  CHECK(preview.coverage[0].ignored == 2);
  CHECK(preview.coverage[0].malformed == 1);
  CHECK(preview.signals.empty());
  CHECK(has_warning(preview.warnings, ia::vendor::claude, ia::warning_kind::malformed));
  for (auto const* sentinel : {"PRIVATE_TRANSCRIPT_PROSE_SENTINEL", "/private/raw/path/sentinel"}) {
    CHECK_FALSE(preview_leaks(preview, sentinel));
  }
}

TEST_CASE("current Claude pairing ignores unmatched out-of-order and duplicate results", "[engine][introspection_adapters]") {
  std::string const jsonl =
      "{\"type\":\"user\",\"message\":{\"role\":\"user\",\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"call-1\",\"is_"
      "error\":true}]},\"timestamp\":\"2026-07-12T12:00:00Z\"}\n"
      "{\"type\":\"assistant\",\"message\":{\"role\":\"assistant\",\"content\":[{\"type\":\"tool_use\",\"id\":\"call-1\","
      "\"name\":\"Bash\",\"input\":{\"command\":\"planar plan show 42\"}}]},\"timestamp\":\"2026-07-12T12:00:01Z\"}\n"
      "{\"type\":\"assistant\",\"message\":{\"role\":\"assistant\",\"content\":[{\"type\":\"tool_use\",\"id\":\"call-1\","
      "\"name\":\"Bash\",\"input\":{\"command\":\"planar task show 77\"}}]},\"timestamp\":\"2026-07-12T12:00:02Z\"}\n"
      "{\"type\":\"user\",\"message\":{\"role\":\"user\",\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"call-1\",\"is_"
      "error\":true}]},\"timestamp\":\"2026-07-12T12:00:03Z\"}\n"
      "{\"type\":\"user\",\"message\":{\"role\":\"user\",\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"call-1\",\"is_"
      "error\":true}]},\"timestamp\":\"2026-07-12T12:00:04Z\"}\n"
      "{\"type\":\"assistant\",\"message\":{\"role\":\"assistant\",\"content\":[{\"type\":\"tool_use\",\"id\":\"call-2\","
      "\"name\":\"Bash\",\"input\":{\"command\":\"planar task show 99\"}}]},\"timestamp\":\"2026-07-12T12:00:05Z\"}";
  std::vector<ia::raw_source> sources{ia::raw_source{.v = ia::vendor::claude, .jsonl = jsonl}};
  auto const                  preview = ia::collect_preview(sources);

  CHECK(preview.coverage[0].scanned == 6);
  CHECK(preview.coverage[0].normalized == 1);
  CHECK(preview.coverage[0].malformed == 0);
  REQUIRE(preview.signals.size() == 1);
  CHECK(preview.signals[0].verb_path == "planar plan show");
}

TEST_CASE("current Claude rejects globally known tokens that are not a valid verb path", "[engine][introspection_adapters]") {
  std::string const jsonl =
      "{\"type\":\"assistant\",\"message\":{\"role\":\"assistant\",\"content\":[{\"type\":\"tool_use\",\"id\":\"call-invalid-"
      "path\",\"name\":\"Bash\",\"input\":{\"command\":\"planar task version "
      "PRIVATE_ARGUMENT_VALUE_SENTINEL\"}}]},\"timestamp\":\"2026-07-12T12:00:00Z\"}\n"
      "{\"type\":\"user\",\"message\":{\"role\":\"user\",\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"call-invalid-"
      "path\",\"is_error\":true,\"content\":\"PRIVATE_TRANSCRIPT_PROSE_SENTINEL\"}]},\"timestamp\":\"2026-07-12T12:00:01Z\"}";
  std::vector<ia::raw_source> sources{ia::raw_source{.v = ia::vendor::claude, .jsonl = jsonl}};
  auto const                  preview = ia::collect_preview(sources);

  CHECK(preview.coverage[0].scanned == 2);
  CHECK(preview.coverage[0].normalized == 0);
  CHECK(preview.coverage[0].malformed == 0);
  CHECK(preview.signals.empty());
}

TEST_CASE("current Claude pairing state is isolated per raw source", "[engine][introspection_adapters]") {
  std::string_view const tool_use =
      "{\"type\":\"assistant\",\"message\":{\"role\":\"assistant\",\"content\":[{\"type\":\"tool_use\",\"id\":\"shared-call-id\","
      "\"name\":\"Bash\",\"input\":{\"command\":\"planar task show 42\"}}]},\"timestamp\":\"2026-07-12T12:00:00Z\"}";
  std::string_view const tool_result =
      "{\"type\":\"user\",\"message\":{\"role\":\"user\",\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"shared-call-"
      "id\",\"is_error\":true}]},\"timestamp\":\"2026-07-12T12:00:01Z\"}";
  std::vector<ia::raw_source> sources{
      ia::raw_source{.v = ia::vendor::claude, .jsonl = std::string{tool_use}},
      ia::raw_source{.v = ia::vendor::claude, .jsonl = std::string{tool_result}},
  };
  auto const preview = ia::collect_preview(sources);

  CHECK(preview.signals.empty());
  REQUIRE(preview.coverage.size() == 2);
  for (auto const& c : preview.coverage) {
    CHECK(c.scanned == 1);
    CHECK(c.normalized == 0);
    CHECK(c.malformed == 0);
  }
}

TEST_CASE("malformed recognized envelope degrades only its adapter", "[engine][introspection_adapters]") {
  std::string_view const claude =
      "{\"type\":\"assistant\",\"message\":{\"role\":\"assistant\",\"content\":{\"type\":\"tool_use\",\"input\":\"PRIVATE_"
      "TRANSCRIPT_PROSE_SENTINEL\"}},\"timestamp\":\"2026-07-12T12:00:00Z\"}";
  std::string_view const codex = "{\"schema_version\":1,\"event\":\"command_execution\",\"timestamp\":\"2026-07-12T12:00:01Z\","
                                 "\"command_name\":\"planar plan show\",\"exit_code\":1}";
  std::vector<ia::raw_source> sources{
      ia::raw_source{.v = ia::vendor::claude, .jsonl = std::string{claude}},
      ia::raw_source{.v = ia::vendor::codex, .jsonl = std::string{codex}},
  };
  auto const preview = ia::collect_preview(sources);

  REQUIRE(preview.signals.size() == 1);
  CHECK(preview.signals[0].v == ia::vendor::codex);
  CHECK(preview.signals[0].verb_path == "planar plan show");
  CHECK(has_warning(preview.warnings, ia::vendor::claude, ia::warning_kind::malformed));
  CHECK_FALSE(has_warning(preview.warnings, ia::vendor::codex, ia::warning_kind::malformed));
  CHECK_FALSE(preview_leaks(preview, "PRIVATE_TRANSCRIPT_PROSE_SENTINEL"));
}

TEST_CASE("current vendor fixture union distinguishes irrelevant and malformed envelopes", "[engine][introspection_adapters]") {
  std::vector<ia::raw_source> sources{
      ia::raw_source{.v = ia::vendor::codex, .jsonl = std::string{k_codex_fixture}},
      ia::raw_source{.v = ia::vendor::copilot, .jsonl = std::string{k_copilot_fixture}},
  };
  auto const preview = ia::collect_preview(sources);

  CHECK(preview.signals.empty());
  REQUIRE(preview.coverage.size() == 2);
  for (auto const& c : preview.coverage) {
    CHECK(c.scanned == 7);
    CHECK(c.normalized == 0);
    CHECK(c.ignored == 6);
    CHECK(c.malformed == 1);
    CHECK(coverage_accounted(c));
  }

  for (auto const* sentinel : {"PRIVATE_TRANSCRIPT_PROSE_SENTINEL", "PRIVATE_ARGUMENT_VALUE_SENTINEL",
                               "PRIVATE_ENTITY_TEXT_SENTINEL", "/private/raw/path/sentinel"}) {
    CHECK_FALSE(preview_leaks(preview, sentinel));
  }
}
