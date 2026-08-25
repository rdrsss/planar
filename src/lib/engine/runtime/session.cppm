/// @file session.cppm
/// @brief `planar.engine.runtime.session` — the `sessions` table and its
/// append-only `session_entries` timeline (plan 996, task 6094).
///
/// Behavior-preserving port (D2) of zig/src/engine/runtime/session.zig.
/// A session is one vendor's episode of work, keyed on the
/// `(vendor, vendor_session_id)` tuple and idempotent on it: opening a
/// session twice with the same key returns the SAME row (verified against
/// the oracle — two consecutive `planar capture session` invocations both
/// reported `id: 1`). The row may bind to a task; binding is one-way
/// (NULL -> task) once set, and a conflicting rebind is refused.
///
/// Per plan 144 M4 sessions carry no scope columns, so no cross-scope
/// guard applies to any function here — that is the Zig original's
/// documented position, not an omission by this port.
///
/// ## Deliberate omissions
///
/// - **`policy.audit.record`.** `startSession` and `endSession` write
///   `audit_log` rows in the oracle and still do not here. The layer-1
///   `planar.policy` module exists as of task 6100 — this is a remaining
///   gap, not a missing dependency, and it IS observable: running the
///   oracle's `question add` produced
///   `create|session|1|start session vendor=cli`, a row this build does
///   not write. Deferred to a dedicated cycle with handoff and snapshot;
///   see engine/runtime/CMakeLists.txt.
/// - **`vendorFromEnv` / `vendorSessionIdFromEnv`.** Pure `$PLANAR_VENDOR`
///   / `$PLANAR_VENDOR_SESSION_ID` reads. Ported as
///   `vendor_from_env` / `vendor_session_id_from_env` below, since the
///   default-to-`"cli"` rule is a real, oracle-observable contract (an
///   unset `$PLANAR_VENDOR` produced `"vendor":"cli"`).
module;

export module planar.engine.runtime.session;

import std;
import planar.db;

namespace planar::engine::runtime::session {

/// @brief A `sessions` row. Mirrors zig's `session.Session`.
export struct session {
  std::int64_t                id{};              ///< Row id.
  std::optional<std::int64_t> task_id;           ///< Bound task, when the session is bound.
  std::optional<std::int64_t> project_id;        ///< Bound project, when known.
  std::optional<std::int64_t> agent_id;          ///< Bound agent, when known.
  std::string                 vendor;            ///< Vendor identity (`cli`, `claude`, ...).
  std::optional<std::string>  vendor_session_id; ///< The vendor's own session id, when supplied.
  std::optional<std::string>  model;             ///< Model identifier, when supplied.
  std::string                 started_at;        ///< Creation timestamp.
  std::optional<std::string>  ended_at;          ///< End timestamp; unset while the session is active.
  std::optional<std::string>  summary;           ///< End-of-session summary, when supplied.
  std::optional<std::string>  repo_root;         ///< Git repo root captured at session open.
  std::optional<std::string>  head_sha_at_start; ///< Git HEAD sha captured at session open.
};

/// @brief A `session_entries` row. Mirrors zig's `session.SessionEntry`.
export struct session_entry {
  std::int64_t id{};         ///< Row id.
  std::int64_t session_id{}; ///< Owning session.
  std::int64_t ordinal{};    ///< 1-based position within the session.
  std::string  prefix;       ///< One of: action, observation, decision, question, file, command, note, error, read.
  std::string  body;         ///< The entry text.
  std::string  created_at;   ///< Creation timestamp.
};

/// @brief Arguments to `start_session`. Mirrors zig's `session.StartArgs`.
export struct start_args {
  std::string_view                vendor;            ///< Vendor identity; never empty (callers default to `"cli"`).
  std::optional<std::string_view> vendor_session_id; ///< The vendor's own session id, when known.
  std::optional<std::int64_t>     task_id;           ///< Task to bind, when the caller wants one.
  std::optional<std::string_view> model;             ///< Model identifier, when known.
};

/// @brief Error surface for this module. Mirrors zig's `session.Error`.
export enum class session_error : std::uint8_t {
  not_found,     ///< No session with that id.
  already_ended, ///< `end_session` called on a session that already has `ended_at`.
  task_conflict, ///< Reusing a session whose bound task differs from the caller's.
  query_failed,  ///< An underlying SQL statement failed.
};

/// @brief Read `$PLANAR_VENDOR`, defaulting to `"cli"` when unset OR set
/// to the empty string. Mirrors zig's `vendorFromEnv`. Verified against
/// the oracle: with `$PLANAR_VENDOR` unset, `planar capture session --json`
/// reported `{"ok":true,"id":1,"vendor":"cli"}`.
/// @return The vendor identity to use.
export auto vendor_from_env() -> std::string;

/// @brief Read `$PLANAR_VENDOR_SESSION_ID`, treating unset AND empty
/// alike as "absent". Mirrors zig's `vendorSessionIdFromEnv`.
/// @return The vendor session id, or unset.
export auto vendor_session_id_from_env() -> std::optional<std::string>;

/// @brief Open or reuse a session keyed on `(vendor, vendor_session_id)`.
///
/// Idempotent on that tuple: when a matching row exists it is returned as
/// is (lowest id first). If the existing row has a NULL `task_id` and
/// `args.task_id` is set, the binding is applied; if the existing row is
/// bound to a DIFFERENT task, the call is refused with
/// `session_error::task_conflict`. Mirrors zig's `startSession`.
///
/// Note the asymmetry this preserves: `model` is written only on INSERT.
/// Reusing an existing session with a different `--model` does NOT rewrite
/// the stored model, matching the Zig original.
///
/// @param conn An open, migrated database connection.
/// @param args The vendor tuple and optional task/model.
/// @return The opened-or-reused session row.
export auto start_session(db::connection& conn, const start_args& args) -> std::expected<session, session_error>;

/// @brief Set `ended_at` (and optionally `summary`) on a session.
///
/// @param conn An open, migrated database connection.
/// @param session_id The session to end.
/// @param summary_text The summary to store, or unset to leave it alone.
/// @return Success, `session_error::not_found` when no such session, or
/// `session_error::already_ended` when `ended_at` is already set.
export auto end_session(db::connection& conn, std::int64_t session_id, std::optional<std::string_view> summary_text)
    -> std::expected<void, session_error>;

/// @brief Return the active (`ended_at IS NULL`) session for a vendor
/// tuple. Mirrors zig's `activeForVendor`.
///
/// @param conn An open, migrated database connection.
/// @param vendor The vendor identity.
/// @param vendor_session_id The vendor's own session id, or unset to match
/// rows whose `vendor_session_id` is SQL NULL (NOT "any value" — an unset
/// argument matches only NULL rows, which is why `capture end` reported
/// `no active session` in the oracle probe once the `cli`/NULL session was
/// ended, even though a live `claude`/`abc` session still existed).
/// @return The active session, `std::nullopt` when there is none.
export auto active_for_vendor(db::connection& conn, std::string_view vendor, std::optional<std::string_view> vendor_session_id)
    -> std::expected<std::optional<session>, session_error>;

/// @brief Return the id of the active session for a vendor tuple,
/// creating one if none exists. Mirrors zig's `ensureActive` — the
/// resolver every `capture note|command|file|snapshot` invocation runs
/// when no `--session` was given.
///
/// @param conn An open, migrated database connection.
/// @param vendor The vendor identity.
/// @param vendor_session_id The vendor's own session id, or unset.
/// @return The active-or-created session id.
export auto ensure_active(db::connection& conn, std::string_view vendor, std::optional<std::string_view> vendor_session_id)
    -> std::expected<std::int64_t, session_error>;

/// @brief Append a `session_entries` row. `ordinal` is computed as
/// `MAX(ordinal) + 1` within the session, so the timeline is dense and
/// 1-based. Mirrors zig's `appendEntry`.
///
/// @param conn An open, migrated database connection.
/// @param session_id The owning session.
/// @param prefix The entry prefix (action, note, command, file, ...).
/// @param body The entry text.
/// @return Success, or `session_error::query_failed` (which is also how a
/// nonexistent `session_id` surfaces — as a foreign-key violation).
export auto append_entry(db::connection& conn, std::int64_t session_id, std::string_view prefix, std::string_view body)
    -> std::expected<void, session_error>;

/// @brief Persist the session-open git context ONCE. Existing non-null
/// values are left untouched, so a reused session keeps its original
/// start boundary. Mirrors zig's `setStartGitContextIfUnset`.
///
/// @param conn An open, migrated database connection.
/// @param session_id The session to stamp.
/// @param repo_root The git repo root.
/// @param head_sha_at_start The git HEAD sha at session open.
/// @return Success, or `session_error::query_failed`.
export auto set_start_git_context_if_unset(db::connection& conn, std::int64_t session_id, std::string_view repo_root,
                                           std::string_view head_sha_at_start) -> std::expected<void, session_error>;

/// @brief Read one session by id.
/// @param conn An open, migrated database connection.
/// @param id The session row id.
/// @return The session, or `session_error::not_found`.
export auto get_by_id(db::connection& conn, std::int64_t id) -> std::expected<session, session_error>;

/// @brief List a session's timeline in `ordinal` order.
/// @param conn An open, migrated database connection.
/// @param session_id The owning session.
/// @return The entries, oldest first.
export auto list_entries_for_session(db::connection& conn, std::int64_t session_id)
    -> std::expected<std::vector<session_entry>, session_error>;

} // namespace planar::engine::runtime::session
