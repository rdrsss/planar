/// @file capture.cppm
/// @brief `planar.engine.runtime.capture` — the orchestrator behind the
/// `planar capture` verb family, plus each leaf's output envelope
/// (plan 996, task 6094).
///
/// Behavior-preserving port (D2) of zig/src/engine/runtime/capture.zig,
/// plus the body-composition and rendering halves of
/// zig/src/cmd/planar/handlers/capture/{session,end,note,command,file,
/// snapshot,util}.zig. Six of the seven `capture` schema leaves are
/// covered here; see the cut list below for the seventh.
///
/// Every output string below was captured by RUNNING the oracle against a
/// scratch database — see capture.t.cpp's header for the verbatim probe
/// session and its transcript.
///
/// ## What is NOT ported, and why
///
/// - **`capture commits` (the seventh leaf).** It rests on
///   zig/src/engine/runtime/sessioncommits.zig (1205 lines) whose entire
///   job is shelling `git` — `rev-parse`, revision walks, per-commit
///   metadata extraction — through `std.process.run`. Porting it means
///   standing up a process-spawn abstraction this tree does not have, and
///   the resulting tests would be git-fixture tests rather than the
///   oracle-pinned DB tests the rest of this module ships. Deferred with
///   its dependency, as an explicitly named gap rather than a silent one.
/// - **`open_session`'s automatic git probe.** The Zig `openSession` runs
///   `git rev-parse --show-toplevel` / `git rev-parse HEAD` in the process
///   cwd and stamps the result onto the row, best-effort, ignoring every
///   failure. That is the same `std::process::run` dependency as above.
///   This port therefore takes the already-probed context as an OPTIONAL
///   `start_git_context` argument: pass it and the row is stamped through
///   `session::set_start_git_context_if_unset` (whose write-once semantics
///   ARE ported and tested); omit it and the stamping step is skipped,
///   exactly as the Zig original skips it when the probe fails.
///
///   SINCE TASK 6128 the `capture session` handler DOES supply it, probing
///   through the layer-1 `planar.git` seam. The note above used to end "no
///   caller in this tree can spawn a process yet, so nothing is lost
///   today" — that was true of the ENGINE and false of the VERB: taking
///   the default meant every session this binary opened carried NULL in
///   both columns while emitting the oracle's exact stdout, and the
///   `commits` no-op two paragraphs down therefore applied to every one of
///   them. The optional stays optional (an engine caller outside a
///   repository legitimately has nothing to pass), but a HANDLER that omits
///   it is a defect, and `handlers.t.cpp`'s `[6128]` cases assert the row
///   rather than the stdout precisely because nothing else can see it.
/// - **`close_session`'s `recordSessionWindow`.** Same reason: it walks
///   git commits between `head_sha_at_start` and HEAD. It is a no-op
///   whenever `repo_root` or `head_sha_at_start` is NULL, which is every
///   session this tree can currently open, so omitting it changes nothing
///   observable here. Named in the report as part of the `commits` gap.
///
/// The `--session` / `$PLANAR_VENDOR` resolution the four append-style
/// leaves share IS ported (`resolve_session_id`), because it is pure
/// environment + DB and it is where the operator-visible
/// "creates a session if none is active" behavior lives.
module;

export module planar.engine.runtime.capture;

import std;
import planar.db;
import planar.engine.runtime.session;
import planar.engine.runtime.snapshot;

namespace planar::engine::runtime::capture {

/// @brief Error surface for this module. The union of the two underlying
/// modules' errors, mirroring zig's `capture.Error`.
export enum class capture_error : std::uint8_t {
  not_found,         ///< No session (or snapshot) with that id.
  already_ended,     ///< `close_session` on a session that already ended.
  task_conflict,     ///< Reusing a session whose bound task differs from the caller's.
  no_active_session, ///< No active session for the vendor tuple, where one was required.
  query_failed,      ///< An underlying SQL statement failed.
};

/// @brief The git context `open_session` stamps onto a fresh session row.
/// Supplied by the caller rather than probed here — see this file's
/// header for why.
export struct start_git_context {
  std::string_view repo_root;         ///< Output of `git rev-parse --show-toplevel`, trimmed.
  std::string_view head_sha_at_start; ///< Output of `git rev-parse HEAD`, trimmed.
};

/// @brief Arguments to `open_session`. Mirrors zig's `capture.OpenArgs`.
export struct open_args {
  std::string_view                vendor;            ///< Vendor identity; callers default to `"cli"`.
  std::optional<std::string_view> vendor_session_id; ///< The vendor's own session id, when known.
  std::optional<std::int64_t>     task_id;           ///< Task to bind, when the operator passed `--task`.
  std::optional<std::string_view> model;             ///< Model identifier, when the operator passed `--model`.
};

/// @brief Arguments to `take_snapshot`. Mirrors zig's
/// `capture.SnapshotArgs`.
export struct snapshot_args {
  std::int64_t                    session_id{};      ///< Owning session.
  std::optional<std::int64_t>     task_id;           ///< Task to bind, when known.
  std::string_view                vendor;            ///< Vendor identity.
  std::optional<std::string_view> vendor_session_id; ///< The vendor's own session id, when known.
  std::optional<std::string_view> body;              ///< Narrative body.
  std::optional<std::string_view> next_action;       ///< Exact next action.
};

/// @brief Open or reuse a session and append its `session opened` marker.
///
/// The marker append is NOT idempotent, and that is the oracle's own
/// behavior, pinned deliberately: running `planar capture session` twice
/// with the same vendor tuple returned the same row id both times and left
/// TWO `action | session opened` entries on the timeline. A port that
/// suppressed the second append would diverge.
///
/// The marker is best-effort — its failure is swallowed so a hiccup in the
/// entry insert cannot lose the session row, matching the Zig original.
///
/// @param conn An open, migrated database connection.
/// @param args The vendor tuple and optional task/model.
/// @param git When set, stamps `repo_root`/`head_sha_at_start` onto the
/// row through the write-once path before the returned row is re-read.
/// @return The opened-or-reused session.
export auto open_session(db::connection& conn, const open_args& args, std::optional<start_git_context> git = std::nullopt)
    -> std::expected<session::session, capture_error>;

/// @brief Append a `session ended` note, then set `ended_at`/`summary`.
///
/// Order matters and is preserved: the note lands BEFORE the row is
/// flipped, so the timeline records the boundary. The note append is
/// best-effort; the `end_session` write is not.
///
/// @param conn An open, migrated database connection.
/// @param session_id The session to close.
/// @param summary The summary to store, or unset.
/// @return Success, `capture_error::not_found`, or
/// `capture_error::already_ended`.
export auto close_session(db::connection& conn, std::int64_t session_id, std::optional<std::string_view> summary)
    -> std::expected<void, capture_error>;

/// @brief Append a `note`-prefixed timeline entry.
/// @param conn An open, migrated database connection.
/// @param session_id The owning session.
/// @param body The note text.
/// @return Success, or `capture_error::query_failed`.
export auto append_note(db::connection& conn, std::int64_t session_id, std::string_view body)
    -> std::expected<void, capture_error>;

/// @brief Append a `command`-prefixed timeline entry.
/// @param conn An open, migrated database connection.
/// @param session_id The owning session.
/// @param body The command text (see `compose_command_body`).
/// @return Success, or `capture_error::query_failed`.
export auto append_command(db::connection& conn, std::int64_t session_id, std::string_view body)
    -> std::expected<void, capture_error>;

/// @brief Append a `file`-prefixed timeline entry.
/// @param conn An open, migrated database connection.
/// @param session_id The owning session.
/// @param body The file text (see `compose_file_body`).
/// @return Success, or `capture_error::query_failed`.
export auto append_file(db::connection& conn, std::int64_t session_id, std::string_view body)
    -> std::expected<void, capture_error>;

/// @brief Create a `context_snapshots` row and append a matching
/// `snapshot created: id=<n>` note to the same session's timeline.
///
/// The note is best-effort: when it fails the snapshot is still returned,
/// mirroring the Zig original's `catch return snap`.
///
/// @param conn An open, migrated database connection.
/// @param args The snapshot payload.
/// @return The stored snapshot, or `capture_error::query_failed`.
export auto take_snapshot(db::connection& conn, const snapshot_args& args) -> std::expected<snapshot::snapshot, capture_error>;

/// @brief Resolve the session id an append-style `capture` leaf should
/// target. Mirrors handlers/capture/util.zig's `resolveSessionId`.
///
/// An explicit `--session` wins outright. Otherwise the vendor tuple is
/// read from `$PLANAR_VENDOR` / `$PLANAR_VENDOR_SESSION_ID` (defaulting
/// vendor to `"cli"`) and the active session for it is returned, CREATING
/// one when none exists — which is why `planar capture note "x"` works
/// with no prior `planar capture session`.
///
/// @param conn An open, migrated database connection.
/// @param explicit_session_id The `--session` value, when supplied.
/// @return The session id to append to.
export auto resolve_session_id(db::connection& conn, std::optional<std::int64_t> explicit_session_id)
    -> std::expected<std::int64_t, capture_error>;

/// @brief Resolve the session id `capture end` and `capture commits`
/// should target. Differs from `resolve_session_id` in one load-bearing
/// way: it NEVER creates a session — an absent active session is
/// `capture_error::no_active_session`, which the oracle surfaces as
/// `error: no active session` with exit 1.
///
/// @param conn An open, migrated database connection.
/// @param explicit_session_id The `--session` (or positional) value, when supplied.
/// @return The session id, or `capture_error::no_active_session`.
export auto resolve_existing_session_id(db::connection& conn, std::optional<std::int64_t> explicit_session_id)
    -> std::expected<std::int64_t, capture_error>;

/// @brief Compose `capture command`'s entry body. With `--outcome`, the
/// oracle stores the command and the outcome on TWO LINES:
/// `"<command>\noutcome: <outcome>"`. Without it, the bare command.
/// @param command The command text.
/// @param outcome The `--outcome` value, when supplied.
/// @return The composed entry body.
export auto compose_command_body(std::string_view command, std::optional<std::string_view> outcome) -> std::string;

/// @brief Compose `capture file`'s entry body. With `--role`, the oracle
/// stores `"<path> [<role>]"`. Without it, the bare path.
/// @param path The file path.
/// @param role The `--role` value, when supplied.
/// @return The composed entry body.
export auto compose_file_body(std::string_view path, std::optional<std::string_view> role) -> std::string;

/// @brief Render `capture session --json`. Oracle-captured:
/// `{"ok":true,"id":1,"vendor":"cli"}`, growing `,"vendor_session_id":".."`
/// and `,"task_id":N` only when those fields are set. The trailing newline
/// the oracle writes (handlers/capture/session.zig:47) is INCLUDED; the
/// caller appends nothing.
/// @param s The opened session.
/// @return The complete stdout payload: the JSON object WITH its newline.
export auto render_session_json(const session::session& s) -> std::string;

/// @brief Render `capture session`'s text line. Oracle-captured:
/// `session 1 opened (vendor: cli)`, growing `, vsid: X` / `, task: N`.
/// @param s The opened session.
/// @return The complete stdout payload: the text line WITH its newline.
export auto render_session_text(const session::session& s) -> std::string;

/// @brief Render the `--json` envelope `capture note|command|file` share.
/// Oracle-captured: `{"ok":true,"session_id":1}`.
/// @param session_id The session appended to.
/// @return The complete stdout payload: the JSON object WITH its newline.
export auto render_append_json(std::int64_t session_id) -> std::string;

/// @brief Render the text line `capture note|command|file` share.
/// Oracle-captured: `captured note in session 1`.
/// @param what The literal leaf noun: `"note"`, `"command"` or `"file"`.
/// @param session_id The session appended to.
/// @return The complete stdout payload: the text line WITH its newline.
export auto render_append_text(std::string_view what, std::int64_t session_id) -> std::string;

/// @brief Render `capture snapshot --json`. Oracle-captured:
/// `{"ok":true,"id":1,"session_id":1,"vendor":"cli"}`, growing
/// `,"task_id":N` only when the snapshot is task-bound.
/// @param snap The stored snapshot.
/// @return The complete stdout payload: the JSON object WITH its newline.
export auto render_snapshot_json(const snapshot::snapshot& snap) -> std::string;

/// @brief Render `capture snapshot`'s text line. Oracle-captured:
/// `snapshot 2 created (vendor: cli)`, growing `, task: N`.
/// @param snap The stored snapshot.
/// @return The complete stdout payload: the text line WITH its newline.
export auto render_snapshot_text(const snapshot::snapshot& snap) -> std::string;

/// @brief Render `capture end --json`. Oracle-captured:
/// `{"ok":true,"id":1}`.
/// @param session_id The session closed.
/// @return The complete stdout payload: the JSON object WITH its newline.
export auto render_end_json(std::int64_t session_id) -> std::string;

/// @brief Render `capture end`'s text line. Oracle-captured:
/// `session 1 ended`.
/// @param session_id The session closed.
/// @return The complete stdout payload: the text line WITH its newline.
export auto render_end_text(std::int64_t session_id) -> std::string;

/// @brief Resolve `capture snapshot`'s `next_action` fallback: an explicit
/// `--next-action` wins; otherwise the bound task's own `next_action`
/// column is used when it is non-empty. Mirrors handlers/capture/
/// snapshot.zig's task lookup.
/// @param conn An open, migrated database connection.
/// @param explicit_next_action The `--next-action` value, when supplied.
/// @param task_id The bound task, when there is one.
/// @return The next action to store, or unset when neither source has one.
export auto resolve_next_action(db::connection& conn, std::optional<std::string_view> explicit_next_action,
                                std::optional<std::int64_t> task_id) -> std::expected<std::optional<std::string>, capture_error>;

} // namespace planar::engine::runtime::capture
