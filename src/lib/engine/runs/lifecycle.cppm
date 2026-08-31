/// @file lifecycle.cppm
/// @brief `planar.engine.runs.lifecycle` — the run-record lifecycle: the
/// measurement-rig substrate behind the `planar bench *` and `planar run *`
/// verb groups (plan 996, task 6095).
///
/// Behavior-preserving port (D2) of zig/src/engine/runs/runs.zig. Owns the
/// SQLite writes for the three tables migration 00025 introduced:
///
///   - `runs`        — one row per (plan, arm, repetition); the experimental
///                     unit. `run_uid` is the stable external handle.
///   - `run_events`  — the run's append-only, seq-ordered journal. `kind` and
///                     `payload` are opaque text at this layer (the CLI
///                     validates `payload` as JSON before calling in).
///   - `run_touches` — the declared-vs-actual touch harvest, one row per
///                     (task, path) tagged `declared` or `actual`.
///
/// ## Two verb families, one table family
///
/// This was NOT obvious and was established by probing the oracle, not by
/// reading names: `planar run *` does **not** use the `workflow_runs` table
/// despite that table existing. `run start` writes a `runs` row exactly like
/// `bench start` does, with `arm` defaulting to the literal `"op"` (or the
/// `--workflow` value when given) and `base_sha` / `config_hash` set to the
/// empty string. Verified:
///
///   $Z run start --plan 1 --json
///       -> {"run_uid":"c5678087d1831bc7fe1e47a35d35f5f3","plan_id":1,"arm":"op"}
///   sqlite3 p.db 'select id,run_uid,arm,base_sha from runs'
///       3|c5678087d1831bc7fe1e47a35d35f5f3|op||
///
/// The two families therefore differ only in their surface: `bench` takes a
/// caller-minted `run_uid` and a caller-assigned `seq`; `run` mints the uid
/// (`lower(hex(randomblob(16)))`) and auto-increments the seq. `bench show`
/// renders touches; `run show` deliberately omits them.
///
/// ## The declared-touch snapshot (the non-obvious part of `start`)
///
/// `start` is not a bare INSERT. In the SAME transaction as the `runs` row it
/// copies the plan's current `task_touch_paths` into `run_touches` as
/// `kind='declared'` rows. Oracle-verified — the brief did not mention this
/// and it is easy to miss:
///
///   $Z bench start r10 --plan 1 --arm strict --base-sha s --config-hash c
///   $Z bench show r10 --json
///       -> ..."touches":[{"id":4,"task_id":1,"path":"src/a.zig","kind":"declared",...},
///                        {"id":5,"task_id":2,"path":"src/b.zig","kind":"declared",...}]
///
/// The snapshot is a VALUE COPY, not a live FK: mutating `task_touch_paths`
/// after `start` cannot retroactively rewrite a recorded prediction. That
/// immutability is the instrument the RQ1 experiment measures.
///
/// Scoping, also oracle-verified rather than assumed:
///
///   - Without a task filter, EVERY task under the plan is snapshotted —
///     including tasks in a terminal status. Task 2 was `done` and its
///     `src/b.zig` still landed in run `r11`'s snapshot. (Contrast
///     `planar.engine.runs` has no say in `groups recommend`, which filters
///     to `status='todo'`; the two surfaces genuinely disagree.)
///   - With a task filter, only the listed ids are snapshotted, and the join
///     to `tasks` still constrains them to the plan.
///
/// ## Atomicity
///
/// The Zig original wraps the insert + snapshot in a named savepoint. This
/// module's `planar.db` has no savepoint API, so `start` uses a plain
/// `db::transaction` (rollback-on-scope-exit). The observable contract is
/// identical for every caller in this tree: `start` is never invoked from
/// inside an outer transaction, so a savepoint and a transaction nest the
/// same way here. A run is never left half-snapshotted.
///
/// ## What is NOT ported, named rather than silently dropped
///
/// - **`policy.audit` rows.** No ported bucket writes them (see
///   engine/planning/CMakeLists.txt, engine/promotion, engine/runtime). No
///   `bench` or `run` leaf reads or emits audit rows, so no observable CLI
///   contract is affected.

module;

export module planar.engine.runs.lifecycle;

import std;
import planar.db;

namespace planar::engine::runs::lifecycle {

/// @brief The declared-vs-actual touch discriminator.
///
/// `declared` is the predicted closure snapshotted at run start; `actual` is
/// ground truth harvested at fan-in. The schema enforces this closed
/// two-value set with a CHECK; this enum mirrors it so the engine refuses an
/// out-of-set value before reaching SQLite.
export enum class touch_kind {
  declared, ///< Predicted, snapshotted from `task_touch_paths` at `start`.
  actual    ///< Ground truth, harvested from the worktree diff.
};

/// @brief Parse the schema's touch-kind text into the enum.
/// @param text The candidate text (`"declared"` or `"actual"`).
/// @return The kind, or `std::nullopt` for anything else.
export auto touch_kind_from_text(std::string_view text) -> std::optional<touch_kind>;

/// @brief Render a touch kind as the schema's own text.
/// @param kind The kind to render.
/// @return `"declared"` or `"actual"`.
export auto touch_kind_to_text(touch_kind kind) -> std::string_view;

/// @brief A run record as read back by `show` / `show_by_uid`.
///
/// `arm` and `status` are plain text: their enum sets are enforced at the CLI
/// parse layer, not the schema, so pilot/probe runs need no migration. The
/// oracle only WARNS on an unrecognized arm and accepts it
/// (`warn: bench start: unrecognized arm 'a1'; recognized arms: strict,
/// eligibility, grouped`, exit 0 — captured).
export struct run {
  std::int64_t               id = 0;      ///< The `runs.id` autoincrement key.
  std::string                run_uid;     ///< The stable external handle.
  std::int64_t               plan_id = 0; ///< The owning plan.
  std::string                arm;         ///< Experiment arm (free text).
  std::string                base_sha;    ///< Corpus baseline sha (empty for op runs).
  std::string                config_hash; ///< Experiment config hash (empty for op runs).
  std::optional<std::string> config_json; ///< Raw JSON blob, or unset.
  std::optional<std::string> corpus_repo; ///< Corpus repo name, or unset.
  std::string                status;      ///< `running` until `finish`.
  std::string                started_at;  ///< Schema-stamped ISO-8601.
  std::optional<std::string> ended_at;    ///< Stamped by `finish`, else unset.
};

/// @brief A journal row as read back by `events`.
export struct event_row {
  std::int64_t               id     = 0; ///< The `run_events.id` key.
  std::int64_t               run_id = 0; ///< The owning run.
  std::int64_t               seq    = 0; ///< Per-run ordinal, UNIQUE with `run_id`.
  std::string                kind;       ///< Opaque event kind.
  std::optional<std::string> payload;    ///< Raw JSON blob, or unset.
  std::string                created_at; ///< Schema-stamped ISO-8601.
};

/// @brief A touch row as read back by `touches`.
export struct touch_row {
  std::int64_t id      = 0;                  ///< The `run_touches.id` key.
  std::int64_t run_id  = 0;                  ///< The owning run.
  std::int64_t task_id = 0;                  ///< Plain integer, NOT an FK cascade.
  std::string  path;                         ///< Repo-relative path.
  touch_kind   kind_ = touch_kind::declared; ///< Declared or actual.
  std::string  created_at;                   ///< Schema-stamped ISO-8601.
};

/// @brief Failure surface for every operation in this module.
export enum class runs_error {
  not_found,         ///< No run with that id / run_uid.
  duplicate_run_uid, ///< `runs.run_uid` UNIQUE violation.
  duplicate_seq,     ///< `run_events (run_id, seq)` UNIQUE violation.
  query_failed       ///< Any other SQLite failure.
};

/// @brief Arguments to `start`.
export struct start_args {
  /// Stable external id minted by the caller. The engine does not synthesize
  /// one — `run start` mints it via `generate_run_uid` and passes it in.
  std::string_view                run_uid;
  std::int64_t                    plan_id = 0; ///< The owning plan; an FK into `plans`.
  std::string_view                arm;         ///< Experiment arm; free text (only warned on).
  std::string_view                base_sha;    ///< Corpus baseline sha; EMPTY, not null, for op runs.
  std::string_view                config_hash; ///< Experiment config hash; empty for op runs.
  std::optional<std::string_view> config_json; ///< Raw JSON blob, validated by the caller.
  std::optional<std::string_view> corpus_repo; ///< Corpus repo name, when there is one.
  /// Initial status; the schema default `'running'` applies when unset.
  std::optional<std::string_view> status;
  /// Optional task-id filter for the declared-touch snapshot. Unset means
  /// "snapshot every task under the plan" (see this module's header for the
  /// oracle capture proving terminal-status tasks are included).
  std::optional<std::vector<std::int64_t>> task_filter;
};

/// @brief The result of `start`.
export struct start_result {
  std::int64_t id = 0;                   ///< The new `runs.id`.
  std::string  run_uid;                  ///< The caller's uid, echoed back.
  std::int64_t declared_snapshotted = 0; ///< Count of `kind='declared'` rows written.
};

/// @brief Insert a `runs` row and snapshot the plan's declared touches into
/// it, atomically.
/// @param conn An open, migrated database connection.
/// @param args The run's identity, config, and optional task filter.
/// @return The new id, the echoed uid, and the snapshot count; or
/// `runs_error::duplicate_run_uid` when the uid collides.
export auto start(db::connection& conn, const start_args& args) -> std::expected<start_result, runs_error>;

/// @brief Append a `run_events` row with a caller-supplied `seq`.
///
/// `bench event` uses this directly; `run event` calls `next_seq` first.
/// @param conn An open, migrated database connection.
/// @param run_id The owning run's autoincrement id.
/// @param seq The per-run ordinal; UNIQUE with `run_id`.
/// @param kind Opaque event kind.
/// @param payload Raw JSON blob, or unset.
/// @return The new `run_events.id`, or `runs_error::duplicate_seq` when the
/// seq is already taken for this run.
export auto event(db::connection& conn, std::int64_t run_id, std::int64_t seq, std::string_view kind,
                  std::optional<std::string_view> payload) -> std::expected<std::int64_t, runs_error>;

/// @brief The next free per-run ordinal: `coalesce(max(seq), 0) + 1`.
///
/// This is `run event`'s auto-increment. Oracle-captured: the first event on
/// a fresh run reports `"seq":1`.
/// @param conn An open, migrated database connection.
/// @param run_id The owning run's autoincrement id.
/// @return The next seq to use.
export auto next_seq(db::connection& conn, std::int64_t run_id) -> std::expected<std::int64_t, runs_error>;

/// @brief Insert a `run_touches` row, failing on a duplicate tuple.
///
/// A repeat (run, task, path, kind) trips the UNIQUE constraint and surfaces
/// as `runs_error::query_failed` — oracle-confirmed as an exit-1
/// `error: bench touch: QueryFailed` rather than a silent no-op.
/// @param conn An open, migrated database connection.
/// @param run_id The owning run.
/// @param task_id The touching task (plain integer, not an FK cascade).
/// @param path The repo-relative path.
/// @param kind Declared or actual.
/// @return The new `run_touches.id`.
export auto touch(db::connection& conn, std::int64_t run_id, std::int64_t task_id, std::string_view path, touch_kind kind)
    -> std::expected<std::int64_t, runs_error>;

/// @brief Insert a `run_touches` row idempotently (`insert or ignore`).
///
/// Identical to `touch` except a UNIQUE conflict is a silent success. This is
/// the primitive `planar.engine.runs.harvest::harvest` writes through (task
/// 6362).
/// @param conn An open, migrated database connection.
/// @param run_id The owning run.
/// @param task_id The touching task.
/// @param path The repo-relative path.
/// @param kind Declared or actual.
/// @return Success, including when the row already existed.
export auto touch_idempotent(db::connection& conn, std::int64_t run_id, std::int64_t task_id, std::string_view path,
                             touch_kind kind) -> std::expected<void, runs_error>;

/// @brief Set the run's terminal status and stamp `ended_at` to now.
///
/// Deliberately NOT guarded against re-finishing: a second `finish` rewrites
/// both columns. Oracle-confirmed — `bench finish r1 --status completed` then
/// `--status aborted` both exit 0 and the second wins, with a fresh
/// `ended_at`. Existence is checked first so a no-op UPDATE on a missing id
/// surfaces as `not_found` rather than a silent success.
/// @param conn An open, migrated database connection.
/// @param run_id The run to finish.
/// @param status The terminal status text (validated by the caller).
/// @return Success, or `runs_error::not_found`.
export auto finish(db::connection& conn, std::int64_t run_id, std::string_view status) -> std::expected<void, runs_error>;

/// @brief Read one run back by autoincrement id.
/// @param conn An open, migrated database connection.
/// @param id The `runs.id` to read.
/// @return The run, or `runs_error::not_found`.
export auto show(db::connection& conn, std::int64_t id) -> std::expected<run, runs_error>;

/// @brief Read one run back by its stable external `run_uid`.
/// @param conn An open, migrated database connection.
/// @param run_uid The external handle to look up.
/// @return The run, or `runs_error::not_found`.
export auto show_by_uid(db::connection& conn, std::string_view run_uid) -> std::expected<run, runs_error>;

/// @brief All journal rows for a run, ordered by `seq` ascending.
///
/// Ordering is by `seq`, NOT by insertion id — oracle-confirmed by inserting
/// seq 9 after seq 1 and 2 and seeing `[1] [2] [9]`.
/// @param conn An open, migrated database connection.
/// @param run_id The owning run.
/// @return The rows in seq order (empty when the run has no events).
export auto events(db::connection& conn, std::int64_t run_id) -> std::expected<std::vector<event_row>, runs_error>;

/// @brief All touch rows for a run, optionally filtered by kind, ordered by
/// `id` ascending (insertion order — NOT by path or task).
/// @param conn An open, migrated database connection.
/// @param run_id The owning run.
/// @param kind Restrict to one kind, or unset for all.
/// @return The rows in id order.
export auto touches(db::connection& conn, std::int64_t run_id, std::optional<touch_kind> kind)
    -> std::expected<std::vector<touch_row>, runs_error>;

/// @brief Mint a run uid the way `run start` does: `lower(hex(randomblob(16)))`.
///
/// The generator is SQLite's, not the process's, matching the claim-token
/// pattern in the agent-activity store. Oracle-captured shape: 32 lowercase
/// hex characters.
/// @param conn An open, migrated database connection.
/// @return A fresh 32-character lowercase hex uid.
export auto generate_run_uid(db::connection& conn) -> std::expected<std::string, runs_error>;

} // namespace planar::engine::runs::lifecycle
