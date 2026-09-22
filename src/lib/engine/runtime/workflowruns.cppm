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
  pid_bound,      ///< `heartbeat` was called on a run supervised by pid, not by lease.
  unsupervised,   ///< `start` was given neither a pid nor a lease ttl.
};

/// @brief Input for starting an external workflow run.
///
/// Exactly one of `pid` / `ttl_secs` must be set (decision D11, task 6847):
/// a pid-bound run is supervised by `reconcile`'s pid-probe; a pid-less run
/// is supervised by its `expires_at` lease, set from `ttl_secs` at insert
/// time and extended by `heartbeat`. The migration-00039 CHECK is the
/// database's own backstop for this — this struct is the caller's.
export struct start_input {
  std::int64_t                plan_id = 0;    ///< Existing plan that owns the run.
  std::optional<std::int64_t> pid;            ///< External workflow process id, when pid-supervised.
  std::optional<std::int64_t> ttl_secs;       ///< Lease seconds from now, when pid-less.
  std::string_view            workflow_name;  ///< Workflow's declared name.
  std::string_view            run_identifier; ///< Caller-supplied, unique workflow run identifier.
  std::string_view            repo_root;      ///< Repository root observed by the workflow.
};

/// @brief A `workflow_runs` row owned by an external workflow execution.
export struct run {
  std::int64_t                id      = 0;    ///< Row id.
  std::int64_t                plan_id = 0;    ///< Owning plan id.
  std::optional<std::int64_t> pid;            ///< External workflow process id, when pid-supervised.
  std::optional<std::string>  expires_at;     ///< Lease deadline, when pid-less.
  std::string                 workflow_name;  ///< Workflow's declared name.
  std::string                 run_identifier; ///< Caller-supplied unique run identifier.
  std::string                 repo_root;      ///< Repository root observed by the workflow.
  std::string                 status;         ///< Lifecycle status, initially `running`.
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

/// @brief Extend a pid-less run's lease by setting `expires_at` to `ttl_secs`
/// seconds from now (decision D11).
/// @param c An open, migrated database connection.
/// @param identifier Unique identifier supplied by the workflow caller.
/// @param ttl_secs Seconds from now the new lease deadline should be.
/// @return The updated run, or `error::run_not_found`, `error::not_running`,
/// `error::pid_bound` (the run is supervised by pid, not by lease), or
/// `error::query_failed`.
export auto heartbeat(db::connection& c, std::string_view identifier, std::int64_t ttl_secs) -> std::expected<run, error>;

/// @brief Test whether a status is writable by the public end verb.
/// @param value Candidate status.
/// @return True only for `completed`, `failed`, or `interrupted`.
export auto terminal_status(std::string_view value) -> bool;

} // namespace planar::engine::runtime::workflowruns
