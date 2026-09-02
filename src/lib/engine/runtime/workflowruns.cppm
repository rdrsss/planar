/// @file workflowruns.cppm
/// @brief Lifecycle store for the workflow-harness-owned `workflow_runs` table.
///
/// This is deliberately separate from `planar.engine.runs.lifecycle`: that
/// module owns the experimental `runs` table, while an external
/// `planar-execute` workflow records its own process in `workflow_runs` by
/// shelling `planar-agent run start/end`.  Conflating the two silently loses
/// the caller supplied identifier and PID that reconcile later needs.
module;

export module planar.engine.runtime.workflowruns;

import std;
import planar.db;

namespace planar::engine::runtime::workflowruns {

export enum class error : std::uint8_t { query_failed, plan_not_found, run_not_found, not_running };

export struct start_input {
  std::int64_t     plan_id = 0, pid = 0;
  std::string_view workflow_name, run_identifier, repo_root;
};

export struct run {
  std::int64_t id = 0, plan_id = 0, pid = 0;
  std::string  workflow_name, run_identifier, repo_root, status;
};

/// Insert a running workflow row after proving its plan exists.
export auto start(db::connection&, const start_input&) -> std::expected<run, error>;

/// Look up one harness run by its caller-supplied identifier.
export auto find(db::connection&, std::string_view run_identifier) -> std::expected<run, error>;

/// Atomically move a running row to an allowed terminal status and timestamp
/// it. `abandoned` is intentionally absent: reconcile owns that transition.
export auto end(db::connection&, std::string_view run_identifier, std::string_view status) -> std::expected<run, error>;

/// The only statuses the public end verb can write.
export auto terminal_status(std::string_view) -> bool;

} // namespace planar::engine::runtime::workflowruns
