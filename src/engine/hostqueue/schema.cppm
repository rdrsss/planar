/// @file schema.cppm
/// @brief `planar.engine.hostqueue.schema` — the queue tables' own
/// compatibility check inside `planar.db` (plan 1089, decision 1220, task
/// qp-queue-compat; tech spec 656 § Queue compatibility check).
///
/// The queue verbs, unlike every other `planar-agent` verb, accept a
/// `planar.db` that is AHEAD of their binary, because a long-lived submitter
/// keeps polling while a newer build migrates the shared file. What makes
/// that safe is not `schema_migrations` but the queue's own marker:
/// `queue_schema(version, compat, description)`, created by migration 00040
/// and given one row by every later migration that names a queue table. The
/// row with the highest `version` is authoritative; its `compat` is the
/// oldest queue version that may use `queue_entries` and `queue_history`.
///
/// `check_queue_schema` passes when:
///  1. `queue_schema`, `queue_entries` and `queue_history` exist;
///  2. the marker's highest row has `compat` <= `k_queue_schema_version`;
///  3. every column in `k_queue_entries_columns` and
///     `k_queue_history_columns` exists (the column guard).
///
/// The column lists are hand-maintained, never derived from SQL at run time.
/// Tests pin them to `pragma table_info` at head, pin every column a
/// hostqueue SQL literal names into them, pin `k_queue_schema_version` to the
/// highest `queue_schema.version` in the embedded chain, and fingerprint the
/// hostqueue SQL together with `k_queue_protocol` (schema.t.cpp).
///
/// A failed check means two different things depending on the version
/// handshake, and the two have different remedies, so they carry different
/// tags (`queue_schema_refusal_tag`): at an EQUAL `schema_migrations` version
/// the database carries a migration this build does not know under the same
/// number (`queue_schema_foreign`, which a newer build alone will not fix);
/// at an AHEAD version the queue tables changed beyond what this build
/// supports (`queue_schema_incompatible`, which a newer build fixes).
///
/// Every fallible entry point returns `std::expected`; nothing throws across
/// the module boundary.

module;

export module planar.engine.hostqueue.schema;

import std;
import planar.db;

namespace planar::engine::hostqueue {

/// @brief This binary's queue version: the highest `queue_schema.version` in
/// the migration chain it embeds. A test pins the two together, so a marker
/// migration that forgets to bump this fails the build's own suite rather
/// than making the binary refuse its own head database.
export constexpr std::uint32_t k_queue_schema_version = 1;

/// @brief Every `queue_entries` column this binary names, in table order.
/// Hand-maintained: a test asserts it equals `pragma table_info` at head.
export constexpr std::array<std::string_view, 25> k_queue_entries_columns{
    "seq",
    "state",
    "host_id",
    "pid",
    "pid_started",
    "child_pgid",
    "child_started",
    "parent_seq",
    "terminating_since_mono",
    "terminate_reason",
    "cancelled_by",
    "cwd",
    "argv",
    "label",
    "vendor",
    "role",
    "claim_token",
    "log_path",
    "enqueued_at",
    "started_at",
    "refreshed_mono",
    "deadline_mono",
    "wait_deadline_mono",
    "run_limit_ms",
    "wait_limit_ms",
};

/// @brief Every `queue_history` column this binary names, in table order.
/// Hand-maintained: a test asserts it equals `pragma table_info` at head.
export constexpr std::array<std::string_view, 21> k_queue_history_columns{
    "seq",         "outcome",    "exit_code", "signal",    "successor_seq", "cancelled_by", "nested",
    "parent_seq",  "cwd",        "argv",      "label",     "vendor",        "role",         "log_path",
    "enqueued_at", "started_at", "ended_at",  "waited_ms", "ran_ms",        "run_limit_ms", "wait_limit_ms",
};

/// @brief The queue's protocol constants, as `name=value` strings, fed into
/// the protocol fingerprint beside the hostqueue SQL.
///
/// Seven entries have a code counterpart and a test asserts each equals it
/// (`entry_states`, `terminate_reasons`, `outcomes`, `submitter_identity`,
/// `unknown_host`, `seq_floor`, `history_day_ms`). Four are
/// fingerprint-only LABELS (`freshness`, `running_live`, `slot_rule`,
/// `nested_rule`): prose naming a rule that lives in code or a comment,
/// which nothing can assert mechanically. They exist so that whoever changes
/// such a rule must also change its label here, which moves the fingerprint
/// and forces the re-pin-or-bump choice (tech spec 656 § Re-pinning).
export constexpr std::array<std::string_view, 11> k_queue_protocol{
    "entry_states=waiting,running",
    "terminate_reasons=timeout,cancelled",
    "outcomes=exited,signaled,timeout,cancelled,wait_timeout,not_started,abandoned",
    "freshness=abs(now_mono-refreshed_mono)<=stale_after",
    "running_live=submitter_passes||group_has_members",
    "submitter_identity=host_id,pid,pid_started",
    "unknown_host=unknown",
    "slot_rule=first_n_live_non_nested_by_seq",
    "nested_rule=same_host&&parent_running&&parent_live",
    "seq_floor=1000000",
    "history_day_ms=86400000",
};

/// @brief The value of the `k_queue_protocol` entry called `name`.
/// @param name The entry's name, the text before `=`.
/// @return The text after `=`, or `std::nullopt` when there is no such entry.
export auto protocol_value(std::string_view name) -> std::optional<std::string_view>;

/// @brief Which condition of the queue check failed.
export enum class queue_schema_failure : std::uint8_t {
  missing_table,       ///< `queue_schema`, `queue_entries` or `queue_history` does not exist.
  incompatible_marker, ///< The marker is empty, or its highest row's `compat` is above this binary's queue version.
  missing_column,      ///< A column this binary names is absent (the column guard).
  query_failed,        ///< A query against the store failed; `message` carries SQLite's reason.
};

/// @brief Why `check_queue_schema` refused, as one printable sentence plus
/// the structured parts a caller may render itself.
export struct queue_schema_error {
  queue_schema_failure         kind = queue_schema_failure::query_failed; ///< Which condition failed.
  std::string                  message;       ///< A complete description naming the table, column or versions involved.
  std::optional<std::uint32_t> store_version; ///< The marker's highest `version`, when it was read.
  std::optional<std::uint32_t> store_compat;  ///< That row's `compat`, when it was read.
};

/// @brief Runs the queue compatibility check against an open `planar.db`.
/// Reads only, so a read-only connection is enough; writes nothing.
/// @param conn The connection to inspect.
/// @return Success when this binary may use the queue tables; otherwise the
/// first failed condition (all missing columns are named together).
export auto check_queue_schema(db::connection& conn) -> std::expected<void, queue_schema_error>;

/// @brief The `--json` tag for a database whose version is ahead of this
/// binary and whose queue check fails.
export constexpr std::string_view k_tag_queue_schema_incompatible = "queue_schema_incompatible";

/// @brief The `--json` tag for a database at this binary's own version whose
/// queue check fails: two branches shipped different migrations under one
/// number.
export constexpr std::string_view k_tag_queue_schema_foreign = "queue_schema_foreign";

/// @brief The tag a queue verb refuses with when `check_queue_schema` fails,
/// chosen by the `schema_migrations` handshake. Neither tag is the exit-7
/// `schema_version_ahead`, because the remedies differ.
/// @param live The database's `schema_migrations` version.
/// @param embedded The highest version this binary embeds.
/// @return `queue_schema_foreign` when the two are equal,
/// `queue_schema_incompatible` when `live` is ahead, and `std::nullopt` when
/// `live` is behind (a behind database is refused before the queue check,
/// with `schema_version_behind`).
export auto queue_schema_refusal_tag(std::uint32_t live, std::uint32_t embedded) -> std::optional<std::string_view>;

} // namespace planar::engine::hostqueue
