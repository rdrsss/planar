/// Run-scoped working-memory store. Command handlers supply validated input;
/// this module owns every context_records SQL shape and preserves nullable
/// claim/provenance fields without leaking SQLite into layer 3.
module;
export module planar.engine.runtime.contextrecords;
import std;
import planar.db;
namespace planar::engine::runtime::contextrecords {
export struct record {
  std::int64_t                id = 0, run_id = 0, session_id = 0;
  std::optional<std::int64_t> claim_id;
  std::string                 stage, kind, body, status, created_at;
  std::optional<std::string>  compiled_from;
};
export struct add_input {
  std::string_view                token, kind, body;
  std::optional<std::string_view> compiled_from;
};
export struct capsule_input {
  std::int64_t                    run_id;
  std::string_view                stage, body;
  std::optional<std::int64_t>     session_id;
  std::optional<std::string_view> compiled_from;
};
export auto add_from_claim(db::connection&, const add_input&) -> std::expected<record, db::db_error>;
export auto add_capsule(db::connection&, const capsule_input&) -> std::expected<record, db::db_error>;
export auto list(db::connection&, std::int64_t run, std::optional<std::string_view> stage, std::optional<std::string_view> status,
                 std::optional<std::string_view> kind) -> std::expected<std::vector<record>, db::db_error>;
export auto get(db::connection&, std::int64_t id) -> std::expected<record, db::db_error>;
export auto resolve_one(db::connection&, std::int64_t id, std::string_view status) -> std::expected<std::int64_t, db::db_error>;
export auto resolve_stage(db::connection&, std::int64_t run, std::string_view stage, std::string_view status)
    -> std::expected<std::int64_t, db::db_error>;
} // namespace planar::engine::runtime::contextrecords
