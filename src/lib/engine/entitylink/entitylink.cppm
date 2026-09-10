/// @file entitylink.cppm
/// @brief `planar.engine.entitylink` — generic cross-cutting relationships
/// between any two Planar entities (`entity_links`), plus the path-level
/// "touches" surface (`task_touch_paths`) (plan 996, task cpp-entity-links).
///
/// Behavior-preserving port (D2) of a SUBSET of
/// zig/src/engine/entitylink.zig (`EntityKind`, `Relationship`, `ParsedRef`,
/// `parseRef`, `add`, `remove`, `show`, `list`) plus the
/// `task_touch_paths` half of zig/src/engine/planning/task.zig
/// (`addTouchPath`, `removeTouchPath`, `touchedPaths`, `touchedRepoIDs`).
/// See this bucket's CMakeLists.txt for why both tables live in one module
/// and what is deliberately NOT ported this cycle.
///
/// Link verbs are UNGUARDED BY DESIGN (docs/concepts.md §
/// cross-scope-guard) — `add`/`list` never consult
/// `planar.engine.identity.scope`'s cross-scope guard, and this module does
/// not even depend on `engine_identity` (see CMakeLists.txt). The `scope`
/// field on `entity_link_add_args`/`entity_link_list_filter` is accepted
/// for forward API compatibility with the Zig original only: a non-null
/// value returns `entity_link_error::unsupported_scope` rather than being
/// resolved (matches zig's M3 forward-compat note verbatim — `.global`
/// (null) is the only value that resolves today).
///
/// D19 (decision 945) consideration: this module needed nothing that
/// `engine_planning` or `engine_identity` already provide (no scope
/// resolution, no task/plan CRUD) — `tableFor`'s existence-check query
/// against `tasks`/`plans`/etc. is a bare `select 1 from <table> where id =
/// ?`, not a call into either bucket's typed CRUD surface, so there was
/// nothing to extract to layer 1. If a future cycle finds one of THIS
/// module's primitives (e.g. `parse_ref`) genuinely needed by
/// `engine_planning`/`engine_identity`, that is the trigger to extract it
/// to layer 1 rather than adding a sideways edge — not yet needed today.
module;

export module planar.engine.entitylink;

import std;
import planar.db;
import planar.json_text;
import planar.policy;

namespace planar::engine::entitylink {

/// @brief Every entity kind accepted by the `entity_links` CHECK
/// constraint (migration 00012 widened this to include `annotation`).
/// Mirrors zig's entitylink.zig `EntityKind`.
export enum class entity_kind : std::uint8_t {
  plan,
  plan_step,
  task,
  question,
  test_scenario,
  artifact,
  decision,
  session,
  repo,
  annotation,
};

/// @brief Parse a `from_kind`/`to_kind` column value / ref-string kind
/// segment.
/// @param s The raw text to parse.
/// @return The parsed kind, or unset for an unrecognized string.
export auto entity_kind_from_text(std::string_view s) -> std::optional<entity_kind>;

/// @brief Render `k` as the wire/column text form.
/// @param k The kind to render.
/// @return The wire/column text form.
export auto entity_kind_to_text(entity_kind k) -> std::string_view;

/// @brief Every relationship value accepted by the `entity_links` CHECK
/// constraint (migration 00033 renamed the stored `blocks` value to
/// `depends-on`; that migration is over the SAME table this module reads —
/// `depends-on` is the only spelling this module ever writes or accepts).
/// Mirrors zig's entitylink.zig `Relationship`.
export enum class relationship : std::uint8_t {
  derives_from,
  depends_on,
  addresses,
  verifies,
  cites,
  supersedes,
  touches,
};

/// @brief Parse a `relationship` column value.
///
/// `"blocks"` is the pre-migration-00033 spelling for `depends_on` and is
/// deliberately NOT accepted as an alias here, mirroring zig's own refusal
/// (entitylink.zig `Relationship.fromText`): `A --blocks--> B` stored "A
/// depends on B", so silently mapping the old word back in would restore
/// exactly the direction-inversion migration 00033 exists to fix.
/// @param s The raw text to parse.
/// @return The parsed relationship, or unset for an unrecognized (or
/// `"blocks"`) string.
export auto relationship_from_text(std::string_view s) -> std::optional<relationship>;

/// @brief Render `r` as the wire/column text form.
/// @param r The relationship to render.
/// @return The wire/column text form (e.g. `"depends-on"`).
export auto relationship_to_text(relationship r) -> std::string_view;

/// @brief One row from the `entity_links` table. Mirrors zig's
/// entitylink.zig `EntityLink`.
export struct entity_link {
  std::int64_t id;            ///< The row's id.
  entity_kind  from_kind;     ///< The link's source entity kind.
  std::int64_t from_id;       ///< The link's source entity row id.
  entity_kind  to_kind;       ///< The link's target entity kind.
  std::int64_t to_id;         ///< The link's target entity row id.
  relationship relationship_; ///< The typed relationship between the two.
  std::string  created_at;    ///< Row creation timestamp.
};

/// @brief Arguments to `add`. Mirrors zig's entitylink.zig `AddArgs`.
export struct entity_link_add_args {
  entity_kind  from_kind;     ///< The link's source entity kind.
  std::int64_t from_id;       ///< The link's source entity row id.
  entity_kind  to_kind;       ///< The link's target entity kind.
  std::int64_t to_id;         ///< The link's target entity row id.
  relationship relationship_; ///< The typed relationship between the two.
  /// Present for API parity with zig's `AddArgs.skip_scope_check` (the
  /// legacy Go `SkipScopeCheck` escape hatch). This module never consults
  /// a guard regardless of this flag's value — link verbs are unguarded by
  /// design (see this file's header comment).
  bool skip_scope_check = false;
  /// Accepted for forward compatibility only; a non-null value returns
  /// `entity_link_error::unsupported_scope` (only `.global` / unset
  /// resolves today — see this file's header comment).
  std::optional<std::string> scope;
};

/// @brief Filter for `list`. Mirrors zig's entitylink.zig `ListFilter`.
export struct entity_link_list_filter {
  std::optional<entity_kind>  from_kind;     ///< Match only this source kind, when set.
  std::optional<std::int64_t> from_id;       ///< Match only this source id, when set.
  std::optional<entity_kind>  to_kind;       ///< Match only this target kind, when set.
  std::optional<std::int64_t> to_id;         ///< Match only this target id, when set.
  std::optional<relationship> relationship_; ///< Match only this relationship, when set.
  /// Accepted for forward compatibility only; a non-null value returns
  /// `entity_link_error::unsupported_scope` (see `entity_link_add_args::scope`).
  std::optional<std::string> scope;
};

/// @brief Error surface for every fallible operation in this module.
export enum class entity_link_error : std::uint8_t {
  not_found,
  link_exists,
  unsupported_scope,
  invalid_ref,
  /// An endpoint references an entity id that does not exist. `add`
  /// carries no detail on which side failed — see `missing_endpoint`
  /// below, exposed separately so a future `cmd/`-layer handler can name
  /// the offending ref (mirrors zig's `missingEndpoint`).
  endpoint_not_found,
  query_failed,
  /// The row's own write succeeded but its `audit_log` companion did not.
  /// Mirrors zig, where `add`/`remove` `try policy.audit.record(...)` and
  /// let `policy.audit.Error.WriteFailed` propagate out of the engine's
  /// error set (task 6193).
  audit_write_failed,
};

/// @brief Which endpoint of a link failed the existence check. Mirrors
/// zig's entitylink.zig `MissingEndpoint`.
export enum class missing_endpoint : std::uint8_t { from, to };

/// @brief Decoded form of a `"kind:id-or-slug"` reference string, produced
/// by `parse_ref`. Mirrors zig's entitylink.zig `ParsedRef`.
export struct parsed_ref {
  /// @brief `id` form: the ref text after the colon parsed as a positive
  /// integer.
  struct id_ref {
    entity_kind  kind; ///< The reference's entity kind (before the colon).
    std::int64_t id;   ///< The parsed positive integer id (after the colon).
  };
  /// @brief `slug` form: the ref text after the colon did not parse as a
  /// positive integer, so it is a slug the caller must resolve per-table.
  struct slug_ref {
    entity_kind kind; ///< The reference's entity kind (before the colon).
    std::string slug; ///< The slug text (after the colon), unresolved.
  };
  std::variant<id_ref, slug_ref> value; ///< The decoded `id_ref` or `slug_ref` form.
};

/// @brief Decode a `"kind:id-or-slug"` reference string.
///
/// If the text after the last colon is a pure positive integer, yields
/// `parsed_ref::id_ref`; otherwise yields `parsed_ref::slug_ref`. There is
/// no generic slug-to-id resolver here — per-table lookups are preferred
/// (matches zig's own documented choice: "per-table lookups are preferred
/// because the entity-specific handler already knows the table").
/// @param s The `"kind:id-or-slug"` string to decode.
/// @return The decoded reference, or `entity_link_error::invalid_ref` for
/// malformed input (no colon, empty kind, empty id-or-slug, unknown kind,
/// non-positive integer id).
export auto parse_ref(std::string_view s) -> std::expected<parsed_ref, entity_link_error>;

/// @brief Check both endpoints of `args` for existence, reporting which
/// side is missing first (`from` is checked before `to`). Exposed so a
/// future `cmd/`-layer handler can name the offending ref in its error
/// rather than guessing, mirroring zig's `missingEndpoint`.
///
/// `entity_links` carries no FK constraint (the column pair is polymorphic
/// — SQLite cannot express a conditional reference), so existence is a
/// write-time check performed here, not a schema-enforced one. A query
/// failure reports "exists" (returns unset) rather than "missing": this
/// check must never turn a transient DB problem into a refusal to record a
/// legitimate link, matching zig's `endpointExists` fail-open comment.
/// @param conn An open, migrated database connection.
/// @param args The link arguments to check.
/// @return The first missing side, or unset when both endpoints exist.
export auto missing_endpoint_of(db::connection& conn, const entity_link_add_args& args) -> std::optional<missing_endpoint>;

/// @brief Insert a new `entity_links` row.
///
/// This is NOT a scope check — link verbs stay deliberately unguarded so
/// polyrepo edges can cross scopes (docs/concepts.md § cross-scope-guard);
/// a legitimate cross-scope link still succeeds, because "does this entity
/// exist" and "is it in my scope" are different questions. See
/// `missing_endpoint_of`'s doc comment for what IS checked.
/// @param conn An open, migrated database connection.
/// @param args The link to create.
/// @return The created row, or `entity_link_error::link_exists` if the
/// `(from, to, relationship)` tuple is already recorded,
/// `entity_link_error::unsupported_scope` if `args.scope` is non-null,
/// `entity_link_error::endpoint_not_found` if either endpoint does not
/// exist, or `entity_link_error::query_failed`.
export auto add(db::connection& conn, const entity_link_add_args& args) -> std::expected<entity_link, entity_link_error>;

/// @brief Delete an `entity_links` row by primary key.
/// @param conn An open, migrated database connection.
/// @param id The link's row id.
/// @return Success, or `entity_link_error::not_found` when the row does
/// not exist, or `entity_link_error::query_failed`.
export auto remove(db::connection& conn, std::int64_t id) -> std::expected<void, entity_link_error>;

/// @brief Look up an `entity_links` row by primary key.
/// @param conn An open, migrated database connection.
/// @param id The link's row id.
/// @return The row, or `entity_link_error::not_found`, or
/// `entity_link_error::query_failed`.
export auto show(db::connection& conn, std::int64_t id) -> std::expected<entity_link, entity_link_error>;

/// @brief List `entity_links` rows matching `filter`, ordered by id.
/// Filters are ANDed; an empty filter returns all rows. Never returns an
/// error for "no matches" — an empty result is an empty vector.
/// @param conn An open, migrated database connection.
/// @param filter The (all-optional) filters to apply.
/// @return The matching rows, or `entity_link_error::unsupported_scope` if
/// `filter.scope` is non-null, or `entity_link_error::query_failed`.
export auto list(db::connection& conn, const entity_link_list_filter& filter)
    -> std::expected<std::vector<entity_link>, entity_link_error>;

// ===========================================================================
// audit trail — the READ half of the `policy.audit` rows `add`/`remove` write
// ===========================================================================

/// @brief One `audit_log` row as returned by `trail`. Mirrors zig's
/// entitylink.zig `AuditRow`, INCLUDING field order — `render_trail_json`
/// serializes in this order and the oracle's bytes depend on it.
///
/// `actor`, `scope` and `summary` are genuinely NULLABLE columns, not
/// empty-string sentinels: `add`/`remove` write all three unset, and the
/// oracle renders them as JSON `null` and as the text literal `(none)`
/// (for `actor`). An `std::optional` rather than a `std::string` is what
/// keeps SQL NULL distinguishable from `''` here.
export struct audit_row {
  std::int64_t               id;          ///< The `audit_log` row's id.
  std::string                verb;        ///< `"link"`, `"unlink"`, ...
  std::string                entity_kind; ///< Always `"entity_link"` for these rows.
  std::int64_t               entity_id;   ///< The `entity_links.id` the row is about.
  std::optional<std::string> actor;       ///< Who did it; NULL on the CLI path today.
  std::optional<std::string> scope;       ///< The write scope; NULL on the CLI path today.
  std::optional<std::string> summary;     ///< One-line summary; NULL for these verbs.
  std::string                recorded_at; ///< The stamp.
};

/// @brief Every `audit_log` row for one `entity_links` id, ordered by id.
///
/// The link's existence is verified FIRST, so a missing link is
/// `not_found` rather than an empty trail — the two are different answers
/// and the oracle distinguishes them (`links trail 999` refuses at exit 1;
/// `links trail 2` on a link with no audit rows succeeds at exit 0).
///
/// Note the rows OUTLIVE the link: `remove` deletes the `entity_links` row
/// but leaves both audit rows behind, so a removed link's trail is
/// unreachable through this function even though the rows still exist.
/// That is the oracle's behaviour, reproduced rather than fixed (D2).
/// @param conn An open, migrated database connection.
/// @param id The `entity_links` row id.
/// @return The rows (possibly empty), `entity_link_error::not_found` when
/// no such link exists, or `entity_link_error::query_failed`.
export auto trail(db::connection& conn, std::int64_t id) -> std::expected<std::vector<audit_row>, entity_link_error>;

// ===========================================================================
// Renderers (D-renderers-in-layer-2)
// ===========================================================================

/// @brief One row of `links list` output: the link plus which END of it the
/// listed subject sits on.
///
/// `links list <ref>` is an OR over both endpoint columns, so a row can
/// arrive from either side and the `direction` column is not a property of
/// the link — it is a property of the link RELATIVE to the subject. The
/// renderer cannot recompute it (a task linked to itself is on both ends),
/// so the caller states it.
export struct directed_link {
  entity_link link;     ///< The `entity_links` row.
  bool        outbound; ///< True when the subject is the link's SOURCE (`from`).
};

/// @brief Merge the two half-queries `links list` issues into one ordered,
/// de-duplicated listing.
///
/// The oracle runs `list` twice — once filtered on the from-side, once on
/// the to-side — then emits every from-side row followed by the to-side
/// rows whose id did not already appear. A self-link appears in BOTH
/// halves and must be listed ONCE, as `from`; that de-duplication is the
/// only reason this is a function rather than two loops at the call site.
/// @param from_links Rows where the subject is the source.
/// @param to_links Rows where the subject is the target.
/// @return From-side rows in order, then the unseen to-side rows in order.
export auto merge_directed(std::span<const entity_link> from_links, std::span<const entity_link> to_links)
    -> std::vector<directed_link>;

/// @brief `links list <ref>` text output — the complete payload, trailing
/// newline included.
///
/// The empty case is NOT an empty string: it is the line `no links for
/// <kind>:<id>`, which is why the subject is a parameter even though every
/// non-empty row already names its peer.
///
/// The id column carries a FORCED `+` SIGN (`+2`, `+1234567`) — captured
/// from the oracle, where zig's `{d:<6}` renders one. It is left-aligned in
/// width 6 and OVERFLOWS rather than truncating past five digits. Do not
/// "clean up" the sign: it is the oracle's bytes.
/// @param rows The merged listing.
/// @param subject_kind The listed entity's kind.
/// @param subject_id The listed entity's id.
/// @return The complete stdout payload.
export auto render_link_list_text(std::span<const directed_link> rows, entity_kind subject_kind, std::int64_t subject_id)
    -> std::string;

/// @brief `links list <ref> --json` output — NDJSON, one object per line,
/// EACH line's newline included.
///
/// This renderer OWNS its terminators, unlike `planar.engine.planning`'s
/// `render_list_json` (a fragment the caller terminates). The reason is the
/// empty case: an empty `--json` listing is ZERO BYTES, and a fragment
/// contract would force the caller to write a bare `"\n"`. Write the result
/// verbatim; append nothing.
///
/// TASK 6270, and why nothing changed here. That row was filed as "`links
/// list --json` emits zero bytes for an entity that HAS an `entity_links`
/// row", ranked above the malformed-JSON defects because it fails SILENTLY
/// -- a consumer cannot tell "no links" from a broken command. The headline
/// was already resolved by the time it was triaged: a populated listing
/// emits one NDJSON object per row. What survived was the EMPTY case, which
/// is this contract, and which is the same shape-split family as 6257
/// (`scope suggest`) and 6326 (`assoc detect`).
///
/// The three verbs disagreed with each other AND with themselves, so one
/// rule had to be chosen for all three. It is the one this renderer already
/// implemented -- NDJSON with N lines for N results, N allowed to be zero
/// -- because it leaves every POPULATED payload, the shape field consumers
/// actually read, byte-identical, and because it was already the majority
/// behaviour rather than a fourth invention. 6257 and 6326 moved onto it;
/// this file is the reference, not the exception.
///
/// The silent-degradation argument -- that a consumer cannot tell "no
/// results" from "the command broke" -- is answered rather than dismissed,
/// and NOT by the exit code, which does not distinguish the two: `links
/// list` exits 0 for an empty listing and would exit 0 for a broken
/// emission too. The real answer is the one decision 1090 records. That
/// objection was correct WHEN FILED (task 6270) because the severe half of
/// it was real: POPULATED listings were emitting zero bytes, so empty
/// output genuinely did mean the command had broken. That half is fixed.
/// Zero bytes on empty now AGREES with the true answer, and an emitter that
/// is empty when the answer is empty is not degraded. For the operator who
/// is reading rather than parsing, the text form says it in words (`no
/// links for <ref>`).
/// @param rows The merged listing.
/// @return The complete stdout payload, empty for an empty listing.
export auto render_link_list_json(std::span<const directed_link> rows) -> std::string;

/// @brief `links trail <id>` text output — the complete payload, trailing
/// newline included. Empty renders `no audit trail for entity_link:<id>`.
///
/// A NULL `actor` renders as the literal `(none)`, not as an empty column.
/// @param rows The trail rows.
/// @param link_id The link the trail is for, for the empty-case line.
/// @return The complete stdout payload.
export auto render_trail_text(std::span<const audit_row> rows, std::int64_t link_id) -> std::string;

/// @brief `links trail <id> --json` output — NDJSON, each line terminated,
/// ZERO BYTES when empty. Same owns-its-terminators contract as
/// `render_link_list_json`.
/// @param rows The trail rows.
/// @return The complete stdout payload, empty for an empty trail.
export auto render_trail_json(std::span<const audit_row> rows) -> std::string;

/// @brief `links add` text output — the complete payload, newline included.
/// @param link The created link.
/// @param relationship_text The relationship as the OPERATOR spelled it.
/// @return The complete stdout payload.
export auto render_links_add_text(const entity_link& link, std::string_view relationship_text) -> std::string;

/// @brief `links add --json` output — the complete payload, newline
/// included.
///
/// This envelope is `{"ok":true,"id":…,"from_kind":…,"from_id":…,…}` and
/// carries NO `created_at`, which is the third distinct JSON envelope on
/// this family: `links list` emits the row WITH `created_at` and WITHOUT
/// `ok`, and `<entity> link` emits a subject-keyed one. They were captured
/// separately and are not interchangeable.
/// @param link The created link.
/// @param relationship_text The relationship as the OPERATOR spelled it.
/// @return The complete stdout payload.
export auto render_links_add_json(const entity_link& link, std::string_view relationship_text) -> std::string;

/// @brief `links remove` text output — the complete payload, newline
/// included.
/// @param link_id The removed link's id.
/// @return The complete stdout payload.
export auto render_links_remove_text(std::int64_t link_id) -> std::string;

/// @brief `links remove --json` output — `{"ok":true,"id":<n>}` plus a
/// newline.
/// @param link_id The removed link's id.
/// @return The complete stdout payload.
export auto render_links_remove_json(std::int64_t link_id) -> std::string;

/// @brief `plan|task|question link` text output — the complete payload,
/// newline included.
///
/// Note the DOUBLE spaces around the `[relationship]` group; they are the
/// oracle's, and this shape (`linked plan:1 -> task:3  [cites]  (link id:
/// 4)`) differs from `links add`'s (`created entity_link: task:1
/// --[cites]--> task:2  (link id: 10)`) in every part but the id suffix.
/// @param subject_kind The subject's kind (`plan`, `task` or `question`).
/// @param subject_id The subject's id.
/// @param link The created link.
/// @param relationship_text The relationship as the OPERATOR spelled it.
/// @return The complete stdout payload.
export auto render_entity_link_text(entity_kind subject_kind, std::int64_t subject_id, const entity_link& link,
                                    std::string_view relationship_text) -> std::string;

/// @brief `plan|task|question link --json` output — the complete payload,
/// newline included.
///
/// The subject's id is emitted under a PER-VERB key (`plan_id`, `task_id`,
/// `question_id`) rather than a shared one, so the key is a parameter. The
/// caller passes its own; there is no default, because a wrong default here
/// would be silently plausible JSON.
/// @param subject_id_key The subject's JSON key, e.g. `"plan_id"`.
/// @param subject_id The subject's id.
/// @param link The created link.
/// @param relationship_text The relationship as the OPERATOR spelled it.
/// @return The complete stdout payload.
export auto render_entity_link_json(std::string_view subject_id_key, std::int64_t subject_id, const entity_link& link,
                                    std::string_view relationship_text) -> std::string;

/// @brief The `link … already exists` refusal BODY (no `error: ` prefix, no
/// trailing newline).
///
/// @warning `plan link` spells its arrow with the UNICODE `→` (U+2192)
/// while `links add`, `task link` and `question link` all spell theirs with
/// the ASCII `->`. That is an inconsistency in the oracle
/// (`handlers/plan/link.zig` writes `\u{2192}`; its two siblings write
/// `->`), captured by running all four, and it is REPRODUCED rather than
/// harmonised (D2). `unicode_arrow` is how a caller selects it — there is
/// no default, so no call site can inherit the wrong arrow silently.
/// @param from_kind The link's source kind.
/// @param from_id The link's source id.
/// @param to_kind The link's target kind.
/// @param to_id The link's target id.
/// @param relationship_text The relationship as the OPERATOR spelled it.
/// @param unicode_arrow True for `plan link`'s `→`; false for the ASCII `->`.
/// @return The message body.
export auto render_link_exists_error(entity_kind from_kind, std::int64_t from_id, entity_kind to_kind, std::int64_t to_id,
                                     std::string_view relationship_text, bool unicode_arrow) -> std::string;

// ===========================================================================
// task_touch_paths — path-level touch declarations (migration 00019)
// ===========================================================================

/// @brief One path-level touch declaration for a task: the repo it belongs
/// to (`projects.id`) and the repo-relative file path. Mirrors zig's
/// task.zig `TouchPath`.
export struct touch_path {
  std::int64_t repo_id; ///< The repo (`projects.id`) this path belongs to.
  std::string  path;    ///< The repo-relative file path.
};

/// @brief Record a path-level touch for a task in `task_touch_paths`.
///
/// Idempotent against the `unique(task_id, repo_id, path)` constraint —
/// `insert or ignore`, so a duplicate declaration is a silent no-op, not an
/// error. Mirrors zig's `addTouchPath`.
///
/// This is the path-level write ONLY — it does NOT also write the coarse
/// `entity_links` `task -> repo touches` edge. The Zig `cmd/`-layer handler
/// (`task touches add --path`) composes both writes in a savepoint; that
/// composition is `cmd/`-layer glue out of this task's scope (see
/// CMakeLists.txt). A caller wanting the same "path-touch implies
/// repo-touch" behavior calls both `add` (with `relationship::touches`,
/// treating `link_exists` as a no-op) and `add_touch_path` itself.
/// @param conn An open, migrated database connection.
/// @param task_id The task's row id.
/// @param repo_id The repo's row id (`projects.id`).
/// @param path The repo-relative file path.
/// @return Success, or `entity_link_error::query_failed`.
export auto add_touch_path(db::connection& conn, std::int64_t task_id, std::int64_t repo_id, std::string_view path)
    -> std::expected<void, entity_link_error>;

/// @brief Withdraw one path-level touch declaration.
///
/// Deliberately leaves the coarse `entity_links` `touches` repo edge alone
/// — it may still be wanted (other paths on the same repo, or an
/// intentional whole-repo claim); withdrawing THAT is a separate `remove`
/// call against the `entity_links` row. Mirrors zig's `removeTouchPath`
/// (same file-header note: "removing the edge alone leaves the path rows
/// still driving eligibility" — the inverse also holds, which is why this
/// function does not touch `entity_links` either).
/// @param conn An open, migrated database connection.
/// @param task_id The task's row id.
/// @param repo_id The repo's row id (`projects.id`).
/// @param path The repo-relative file path to withdraw.
/// @return Success, or `entity_link_error::not_found` when no such row
/// exists, or `entity_link_error::query_failed`.
export auto remove_touch_path(db::connection& conn, std::int64_t task_id, std::int64_t repo_id, std::string_view path)
    -> std::expected<void, entity_link_error>;

/// @brief List the path-level touch declarations for a task, ordered by
/// repo then path. Mirrors zig's `touchedPaths`.
/// @param conn An open, migrated database connection.
/// @param task_id The task's row id.
/// @return The declared paths (possibly empty), or
/// `entity_link_error::query_failed`.
export auto touched_paths(db::connection& conn, std::int64_t task_id)
    -> std::expected<std::vector<touch_path>, entity_link_error>;

/// @brief List repo ids linked from a task via `entity_links` with
/// `relationship = 'touches'`, ordered by link id. Mirrors zig's
/// `touchedRepoIDs`.
/// @param conn An open, migrated database connection.
/// @param task_id The task's row id.
/// @return The linked repo ids (possibly empty), or
/// `entity_link_error::query_failed`.
export auto touched_repo_ids(db::connection& conn, std::int64_t task_id)
    -> std::expected<std::vector<std::int64_t>, entity_link_error>;

} // namespace planar::engine::entitylink
