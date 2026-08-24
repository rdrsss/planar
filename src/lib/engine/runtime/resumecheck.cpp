/// @file resumecheck.cpp
/// @brief Implementation of `planar.engine.runtime.resumecheck` (plan 996,
/// task 6040). See resumecheck.cppm for the derived rule set and for why
/// only `validate` lives here.

module planar.engine.runtime.resumecheck;

import std;
import planar.db;
import planar.json_text;
import planar.engine.runtime.snapshot;

namespace planar::engine::runtime::resumecheck {

namespace {

namespace snap = planar::engine::runtime::snapshot;

} // namespace

auto validate(db::connection& conn, std::int64_t task_id) -> std::expected<validation_result, resume_error> {
  // Read the task's own columns directly rather than through
  // `planar.engine.planning.task`. That is the Zig original's explicit
  // choice ("keep this module independent of engine.planning.task"), and
  // here it is also what keeps this bucket at layer 2 with no
  // `engine_* -> engine_*` edge, which cmake/architecture.cmake FATALs on.
  auto stmt = conn.prepare("select coalesce(next_action, '') from tasks where id = ?");
  if (!stmt) {
    return std::unexpected(resume_error::query_failed);
  }
  if (auto bound = stmt->bind_int64(1, task_id); !bound) {
    return std::unexpected(resume_error::query_failed);
  }
  auto stepped = stmt->step();
  if (!stepped) {
    return std::unexpected(resume_error::query_failed);
  }
  if (*stepped != db::step_result::row) {
    return std::unexpected(resume_error::not_found);
  }
  auto const next_action = stmt->column_text(0);

  validation_result result{.task_id = task_id, .resumable = false, .failures = {}};

  // Check 1 — next_action. A BYTE-LENGTH test, deliberately: a
  // next_action of a single space passes here and passes in the oracle.
  // See this module's header for the probe that established it.
  if (next_action.empty()) {
    result.failures.push_back(validation_failure{
        .check       = "next_action",
        .message     = "next_action is null",
        .remediation = std::format("planar task update {} --next-action \"<text>\"", task_id),
    });
  }

  // Check 2 — at least one TASK-SCOPED context snapshot. A session-level
  // snapshot (task_id NULL) does not satisfy this; `get_latest_for_task`
  // filters on `where task_id = ?`, which NULL never matches.
  auto const latest = snap::get_latest_for_task(conn, task_id);
  if (!latest) {
    return std::unexpected(resume_error::query_failed);
  }
  if (!latest->has_value()) {
    result.failures.push_back(validation_failure{
        .check       = "snapshot",
        .message     = "no context snapshot found",
        .remediation = std::format("planar capture snapshot --task {}", task_id),
    });
  }

  result.resumable = result.failures.empty();
  return result;
}

auto render_validate_json(const validation_result& result) -> std::string {
  std::string out =
      std::format("{{\"task_id\":{},\"resumable\":{},\"failures\":", result.task_id, result.resumable ? "true" : "false");
  // Empty failures serialize as `null`, NOT `[]`. Cross-binary parity with
  // the Go original is load-bearing here and the oracle comment says so.
  if (result.failures.empty()) {
    out += "null}\n";
    return out;
  }
  out += "[";
  bool first = true;
  for (auto const& failure : result.failures) {
    if (!first) {
      out += ",";
    }
    first = false;
    out += "{\"check\":";
    json_text::append_json_string(out, failure.check);
    out += ",\"message\":";
    json_text::append_json_string(out, failure.message);
    out += ",\"remediation\":";
    json_text::append_json_string(out, failure.remediation);
    out += "}";
  }
  out += "]}\n";
  return out;
}

auto render_validate_text(const validation_result& result) -> std::string {
  if (result.resumable) {
    return std::format("OK task:{} is resume-ready\n", result.task_id);
  }
  std::string out = std::format("FAIL task:{} is not resumable:\n", result.task_id);
  for (auto const& failure : result.failures) {
    // U+2192 RIGHTWARDS ARROW, spelled as UTF-8 bytes so the file's own
    // encoding cannot silently change the payload.
    out += std::format("  - {} \xe2\x86\x92 run: {}\n", failure.message, failure.remediation);
  }
  return out;
}

} // namespace planar::engine::runtime::resumecheck
