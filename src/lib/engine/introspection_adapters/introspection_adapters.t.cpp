// @file introspection_adapters.t.cpp
// @brief Unit tests for `planar.engine.introspection_adapters` (plan 996,
// tasks 6102 and 6352).
//
// Ported from zig/src/engine/introspection_adapters.zig's colocated `test`
// blocks. The in-memory-collector cases (task 6102) transcribe their
// fixtures inline from
// zig/integration_tests/fixtures/introspection_transcripts/*.jsonl rather
// than reading from disk, so THOSE tests have no filesystem dependency and
// no working-directory assumption. The discovery-half cases (task 6352,
// below the `--- discovery half ---` marker) necessarily DO touch a real
// filesystem — that is the surface under test — but every path they touch
// is constructed under `std::filesystem::temp_directory_path()` with a
// clock-keyed scratch directory removed on scope exit (`scratch_dir`,
// the same pattern `planar.engine.workbench.fsutil.t.cpp` uses). None of
// them read `$HOME` or any other environment variable.
//
// What is pinned:
//
//   * A source past enabled/available contributes a coverage row and
//     nothing else — no signals, no scanned count.
//   * Every seeded PRIVATE_*_SENTINEL / secret / private-argument value is
//     checked absent from every signal's verb_path/first_seen/last_seen via
//     `preview_leaks` — the redaction invariant is checked directly, not
//     via a would-be JSON encode (this module does not own JSON rendering;
//     introspect.zig's port does). `preview_leaks` only DISCRIMINATES at
//     call sites where `preview.signals` is non-empty (the "current Claude
//     fixture pairs tool use..." and "malformed recognized envelope..."
//     cases); at the several sites where the same fixtures also drive
//     `signals.empty()` to true (e.g. the "current vendor fixture union"
//     case), the leak-check is vacuously satisfied and documents intent
//     rather than proving it.
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

TEST_CASE("coverage rows for one vendor keep INPUT order — the sort is stable", "[engine][introspection_adapters][6348]") {
  // TASK 6348. `coverage_less_than` compares the VENDOR AND NOTHING ELSE, so
  // every coverage row for one vendor is a tie and only `stable_sort`'s
  // guarantee decides their order. The module comment recorded that this was
  // a fidelity call with no test behind it: the existing two-`cli_log`-row
  // case pins one tie's OUTCOME, which a lucky unstable sort would also
  // satisfy.
  //
  // SIXTY-FOUR rows, not two -- and not twenty either. An unstable introsort
  // falls back to INSERTION SORT below an implementation threshold (~30 in
  // libc++), and insertion sort is stable by accident, so a small fixture
  // cannot tell `sort` from `stable_sort` at all. Measured: a first cut of
  // this case used twenty rows and the `stable_sort` -> `sort` probe SURVIVED.
  // Sixty-four is comfortably past the threshold on both libc++ and
  // libstdc++.
  // TWO vendors, INTERLEAVED. An all-ties range is degenerate for introsort --
  // it partitions to nothing and preserves order whatever the algorithm --
  // so a single-vendor fixture cannot tell `sort` from `stable_sort` at any
  // size. Measured: single-vendor cuts at n=20 AND n=64 both SURVIVED the
  // probe. Interleaving forces a real partition, and the ties then sit
  // inside it where an unstable sort actually disturbs them.
  std::vector<ia::raw_source> sources;
  sources.reserve(64);
  for (std::uint32_t i = 0; i < 64; ++i) {
    std::string jsonl;
    for (std::uint32_t line = 0; line <= i; ++line) {
      jsonl += "{\"version\":1,\"type\":\"tool_result\",\"timestamp\":\"2026-07-12T12:05:00Z\","
               "\"tool\":{\"name\":\"planar task add\",\"input\":{}},\"exit_code\":0,\"retry\":false}\n";
    }
    // `codex` sorts AFTER `claude`, so the odd rows must all migrate past the
    // even ones -- real work for the sort -- while each vendor's own rows
    // keep their relative input order.
    auto const v = (i % 2 == 0) ? ia::vendor::claude : ia::vendor::codex;
    sources.push_back(ia::raw_source{.v = v, .enabled = true, .available = true, .jsonl = std::move(jsonl)});
  }

  auto const got = ia::collect_preview(sources);
  REQUIRE(got.coverage.size() == 64);

  // Grouped by vendor, and WITHIN each group the `scanned` counts must still
  // ascend exactly as they were supplied: claude saw 1, 3, 5, … and codex
  // saw 2, 4, 6, …
  std::vector<std::uint32_t> claude_scanned;
  std::vector<std::uint32_t> codex_scanned;
  for (auto const& row : got.coverage) {
    (row.v == ia::vendor::claude ? claude_scanned : codex_scanned).push_back(row.scanned);
  }
  REQUIRE(claude_scanned.size() == 32);
  REQUIRE(codex_scanned.size() == 32);
  CHECK(std::ranges::is_sorted(claude_scanned));
  CHECK(std::ranges::is_sorted(codex_scanned));

  // And the grouping itself: every claude row precedes every codex row.
  auto const first_codex = std::ranges::find(got.coverage, ia::vendor::codex, &ia::coverage_row::v);
  CHECK(std::ranges::none_of(std::ranges::subrange(first_codex, got.coverage.end()),
                             [](auto const& row) { return row.v == ia::vendor::claude; }));
}

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

TEST_CASE("abandonment takes precedence over gap when both are present in the same legacy record",
          "[engine][introspection_adapters]") {
  // category_from_evidence checks abandoned BEFORE gap (zig:701-703): a
  // copilot legacy record with status:"abandoned" AND invalid_flag true
  // sets both booleans, and the oracle's precedence must win as
  // `abandonment`, never `gap`. Swapping the two checks leaves every other
  // fixture in this file passing, so this is the one case that pins the
  // order rather than just the individual outcomes.
  std::string const           jsonl = R"({"version":"1","kind":"shell_result","time":"2026-07-12T15:00:00Z",)"
                                      R"("command":{"name":"planar task add"},"exit_code":0,"retry":false,)"
                                      R"("status":"abandoned","invalid_flag":true})";
  std::vector<ia::raw_source> sources{ia::raw_source{.v = ia::vendor::copilot, .jsonl = jsonl}};
  auto const                  preview = ia::collect_preview(sources);
  REQUIRE(preview.signals.size() == 1);
  CHECK(preview.signals[0].cat == ia::category::abandonment);
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

// ============================================================================
// --- discovery half --- (task 6352): collect_preview_from_paths,
// collect_vendor_path, the fault-injection seam, CliLogAdapter.
// ============================================================================

namespace {

/// @brief A scratch directory removed on scope exit. Same pattern as
/// `planar.engine.workbench.fsutil.t.cpp`'s `scratch_dir`.
struct scratch_dir {
  std::filesystem::path path_;

  scratch_dir()
      : path_(std::filesystem::temp_directory_path() / std::format("planar_ia_discovery_{}_{}",
                                                                   std::chrono::steady_clock::now().time_since_epoch().count(),
                                                                   reinterpret_cast<std::uintptr_t>(this))) {
    std::error_code ec;
    std::filesystem::create_directories(path_, ec);
  }
  scratch_dir(const scratch_dir&)            = delete;
  scratch_dir& operator=(const scratch_dir&) = delete;
  ~scratch_dir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
};

auto write(const std::filesystem::path& path, std::string_view content) -> void {
  if (path.has_parent_path()) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
  }
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file << content;
}

auto coverage_for(const ia::preview& p, ia::vendor v) -> const ia::coverage_row* {
  for (auto const& c : p.coverage) {
    if (c.v == v) {
      return &c;
    }
  }
  return nullptr;
}

} // namespace

TEST_CASE("collect_preview_from_paths: a disabled vendor reports coverage disabled, no filesystem touch",
          "[engine][introspection_adapters][discovery]") {
  scratch_dir                 scratch;
  ia::transcript_config const config{.home_dir = scratch.path_.string(), .claude_enabled = false};
  auto const                  preview = ia::collect_preview_from_paths(config, std::nullopt);

  auto const* claude = coverage_for(preview, ia::vendor::claude);
  REQUIRE(claude != nullptr);
  CHECK(claude->state == ia::coverage_state::disabled);
  CHECK(claude->scanned == 0);
  CHECK(has_warning(preview.warnings, ia::vendor::claude, ia::warning_kind::disabled));
}

TEST_CASE("collect_preview_from_paths: a missing built-in directory reports unavailable, not a crash",
          "[engine][introspection_adapters][discovery]") {
  // No .claude/projects under this scratch home at all — the built-in the
  // oracle's own `report --json` sees on a fresh scratch $HOME (verified
  // against the built zig oracle in a pinned arena).
  scratch_dir                 scratch;
  ia::transcript_config const config{.home_dir = scratch.path_.string()};
  auto const                  preview = ia::collect_preview_from_paths(config, std::nullopt);

  for (auto const v : {ia::vendor::claude, ia::vendor::codex, ia::vendor::copilot}) {
    auto const* cov = coverage_for(preview, v);
    REQUIRE(cov != nullptr);
    CHECK(cov->state == ia::coverage_state::unavailable);
    CHECK(cov->scanned == 0);
  }
  // No CLI adapter supplied at all -> cli_log reports unavailable too,
  // mirroring collectPreviewFromPathsWithFs's `else` arm (zig:275).
  auto const* cli_log = coverage_for(preview, ia::vendor::cli_log);
  REQUIRE(cli_log != nullptr);
  CHECK(cli_log->state == ia::coverage_state::unavailable);
}

TEST_CASE("collect_preview_from_paths: a real directory of transcripts is scanned and normalized",
          "[engine][introspection_adapters][discovery]") {
  scratch_dir scratch;
  auto const  claude_dir = scratch.path_ / ".claude" / "projects";
  write(claude_dir / "session1.jsonl",
        "{\"schema\":1,\"kind\":\"cli_invocation\",\"verb_path\":\"planar task show\",\"exit_code\":1,"
        "\"error_category\":\"not_found\",\"recorded_at\":\"2026-07-12T12:00:01Z\"}\n"
        "not json at all\n");
  // A non-.jsonl file must be SKIPPED for claude (jsonl_only == true).
  write(claude_dir / "notes.txt", "irrelevant");

  ia::transcript_config const config{.home_dir = scratch.path_.string()};
  auto const                  preview = ia::collect_preview_from_paths(config, std::nullopt);

  auto const* claude = coverage_for(preview, ia::vendor::claude);
  REQUIRE(claude != nullptr);
  CHECK(claude->state == ia::coverage_state::observed);
  // Two lines scanned: the malformed non-json line is counted, and the
  // legacy cli_invocation-shaped record parses as JSON but is not a
  // recognized Claude envelope (no "version"/"type": tool_result/
  // assistant/user) — malformed too, under extract_claude's own rules.
  CHECK(claude->scanned == 2);
  CHECK(coverage_accounted(*claude));
}

TEST_CASE("collect_preview_from_paths: an override directory with the documented /**/*.jsonl suffix resolves to its parent",
          "[engine][introspection_adapters][discovery]") {
  scratch_dir scratch;
  auto const  override_dir = scratch.path_ / "custom_claude_logs";
  write(override_dir / "a.jsonl", "not json\n");

  ia::transcript_config const config{.home_dir    = scratch.path_.string(),
                                     .claude_path = (override_dir / "**" / "*.jsonl").string()};
  auto const                  preview = ia::collect_preview_from_paths(config, std::nullopt);

  auto const* claude = coverage_for(preview, ia::vendor::claude);
  REQUIRE(claude != nullptr);
  CHECK(claude->state == ia::coverage_state::observed);
  CHECK(claude->scanned == 1);
  CHECK(claude->malformed == 1);
}

TEST_CASE("collect_preview_from_paths: copilot is NOT jsonl_only — a non-.jsonl file still counts",
          "[engine][introspection_adapters][discovery]") {
  scratch_dir scratch;
  auto const  copilot_dir = scratch.path_ / ".copilot" / "session-state";
  write(copilot_dir / "session.log", "not json\n");

  ia::transcript_config const config{.home_dir = scratch.path_.string()};
  auto const                  preview = ia::collect_preview_from_paths(config, std::nullopt);

  auto const* copilot = coverage_for(preview, ia::vendor::copilot);
  REQUIRE(copilot != nullptr);
  CHECK(copilot->state == ia::coverage_state::observed);
  CHECK(copilot->scanned == 1);
}

TEST_CASE("collect_preview_from_paths: fault injection on selected_stat marks the vendor unavailable with no warning",
          "[engine][introspection_adapters][discovery]") {
  scratch_dir scratch;
  write(scratch.path_ / ".claude" / "projects" / "a.jsonl", "not json\n");

  ia::transcript_config const config{.home_dir = scratch.path_.string()};
  ia::fs_fault const          fault = [](ia::vendor v, ia::fs_operation op, std::string_view) {
    return v == ia::vendor::claude && op == ia::fs_operation::selected_stat;
  };
  auto const preview = ia::collect_preview_from_paths(config, std::nullopt, {}, fault);

  auto const* claude = coverage_for(preview, ia::vendor::claude);
  REQUIRE(claude != nullptr);
  CHECK(claude->state == ia::coverage_state::unavailable);
  CHECK(claude->scanned == 0);
  // `collect_preview`'s OWN post-processing loop (unchanged since task
  // 6102) raises exactly one `unavailable` warning for ANY unavailable
  // coverage row, regardless of what marked it that way — so this DOES
  // carry a warning, unlike `collect_vendor_path`'s own early-return arm
  // (mirroring the oracle's `stat := ... catch { return owned.append(...)
  // }`, zig:322-324) which pushes NO warning of its own. The net count is
  // what distinguishes the two paths: exactly one warning here, versus two
  // (one from `collect_vendor_path`'s explicit push, one from
  // `collect_preview`'s generic loop) for `directory_open`/
  // `directory_walk` below — matches the oracle's own `report --json`
  // against a scratch $HOME with NO `.claude/projects` at all, which
  // reports exactly one `{"vendor":"claude","kind":"unavailable",
  // "count":1}`, not two.
  auto const unavailable_count = std::ranges::count_if(preview.warnings, [](ia::warning_row const& w) {
    return w.v == ia::vendor::claude && w.kind == ia::warning_kind::unavailable;
  });
  CHECK(unavailable_count == 1);

  // A fault targeting a DIFFERENT vendor leaves this one untouched — the
  // seam discriminates by vendor, not just by operation.
  CHECK(coverage_for(preview, ia::vendor::codex)->state == ia::coverage_state::unavailable);
}

TEST_CASE("collect_preview_from_paths: fault injection on directory_walk raises a warning and marks unavailable",
          "[engine][introspection_adapters][discovery]") {
  scratch_dir scratch;
  std::filesystem::create_directories(scratch.path_ / ".claude" / "projects");
  write(scratch.path_ / ".claude" / "projects" / "a.jsonl", "not json\n");

  ia::transcript_config const config{.home_dir = scratch.path_.string()};
  ia::fs_fault const          fault = [](ia::vendor v, ia::fs_operation op, std::string_view) {
    return v == ia::vendor::claude && op == ia::fs_operation::directory_walk;
  };
  auto const preview = ia::collect_preview_from_paths(config, std::nullopt, {}, fault);

  auto const* claude = coverage_for(preview, ia::vendor::claude);
  REQUIRE(claude != nullptr);
  CHECK(claude->state == ia::coverage_state::unavailable);
  // TWO `unavailable` warnings this time — `collect_vendor_path`'s own
  // explicit push (zig:337-339's `try warnings.append(...)` before the
  // early return) PLUS `collect_preview`'s generic one, unlike
  // `selected_stat` above which contributes only the latter. See that
  // test's comment for the full account.
  auto const unavailable_count = std::ranges::count_if(preview.warnings, [](ia::warning_row const& w) {
    return w.v == ia::vendor::claude && w.kind == ia::warning_kind::unavailable;
  });
  CHECK(unavailable_count == 2);
}

TEST_CASE("collect_preview_from_paths: the file cap stops scanning and raises file_cap, not a crash",
          "[engine][introspection_adapters][discovery]") {
  scratch_dir scratch;
  write(scratch.path_ / ".claude" / "projects" / "a.jsonl", "not json\n");
  write(scratch.path_ / ".claude" / "projects" / "b.jsonl", "also not json\n");

  ia::transcript_config const config{.home_dir = scratch.path_.string()};
  ia::collector_limits const  limits{.max_files = 1};
  auto const                  preview = ia::collect_preview_from_paths(config, std::nullopt, limits);

  auto const* claude = coverage_for(preview, ia::vendor::claude);
  REQUIRE(claude != nullptr);
  // Exactly one of the two files was scanned before the cap fired.
  CHECK(claude->scanned == 1);
  CHECK(has_warning(preview.warnings, ia::vendor::claude, ia::warning_kind::file_cap));
}

TEST_CASE("collect_preview_from_paths: the byte cap stops scanning and raises byte_cap, not a crash",
          "[engine][introspection_adapters][discovery][6352][iter2]") {
  // Reviewer finding (iteration 2): no test exercised `byte_cap` at all —
  // a break-probe deleting the whole `if (size > bytes_left) { byte_cap;
  // break; }` block SURVIVED every existing test. `a.jsonl` and `b.jsonl`
  // are each exactly 10 bytes; a 10-byte budget lets `a.jsonl` (sorted
  // first) exactly exhaust it, so `b.jsonl`'s stat sees `bytes_left == 0`
  // and trips the cap instead of being scanned.
  scratch_dir scratch;
  write(scratch.path_ / ".claude" / "projects" / "a.jsonl", "1234567890");
  write(scratch.path_ / ".claude" / "projects" / "b.jsonl", "1234567890");

  ia::transcript_config const config{.home_dir = scratch.path_.string()};
  ia::collector_limits const  limits{.max_bytes = 10};
  auto const                  preview = ia::collect_preview_from_paths(config, std::nullopt, limits);

  auto const* claude = coverage_for(preview, ia::vendor::claude);
  REQUIRE(claude != nullptr);
  CHECK(claude->scanned == 1);
  CHECK(has_warning(preview.warnings, ia::vendor::claude, ia::warning_kind::byte_cap));
}

TEST_CASE("collect_preview_from_paths: the record cap stops scanning and raises record_cap, not a crash",
          "[engine][introspection_adapters][discovery][6352][iter2]") {
  // Reviewer finding (iteration 2): no test exercised `record_cap` at
  // all — a break-probe deleting the whole `if (record_count >
  // records_left) { record_cap; break; }` block SURVIVED every existing
  // test. The file holds two non-empty (record-bearing) lines; a
  // one-record budget cannot admit it, so it is skipped rather than
  // scanned, and the coverage row's `scanned` count stays 0.
  scratch_dir scratch;
  write(scratch.path_ / ".claude" / "projects" / "a.jsonl", "not json\nalso not json\n");

  ia::transcript_config const config{.home_dir = scratch.path_.string()};
  ia::collector_limits const  limits{.max_records = 1};
  auto const                  preview = ia::collect_preview_from_paths(config, std::nullopt, limits);

  auto const* claude = coverage_for(preview, ia::vendor::claude);
  REQUIRE(claude != nullptr);
  CHECK(claude->scanned == 0);
  CHECK(has_warning(preview.warnings, ia::vendor::claude, ia::warning_kind::record_cap));
}

TEST_CASE("collect_preview_from_paths: files are scanned in sorted order, not filesystem iteration order",
          "[engine][introspection_adapters][discovery][6352][iter2]") {
  // Reviewer finding (iteration 2): the header advertises a determinism
  // guarantee (`std::ranges::sort(paths)`) that no test pinned — a
  // break-probe swapping the sort for a reverse SURVIVED. Files are
  // created in DESCENDING name order (`z` before `a`) so a mutant that
  // scans in filesystem/creation order instead of sorted order picks
  // `z.jsonl` first; with `max_files == 1` only the first-scanned file's
  // shape reaches `coverage`. `a.jsonl` is a recognized-but-irrelevant
  // envelope (ignored, not malformed); `z.jsonl` is a bare unparseable
  // line (malformed). Sorted order must pick `a.jsonl`.
  scratch_dir scratch;
  write(scratch.path_ / ".claude" / "projects" / "z.jsonl", "not json at all\n");
  write(scratch.path_ / ".claude" / "projects" / "a.jsonl", "{\"type\":\"summary\",\"other\":\"irrelevant-record-shape\"}\n");

  ia::transcript_config const config{.home_dir = scratch.path_.string()};
  ia::collector_limits const  limits{.max_files = 1};
  auto const                  preview = ia::collect_preview_from_paths(config, std::nullopt, limits);

  auto const* claude = coverage_for(preview, ia::vendor::claude);
  REQUIRE(claude != nullptr);
  CHECK(claude->scanned == 1);
  CHECK(claude->malformed == 0);
  CHECK(claude->ignored == 1);
  CHECK(has_warning(preview.warnings, ia::vendor::claude, ia::warning_kind::file_cap));
}

TEST_CASE("collect_preview_from_paths: the byte/record budget is SHARED across vendors, not reset per vendor",
          "[engine][introspection_adapters][discovery][6352][iter2]") {
  // Reviewer finding (iteration 2): every existing cap test uses a single
  // vendor, so a break-probe neutralizing `bytes_left -=`/`records_left
  // -=` after each accepted file SURVIVED — nothing observed that the
  // budget crosses vendor boundaries. Claude's file exactly exhausts a
  // 10-byte budget; Codex then sees `bytes_left == 0` for ITS file and
  // must trip its OWN byte_cap, proving the same `bytes_left` counter
  // carried over rather than resetting.
  scratch_dir scratch;
  write(scratch.path_ / ".claude" / "projects" / "a.jsonl", "1234567890");
  write(scratch.path_ / ".codex" / "sessions" / "b.jsonl", "1234567890");

  ia::transcript_config const config{.home_dir = scratch.path_.string()};
  ia::collector_limits const  limits{.max_bytes = 10};
  auto const                  preview = ia::collect_preview_from_paths(config, std::nullopt, limits);

  auto const* claude = coverage_for(preview, ia::vendor::claude);
  auto const* codex  = coverage_for(preview, ia::vendor::codex);
  REQUIRE(claude != nullptr);
  REQUIRE(codex != nullptr);
  CHECK(claude->scanned == 1);
  CHECK(codex->scanned == 0);
  CHECK(has_warning(preview.warnings, ia::vendor::codex, ia::warning_kind::byte_cap));
}

TEST_CASE("collect_preview_from_paths: an override naming a single FILE is read directly, not treated as a directory",
          "[engine][introspection_adapters][discovery][6352][iter2]") {
  // Reviewer finding (iteration 2): every existing override test points
  // at a DIRECTORY (via the `/**/*.jsonl` suffix). A break-probe forcing
  // the `status.type() == regular` arm to fall through to the directory
  // branch SURVIVED, because no test names a single file. `open_ec` on
  // `directory_iterator(file_path)` fails (ENOTDIR), so the mutant reports
  // `unavailable` where the un-mutated code reads the file directly and
  // reports `observed`.
  scratch_dir scratch;
  auto const  single_file = scratch.path_ / "custom" / "one.jsonl";
  write(single_file, "not json\n");

  ia::transcript_config const config{.home_dir = scratch.path_.string(), .claude_path = single_file.string()};
  auto const                  preview = ia::collect_preview_from_paths(config, std::nullopt);

  auto const* claude = coverage_for(preview, ia::vendor::claude);
  REQUIRE(claude != nullptr);
  CHECK(claude->state == ia::coverage_state::observed);
  CHECK(claude->scanned == 1);
}

TEST_CASE("collect_preview_from_paths: the CLI adapter feeds an authoritative cli_log source",
          "[engine][introspection_adapters][discovery]") {
  scratch_dir scratch;

  ia::transcript_config const config{
      .home_dir = scratch.path_.string(), .claude_enabled = false, .codex_enabled = false, .copilot_enabled = false};
  ia::cli_log_adapter const adapter{
      .enabled = true,
      .read    = [](std::size_t) -> ia::cli_read_result {
        return ia::cli_read_result{
            .status = ia::cli_read_status::ok,
            .bytes  = "{\"schema\":1,\"kind\":\"cli_invocation\",\"verb_path\":\"planar plan show\","
                      "\"exit_code\":0,\"error_category\":null,\"recorded_at\":\"2026-07-12T12:00:01Z\"}\n",
        };
      },
  };
  auto const preview = ia::collect_preview_from_paths(config, adapter);

  auto const* cli_log = coverage_for(preview, ia::vendor::cli_log);
  REQUIRE(cli_log != nullptr);
  CHECK(cli_log->state == ia::coverage_state::observed);
  CHECK(cli_log->scanned == 1);
  // exit_code 0, no retry/abandon/gap evidence -> ignored, not a signal.
  CHECK(cli_log->ignored == 1);
}

TEST_CASE("collect_preview_from_paths: an adapter read failure raises cli_adapter_failed, an empty read does not",
          "[engine][introspection_adapters][discovery]") {
  scratch_dir                 scratch;
  ia::transcript_config const config{
      .home_dir = scratch.path_.string(), .claude_enabled = false, .codex_enabled = false, .copilot_enabled = false};

  ia::cli_log_adapter const failing{
      .enabled = true,
      .read    = [](std::size_t) -> ia::cli_read_result { return ia::cli_read_result{.status = ia::cli_read_status::failed}; },
  };
  auto const failed_preview = ia::collect_preview_from_paths(config, failing);
  CHECK(has_warning(failed_preview.warnings, ia::vendor::cli_log, ia::warning_kind::cli_adapter_failed));
  CHECK(coverage_for(failed_preview, ia::vendor::cli_log)->state == ia::coverage_state::unavailable);

  ia::cli_log_adapter const unavailable{
      .enabled = true,
      .read    = [](std::size_t) -> ia::cli_read_result { return ia::cli_read_result{}; }, // default: unavailable, no error
  };
  auto const unavailable_preview = ia::collect_preview_from_paths(config, unavailable);
  CHECK_FALSE(has_warning(unavailable_preview.warnings, ia::vendor::cli_log, ia::warning_kind::cli_adapter_failed));
  CHECK(coverage_for(unavailable_preview, ia::vendor::cli_log)->state == ia::coverage_state::unavailable);
}

TEST_CASE("collect_preview_from_paths: a disabled CLI adapter reports coverage disabled",
          "[engine][introspection_adapters][discovery]") {
  scratch_dir                 scratch;
  ia::transcript_config const config{
      .home_dir = scratch.path_.string(), .claude_enabled = false, .codex_enabled = false, .copilot_enabled = false};
  ia::cli_log_adapter const adapter{.enabled = false, .read = {}};
  auto const                preview = ia::collect_preview_from_paths(config, adapter);

  auto const* cli_log = coverage_for(preview, ia::vendor::cli_log);
  REQUIRE(cli_log != nullptr);
  CHECK(cli_log->state == ia::coverage_state::disabled);
}
