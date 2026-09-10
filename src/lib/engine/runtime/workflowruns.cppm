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

/// @brief Errors produced by workflow-run lifecycle operations.
export enum class error : std::uint8_t {
  query_failed,   ///< An underlying database operation failed.
  plan_not_found, ///< The requested owning plan does not exist.
  run_not_found,  ///< No workflow run has the supplied identifier.
  not_running,    ///< A terminal transition was requested for a non-running row.
};

/// @brief Input for starting an external workflow run.
export struct start_input {
  std::int64_t     plan_id = 0;    ///< Existing plan that owns the run.
  std::int64_t     pid     = 0;    ///< External workflow process id.
  std::string_view workflow_name;  ///< Workflow's declared name.
  std::string_view run_identifier; ///< Caller-supplied, unique workflow run identifier.
  std::string_view repo_root;      ///< Repository root observed by the workflow.
};

/// @brief A `workflow_runs` row owned by an external workflow execution.
export struct run {
  std::int64_t id      = 0;    ///< Row id.
  std::int64_t plan_id = 0;    ///< Owning plan id.
  std::int64_t pid     = 0;    ///< External workflow process id.
  std::string  workflow_name;  ///< Workflow's declared name.
  std::string  run_identifier; ///< Caller-supplied unique run identifier.
  std::string  repo_root;      ///< Repository root observed by the workflow.
  std::string  status;         ///< Lifecycle status, initially `running`.
};

/// @brief Insert a running workflow row after proving its plan exists.
/// @param c An open, migrated database connection.
/// @param input Owning plan and external workflow metadata.
/// @return The inserted run, or `error::plan_not_found` / `error::query_failed`.
export auto start(db::connection& c, const start_input& input) -> std::expected<run, error>;

/// @brief Look up one harness run by its caller-supplied identifier.
/// @param c An open, migrated database connection.
/// @param identifier Unique identifier supplied by the workflow caller.
/// @return The matching run, or `error::run_not_found` / `error::query_failed`.
export auto find(db::connection& c, std::string_view identifier) -> std::expected<run, error>;

/// @brief Atomically move a running row to an allowed terminal status and timestamp
/// it. `abandoned` is intentionally absent: reconcile owns that transition.
/// @param c An open, migrated database connection.
/// @param identifier Unique identifier supplied by the workflow caller.
/// @param status Allowed terminal status to persist.
/// @return The transitioned run, or `error::run_not_found`, `error::not_running`,
/// or `error::query_failed`.
export auto end(db::connection& c, std::string_view identifier, std::string_view status) -> std::expected<run, error>;

/// @brief Test whether a status is writable by the public end verb.
/// @param value Candidate status.
/// @return True only for `completed`, `failed`, or `interrupted`.
export auto terminal_status(std::string_view value) -> bool;

} // namespace planar::engine::runtime::workflowruns
