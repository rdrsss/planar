/// @file link.cppm
/// @brief `planar.engine.external.link` — the `external_links` row: the
/// local-entity ↔ external-ticket binding (plan 996, task 6106).
///
/// Behavior-preserving port (D2) of the create/show/delete third of
/// zig/src/engine/external/link.zig.
///
/// ## Why this bucket exists at all, and why it is this small
///
/// `unlink` is one of the leaves task 6105 recorded as blocked on layer 3,
/// and it is the only one of them whose layer-2 dependency was ALSO
/// missing: the C++ tree had no `engine_external` bucket of any kind. So
/// standing one up was a precondition, not a refactor.
///
/// It is scoped to what `planar unlink <link-id>` composes and nothing
/// more. `unlink` reads a link (to fail precisely on a bad id) and deletes
/// it; `create` is here because `show`/`delete` cannot be tested against a
/// real row without it, and hand-rolled `INSERT`s in a test file are a
/// second, undocumented definition of the table's defaults. Everything
/// else in the Zig original is DEFERRED WITH ITS VERB rather than ported
/// speculatively:
///
///   - `list` / `linksForEntity` / `pullable` / `pushable` — LANDED, task
///     6041, with `planar.engine.external.sync`.
///   - `updateSyncState` / `updateSyncDirection` and the `sync_events`
///     audit row they write — LANDED, task 6041.
///   - `baseline_title` / `baseline_status` (columns 13-14, added by a
///     later migration and absent from the Zig `ExtLink` struct entirely)
///     — LANDED as `load_baseline` / `store_baseline` on this module, task
///     6041. They stay OFF `ext_link` for the reason the Zig struct leaves
///     them off: no SELECT in this file reads them, and the conflict engine
///     wants them separately anyway.
///
/// Nothing from `link.zig` is deferred any more.
///
/// ## Scope: this table has no scope column, and no guard runs on it
///
/// `external_links` carries no `scope_kind`/`scope_id`. `planar unlink`
/// declares `--scope` and its own handler discards it (the Zig handler's
/// `_ = args.scope;` is literal), which matches the documented rule that
/// link verbs are UNGUARDED BY DESIGN (docs/concepts.md §
/// cross-scope-guard). Nothing in this module names the guard.
///
/// ## `delete` reports not-found, and that is load-bearing
///
/// The Zig original checks `changes() == 0` after the `DELETE` and raises
/// `NotFound`. `planar.db`'s connection exposes no `sqlite3_changes`
/// accessor, so this port uses `delete ... returning id` — a row comes
/// back exactly when one matched. Same signal, same distinction, and the
/// same shape `engine_models`' `update` already uses for its own
/// `changes() != 1` check.
module;

export module planar.engine.external.link;

import std;
import planar.db;

namespace planar::engine::external::link {

/// @brief The local entity a link points at. Mirrors the
/// `external_links.entity_kind` CHECK constraint exactly.
export enum class external_entity_kind : std::uint8_t {
  plan,
  task,
  question,
  test_scenario,
  artifact,
  decision,
  session,
};

/// @brief Parse an `entity_kind` column value.
/// @param s The stored text.
/// @return The kind, or unset when `s` is not one of the seven.
export auto external_entity_kind_from_text(std::string_view s) -> std::optional<external_entity_kind>;

/// @brief The stored text for a kind.
/// @param k The kind.
/// @return The column value.
export auto external_entity_kind_to_text(external_entity_kind k) -> std::string_view;

/// @brief What the link asserts about the two sides. Mirrors the
/// `link_role` CHECK constraint.
export enum class link_role : std::uint8_t {
  mirror,
  parent,
  child,
  reference,
};

/// @brief Parse a `link_role` column value.
/// @param s The stored text.
/// @return The role, or unset when unrecognized.
export auto link_role_from_text(std::string_view s) -> std::optional<link_role>;

/// @brief The stored text for a role.
/// @param r The role.
/// @return The column value.
export auto link_role_to_text(link_role r) -> std::string_view;

/// @brief Which way sync may flow. Mirrors the `sync_direction` CHECK
/// constraint — note the stored values are HYPHENATED (`read-only`), which
/// is why the enumerator names and the column values differ.
export enum class sync_direction : std::uint8_t {
  read_only,  ///< Stored as `read-only`.
  write_back, ///< Stored as `write-back`.
  two_way,    ///< Stored as `two-way`.
};

/// @brief Parse a `sync_direction` column value.
/// @param s The stored text, hyphenated.
/// @return The direction, or unset when unrecognized.
export auto sync_direction_from_text(std::string_view s) -> std::optional<sync_direction>;

/// @brief The stored, hyphenated text for a direction.
/// @param d The direction.
/// @return The column value.
export auto sync_direction_to_text(sync_direction d) -> std::string_view;

/// @brief The outcome of the last sync attempt. Mirrors the
/// `last_sync_status` CHECK constraint.
export enum class sync_status : std::uint8_t {
  ok,
  conflict,
  error,
  never,
};

/// @brief Parse a `last_sync_status` column value.
/// @param s The stored text.
/// @return The status, or unset when unrecognized.
export auto sync_status_from_text(std::string_view s) -> std::optional<sync_status>;

/// @brief The stored text for a status.
/// @param s The status.
/// @return The column value.
export auto sync_status_to_text(sync_status s) -> std::string_view;

/// @brief One `external_links` row.
///
/// `baseline_title` / `baseline_status` are deliberately absent — see this
/// file's header.
export struct ext_link {
  std::int64_t               id          = 0;                          ///< The row id.
  external_entity_kind       entity_kind = external_entity_kind::plan; ///< Which local table.
  std::int64_t               entity_id   = 0;                          ///< The local row id.
  std::int64_t               system_id   = 0;                          ///< The `external_systems` row.
  std::string                external_id;                              ///< The external ticket's id.
  std::optional<std::string> external_url;                             ///< The ticket URL, when known.
  link_role                  role      = link_role::mirror;            ///< The link role.
  sync_direction             direction = sync_direction::two_way;      ///< The permitted sync flow.
  std::optional<std::string> last_synced_at;                           ///< Last sync timestamp, when ever synced.
  sync_status                last_sync_status = sync_status::never;    ///< The last sync outcome.
  std::optional<std::string> config_json;                              ///< Per-link config blob.
  std::string                created_at;                               ///< Row creation timestamp.
};

/// @brief The arguments `create` takes. Defaults reproduce the Zig
/// original's `CreateArgs` defaults, which are NOT all the same as the
/// column defaults: the schema defaults `link_role` to `mirror` and
/// `sync_direction` to `two-way`, and so does this struct, but the CLI's
/// own `link` verb documents different defaults again (`reference` /
/// `read-only`) and passes them explicitly. This struct is the engine
/// default, not the CLI default.
export struct create_args {
  external_entity_kind       entity_kind = external_entity_kind::plan; ///< Which local table.
  std::int64_t               entity_id   = 0;                          ///< The local row id.
  std::int64_t               system_id   = 0;                          ///< The `external_systems` row.
  std::string_view           external_id;                              ///< The external ticket's id.
  std::optional<std::string> external_url;                             ///< The ticket URL, when known.
  link_role                  role           = link_role::mirror;       ///< The link role.
  sync_direction             direction      = sync_direction::two_way; ///< The permitted sync flow.
  sync_status                initial_status = sync_status::never;      ///< The initial sync outcome.
  std::optional<std::string> config_json;                              ///< Per-link config blob.
};

/// @brief Why a link operation failed. Mirrors the Zig original's `Error`
/// set one-for-one.
export enum class link_error : std::uint8_t {
  not_found,    ///< No row with that id.
  link_exists,  ///< The `(entity_kind, entity_id, system_id, external_id)` UNIQUE was violated.
  query_failed, ///< SQL failure.
};

/// @brief Insert a link and read it back.
/// @param conn An open, migrated database connection.
/// @param args The row to write.
/// @return The stored row, or the failure.
export auto create(db::connection& conn, const create_args& args) -> std::expected<ext_link, link_error>;

/// @brief Read one link by id.
/// @param conn An open, migrated database connection.
/// @param id The link id.
/// @return The stored row, `link_error::not_found` when there is none.
export auto show(db::connection& conn, std::int64_t id) -> std::expected<ext_link, link_error>;

/// @brief Delete one link by id.
///
/// The schema detaches dependent `sync_events` rows by setting `link_id`
/// to NULL (`on delete set null`) rather than cascade-deleting them, so
/// the event history survives the link. This function does not do that
/// itself — the foreign key does.
/// @param conn An open, migrated database connection.
/// @param id The link id.
/// @return Success, or `link_error::not_found` when no row matched.
export auto remove(db::connection& conn, std::int64_t id) -> std::expected<void, link_error>;

/// @brief Which links `list` should return. Every field is a conjunctive
/// filter; an unset field constrains nothing.
export struct list_filter {
  std::optional<external_entity_kind> entity_kind; ///< Restrict to one local table.
  std::optional<std::int64_t>         entity_id;   ///< Restrict to one local row.
  std::optional<std::int64_t>         system_id;   ///< Restrict to one `external_systems` row.
  /// @brief Restrict by the system's SLUG rather than its id. Supplying this
  /// makes the query JOIN `external_systems`; the Zig original adds the join
  /// only when this field is set, and that is preserved because the join is
  /// observable in the row set when a link points at a deleted system.
  std::optional<std::string> system_slug;
};

/// @brief The `baseline_title` / `baseline_status` pair: what BOTH sides
/// agreed on at the last successful sync.
///
/// This is the whole basis of field-level conflict detection. A field is
/// "changed" on a side when that side differs from the baseline, and a field
/// CONFLICTS only when both sides changed AND now differ from each other. No
/// baseline means no conflict is possible — the first pull just applies.
export struct baseline {
  std::optional<std::string> title;  ///< The agreed title, unset before the first sync.
  std::optional<std::string> status; ///< The agreed status, unset before the first sync.

  /// @brief Whether a usable baseline exists.
  ///
  /// BOTH halves must be present, matching the Zig original's
  /// `present()`. A half-written baseline is treated as none at all.
  /// @return `true` when both fields are set.
  [[nodiscard]] auto present() const -> bool {
    return title.has_value() && status.has_value();
  }
};

/// @brief Links matching `filter`, ordered by id.
/// @param conn An open, migrated database connection.
/// @param filter Which links to return.
/// @return The rows, or the failure.
export auto list(db::connection& conn, const list_filter& filter) -> std::expected<std::vector<ext_link>, link_error>;

/// @brief Every link on one local entity, ordered by id.
/// @param conn An open, migrated database connection.
/// @param entity_kind Which local table.
/// @param entity_id The local row id.
/// @return The rows, or the failure.
export auto links_for_entity(db::connection& conn, external_entity_kind entity_kind, std::int64_t entity_id)
    -> std::expected<std::vector<ext_link>, link_error>;

/// @brief Every link a pull may read: `read-only` or `two-way`.
/// @param conn An open, migrated database connection.
/// @return The rows, or the failure.
export auto all_pullable(db::connection& conn) -> std::expected<std::vector<ext_link>, link_error>;

/// @brief Every link a push may write: `write-back` or `two-way`.
/// @param conn An open, migrated database connection.
/// @return The rows, or the failure.
export auto all_pushable(db::connection& conn) -> std::expected<std::vector<ext_link>, link_error>;

/// @brief Stamp `last_synced_at` to now and set `last_sync_status`.
///
/// `last_synced_at` is written on EVERY outcome, including `error` — the
/// column records when the last ATTEMPT happened, not when the last success
/// did.
/// @param conn An open, migrated database connection.
/// @param link_id The link.
/// @param status The outcome to record.
/// @return Success, or `link_error::not_found` when no row matched.
export auto update_sync_state(db::connection& conn, std::int64_t link_id, sync_status status) -> std::expected<void, link_error>;

/// @brief Change a link's `sync_direction`, recording the change as a
/// `sync_events` row, in one transaction.
///
/// The event row is `direction='push'`, `outcome='ok'`,
/// `fields_changed='["sync_direction"]'` and a `detail` JSON object
/// `{"old_direction":..,"new_direction":..,"operation":"sync_direction_update"}`.
/// The Zig original builds that detail by RAW INTERPOLATION rather than
/// through an escaper, which is safe there and here only because both
/// interpolated values are enum texts from a closed set — this port keeps
/// the same shape rather than "fixing" it into a byte divergence.
/// @param conn An open, migrated database connection.
/// @param link_id The link.
/// @param direction The new direction.
/// @return The PRIOR direction, or the failure.
export auto update_sync_direction(db::connection& conn, std::int64_t link_id, sync_direction direction)
    -> std::expected<sync_direction, link_error>;

/// @brief Read a link's stored baseline.
/// @param conn An open, migrated database connection.
/// @param link_id The link.
/// @return The baseline (either half may be unset), or
/// `link_error::not_found` when the link does not exist.
export auto load_baseline(db::connection& conn, std::int64_t link_id) -> std::expected<baseline, link_error>;

/// @brief Overwrite a link's stored baseline.
/// @param conn An open, migrated database connection.
/// @param link_id The link.
/// @param title The agreed title.
/// @param status_value The agreed status.
/// @return Success, or the failure.
export auto store_baseline(db::connection& conn, std::int64_t link_id, std::string_view title, std::string_view status_value)
    -> std::expected<void, link_error>;

/// @brief Read the `mirror` link's external id for one entity on one system.
///
/// The oracle spells this `extsync.propagate.loadExistingMirror`. It lands
/// here rather than in `planar.engine.extsync` because that bucket carries no
/// `db` edge by design and this function is nothing but SQL against
/// `external_links` — the `plan descendants` split (task 6298) again. See
/// `planar.engine.extsync.propagate`'s header for why that file divides.
///
/// ## The empty string is a real answer, not a failure
///
/// The oracle returns an ALLOCATED EMPTY STRING when no row matches, and its
/// caller (`ext propagate-one`) branches on `.len > 0`. The `coalesce` means
/// a matching row whose `external_id` is SQL NULL is likewise the empty
/// string. So "no mirror link" and "a mirror link with no id" are
/// deliberately indistinguishable to the caller, and `propagate-one` treats
/// both as "not yet propagated". Reproduced rather than tightened.
///
/// Filters on `link_role = 'mirror'`: a `parent` or `child` row for the same
/// entity does NOT suppress propagation.
///
/// @param conn An open, migrated database connection.
/// @param entity_kind The entity kind TEXT, as stored.
/// @param entity_id The entity id.
/// @param system_id The registered system.
/// @return The external id, the empty string when there is no mirror row, or
/// the query failure.
export auto load_existing_mirror(db::connection& conn, std::string_view entity_kind, std::int64_t entity_id,
                                 std::int64_t system_id) -> std::expected<std::string, link_error>;

/// @brief Insert a `mirror` link AND its `push`/`ok` sync event in one
/// transaction.
///
/// The oracle spells this `extsync.parent_issue.recordLink`, and it is the
/// ONLY thing `workbench publish` needs from that 1205-line file.
///
/// ## This is NOT `create` with different arguments
///
/// `create` writes `external_links` and nothing else. This writes a
/// `sync_events` row too — `(link_id, 'push', 'ok')`, with `fields_changed`
/// and `detail` left NULL — and wraps both in `begin immediate` so a
/// published mirror can never exist without its audit event. A caller that
/// substituted `create` would leave the sync history silently short one row,
/// which no state differential over `external_links` alone would catch.
///
/// `link_role` is always `mirror` and `last_sync_status` always `ok`; neither
/// is a parameter, matching the oracle's hardcoded literals.
///
/// @param conn An open, migrated database connection.
/// @param entity_kind The entity kind TEXT, as stored.
/// @param entity_id The entity id.
/// @param system_id The registered system.
/// @param external_id The id the provider assigned.
/// @param external_url The provider URL; an EMPTY view is stored as SQL NULL,
/// matching the oracle's explicit null branch.
/// @param direction The sync direction to record.
/// @return The new link's id, or the failure.
export auto record_mirror_link(db::connection& conn, std::string_view entity_kind, std::int64_t entity_id, std::int64_t system_id,
                               std::string_view external_id, std::string_view external_url, sync_direction direction)
    -> std::expected<std::int64_t, link_error>;

} // namespace planar::engine::external::link
