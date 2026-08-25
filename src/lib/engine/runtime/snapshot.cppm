/// @file snapshot.cppm
/// @brief `planar.engine.runtime.snapshot` — the `context_snapshots` table
/// (plan 996, task 6094).
///
/// Behavior-preserving port (D2) of zig/src/engine/runtime/snapshot.zig.
/// A context snapshot is the resume-packet payload produced at terminate
/// time or on demand: a narrative body, an exact next action, and the
/// vendor identity needed for cross-vendor handoff.
///
/// The load-bearing detail this port preserves: an EMPTY `body` or
/// `next_action` is written as SQL NULL, not as `''`. The Zig insert
/// binds `if (b.len > 0) .text else .null` for both. Verified against the
/// oracle: `planar capture snapshot "body 2"` (no `--next-action`, and no
/// task bound so nothing to inherit from) stored `next_action` as NULL.
/// The read side then coalesces both back to `""`, so the round-trip is
/// lossy in exactly the way the original is.
///
/// `policy.audit.record` is STILL omitted, but no longer for want of a
/// module: task 6100 landed layer-1 `planar.policy` and wired
/// `engine_planning`/`engine_identity`. This bucket's seven call sites
/// (session 2, handoff 4, snapshot 1) were left for a dedicated cycle —
/// see engine/runtime/CMakeLists.txt. They ARE observable: the oracle
/// writes `create|session|<id>|start session vendor=cli` where this build
/// writes nothing.
module;

export module planar.engine.runtime.snapshot;

import std;
import planar.db;

namespace planar::engine::runtime::snapshot {

/// @brief A `context_snapshots` row. Mirrors zig's `snapshot.Snapshot`.
export struct snapshot {
  std::int64_t                id{};              ///< Row id.
  std::int64_t                session_id{};      ///< Owning session.
  std::optional<std::int64_t> task_id;           ///< Bound task, when the snapshot has one.
  std::string                 vendor;            ///< Vendor identity.
  std::optional<std::string>  vendor_session_id; ///< The vendor's own session id, when supplied.
  std::string                 body;              ///< Narrative body; `""` when stored NULL.
  std::string                 next_action;       ///< Exact next action; `""` when stored NULL.
  std::string                 created_at;        ///< Creation timestamp.
};

/// @brief Arguments to `create`. Mirrors zig's `snapshot.CreateArgs`.
export struct create_args {
  std::int64_t                    session_id{};      ///< Owning session.
  std::optional<std::int64_t>     task_id;           ///< Task to bind, when known.
  std::string_view                vendor;            ///< Vendor identity.
  std::optional<std::string_view> vendor_session_id; ///< The vendor's own session id, when known.
  std::optional<std::string_view> body;              ///< Narrative body; empty or unset stores SQL NULL.
  std::optional<std::string_view> next_action;       ///< Next action; empty or unset stores SQL NULL.
};

/// @brief Error surface for this module. Mirrors zig's `snapshot.Error`.
export enum class snapshot_error : std::uint8_t {
  not_found,    ///< No snapshot with that id.
  query_failed, ///< An underlying SQL statement failed.
};

/// @brief Insert a `context_snapshots` row and return it as stored.
/// @param conn An open, migrated database connection.
/// @param args The snapshot payload.
/// @return The stored snapshot, or `snapshot_error::query_failed`.
export auto create(db::connection& conn, const create_args& args) -> std::expected<snapshot, snapshot_error>;

/// @brief Read one snapshot by id.
/// @param conn An open, migrated database connection.
/// @param id The snapshot row id.
/// @return The snapshot, or `snapshot_error::not_found`.
export auto show(db::connection& conn, std::int64_t id) -> std::expected<snapshot, snapshot_error>;

/// @brief Return the most recent snapshot for a task, ordered by
/// `(created_at DESC, id DESC)` so ties inside one millisecond still
/// resolve stably. Mirrors zig's `getLatestForTask`.
/// @param conn An open, migrated database connection.
/// @param task_id The task to look under.
/// @return The latest snapshot, or `std::nullopt` when the task has none.
export auto get_latest_for_task(db::connection& conn, std::int64_t task_id)
    -> std::expected<std::optional<snapshot>, snapshot_error>;

/// @brief List every snapshot for a task, newest first.
/// @param conn An open, migrated database connection.
/// @param task_id The task to look under.
/// @return The snapshots, newest first.
export auto list_for_task(db::connection& conn, std::int64_t task_id) -> std::expected<std::vector<snapshot>, snapshot_error>;

} // namespace planar::engine::runtime::snapshot
