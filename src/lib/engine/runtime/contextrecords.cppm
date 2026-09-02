/// @file contextrecords.cppm
/// @brief Run-scoped working-memory store.
///
/// Command handlers supply validated input;
/// this module owns every context_records SQL shape and preserves nullable
/// claim/provenance fields without leaking SQLite into layer 3.
module;
export module planar.engine.runtime.contextrecords;
import std;
import planar.db;
namespace planar::engine::runtime::contextrecords {

/// @brief A `context_records` row associated with a workflow run.
export struct record {
  std::int64_t                id         = 0; ///< Row id.
  std::int64_t                run_id     = 0; ///< Owning workflow run.
  std::int64_t                session_id = 0; ///< Session that produced the record.
  std::optional<std::int64_t> claim_id;       ///< Work claim that produced it, when applicable.
  std::string                 stage;          ///< Workflow stage at capture time.
  std::string                 kind;           ///< Record kind, such as `capsule`.
  std::string                 body;           ///< Captured working-memory text.
  std::string                 status;         ///< Lifecycle status (`active` until resolved).
  std::string                 created_at;     ///< Creation timestamp.
  std::optional<std::string>  compiled_from;  ///< Provenance reference, when supplied.
};

/// @brief Input for a context record derived from an existing work claim.
export struct add_input {
  std::string_view                token;         ///< Claim token identifying the source claim.
  std::string_view                kind;          ///< Record kind to persist.
  std::string_view                body;          ///< Captured working-memory text.
  std::optional<std::string_view> compiled_from; ///< Provenance reference, when supplied.
};

/// @brief Input for a compaction capsule associated with a workflow run.
export struct capsule_input {
  std::int64_t                    run_id;        ///< Workflow run to receive the capsule.
  std::string_view                stage;         ///< Workflow stage at capture time.
  std::string_view                body;          ///< Capsule text.
  std::optional<std::int64_t>     session_id;    ///< Producing session, or unset to create a compactor session.
  std::optional<std::string_view> compiled_from; ///< Provenance reference, when supplied.
};

/// @brief Create an active context record using the run, stage, and session
/// attached to a work claim.
/// @param c An open, migrated database connection.
/// @param i Claim token and record contents.
/// @return The inserted record, or a database error when the claim is absent,
/// lacks a run, or persistence fails.
export auto add_from_claim(db::connection& c, const add_input& i) -> std::expected<record, db::db_error>;

/// @brief Create an active `capsule` context record for a workflow run.
/// @param c An open, migrated database connection.
/// @param i Workflow run, stage, capsule text, and optional session/provenance.
/// @return The inserted record, or a database error when the workflow run is
/// absent or persistence fails.
export auto add_capsule(db::connection& c, const capsule_input& i) -> std::expected<record, db::db_error>;

/// @brief List records for a workflow run in ascending row-id order.
/// @param c An open, migrated database connection.
/// @param run Workflow run to search.
/// @param stage Optional exact stage filter.
/// @param status Optional exact status filter.
/// @param kind Optional exact kind filter.
/// @return Matching records, oldest first, or a database error.
export auto list(db::connection& c, std::int64_t run, std::optional<std::string_view> stage,
                 std::optional<std::string_view> status, std::optional<std::string_view> kind)
    -> std::expected<std::vector<record>, db::db_error>;

/// @brief Read one context record by id.
/// @param c An open, migrated database connection.
/// @param id Context record id.
/// @return The record, or a database error when it is absent or the query fails.
export auto get(db::connection& c, std::int64_t id) -> std::expected<record, db::db_error>;

/// @brief Change one active record to a supplied status.
/// @param c An open, migrated database connection.
/// @param id Context record id.
/// @param status Status to write.
/// @return The count of rows now having the supplied status, or a database error.
export auto resolve_one(db::connection& c, std::int64_t id, std::string_view status) -> std::expected<std::int64_t, db::db_error>;

/// @brief Change all active records for one workflow run and stage to a supplied status.
/// @param c An open, migrated database connection.
/// @param run Workflow run to update.
/// @param stage Exact stage to update.
/// @param status Status to write.
/// @return The number of records that were active before the update, or a database error.
export auto resolve_stage(db::connection& c, std::int64_t run, std::string_view stage, std::string_view status)
    -> std::expected<std::int64_t, db::db_error>;
} // namespace planar::engine::runtime::contextrecords
