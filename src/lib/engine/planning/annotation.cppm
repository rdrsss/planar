/// @file annotation.cppm
/// @brief `planar.engine.planning.annotation` — the annotation entity:
/// line-anchored notes on code, with anchor-drift fields, an optional
/// slug, tags, and the retention-tier lifecycle (plan 996, task 6094).
///
/// Behavior-preserving port (D2) of zig/src/engine/planning/annotation.zig
/// PLUS the parts of zig/src/cmd/planar/handlers/annotate/*.zig that carry
/// real behavior rather than argument plumbing: `bulk.zig`'s selection and
/// per-row skip rules, `sweep.zig`'s staleness cutoff, `verify.zig`'s
/// anchor classifier, and `render.zig`'s JSON/text emitters.
///
/// Thirteen of the fourteen `annotate` schema leaves are backed here (add,
/// show, list, update, remove, tag, resolve, dismiss, archive,
/// bulk-resolve, bulk-dismiss, bulk-archive, sweep). The fourteenth,
/// `verify`, is backed too but with one dependency this tree does not
/// have — see `classify_anchor` below.
///
/// ## Why the handler-layer logic lives in this engine module
///
/// There is no layer-3 `cmd_*` module in the C++ tree yet. `bulk`, `sweep`
/// and `verify` are not argument parsing — they are the actual selection
/// semantics and transactional shape of three schema leaves, and this
/// task's whole point is to pin those against the oracle. Leaving them
/// unported until a binary exists would mean shipping `bulk-*` with no
/// coverage of the one hazard the task brief singles out. They compose
/// nothing outside this bucket (D20/decision 947 is about verbs that reach
/// across layer-2 PEERS; these reach only into this module's own
/// functions), so they sit here and lift cleanly into a handler later.
///
/// ## Deliberate omissions
///
/// - **`policy.audit.record`.** Same omission as `engine_planning`'s
///   plan/task surface already documents, and for the same reason: there
///   is no `policy.audit` module in the C++ tree. No `annotate` leaf reads
///   or emits audit rows.
/// - **`policy.scope_guard.check(null, null)`.** The Zig original calls it
///   with two nulls on create/update/remove/transition, which is
///   unconditionally a no-op (a null entity scope means "global", which
///   any write may touch). Porting a guaranteed-success call would be
///   noise. The guard question itself is Planar task 6075, an OPEN
///   operator decision, and this port deliberately takes no side.
module;

export module planar.engine.planning.annotation;

import std;
import planar.db;

namespace planar::engine::planning::annotation {

/// @brief Which kind of scope an annotation is stored under. Mirrors
/// zig's `annotation.ScopeKind`.
export enum class scope_kind : std::uint8_t {
  global,      ///< No project or association filter.
  repo,        ///< A `projects` row.
  association, ///< An `associations` row.
};

/// @brief Annotation lifecycle status. Mirrors zig's `annotation.Status`.
export enum class status : std::uint8_t {
  active,    ///< The default on create.
  resolved,  ///< Outcome state; may still progress to `archived`.
  dismissed, ///< Outcome state; may still progress to `archived`.
  archived,  ///< The sole final retention state.
};

/// @brief Where the annotation hangs in the source tree. Mirrors zig's
/// `annotation.AnchorFields`. `line_start`/`line_end` unset means a
/// file-level annotation (the columns are SQL NULL).
export struct anchor_fields {
  std::string                 path;       ///< Repository-relative path. Required.
  std::optional<std::int64_t> line_start; ///< First anchored line, unset for file-level.
  std::optional<std::int64_t> line_end;   ///< Last anchored line, unset for file-level.
  std::string                 commit_sha; ///< Commit the anchor was taken against; `""` when unknown.
  std::string                 text_hash;  ///< Hash of the anchored text; `""` when unknown.
  std::string                 text;       ///< Snippet of the anchored text; `""` when unknown.
};

/// @brief A stored annotation. Mirrors zig's `annotation.Annotation`.
export struct annotation {
  std::int64_t                id{};                            ///< Row id.
  scope_kind                  scope_kind_{scope_kind::global}; ///< Stored scope kind.
  std::optional<std::int64_t> scope_id;                        ///< Stored scope id; unset for global.
  anchor_fields               anchor;                          ///< Anchor descriptor.
  std::optional<std::string>  title;                           ///< Title; nullable.
  std::optional<std::string>  slug;                            ///< Slug; nullable and globally UNIQUE.
  std::string                 body;                            ///< Body; NOT NULL, defaults to `""`.
  status                      status_{status::active};         ///< Lifecycle status.
  std::string                 vendor;                          ///< Vendor tag; NOT NULL, defaults to `""`.
  std::optional<std::int64_t> plan_id;                         ///< Direct FK column (not an entity_link).
  std::optional<std::int64_t> task_id;                         ///< Direct FK column (not an entity_link).
  std::vector<std::string>    tags;                            ///< Tags, lexicographically ascending.
  std::string                 created_at;                      ///< Creation timestamp.
  std::string                 updated_at;                      ///< Last-modification timestamp.
};

/// @brief Arguments to `create`. Mirrors zig's `annotation.CreateArgs`.
export struct create_args {
  anchor_fields                   anchor;                  ///< Required anchor descriptor.
  std::optional<std::string_view> title;                   ///< Title, when supplied.
  std::optional<std::string_view> slug;                    ///< Slug, when supplied.
  std::string_view                body{""};                ///< Body; defaults to `""`.
  status                          status_{status::active}; ///< Initial status.
  std::string_view                vendor{""};              ///< Vendor tag; defaults to `""`.
  std::optional<std::int64_t>     plan_id;                 ///< Owning plan, when supplied.
  std::optional<std::int64_t>     task_id;                 ///< Owning task, when supplied.
  std::vector<std::string>        tags;                    ///< Tags; trimmed and de-duplicated on write.
  std::optional<std::string_view> scope;                   ///< Scope-ref slug; unset means global.
};

/// @brief Patch for `update`. Every field is independently optional; an
/// all-unset patch is a no-op that returns a fresh snapshot WITHOUT
/// bumping `updated_at`. Mirrors zig's `annotation.UpdateArgs`.
export struct update_args {
  std::optional<std::string_view> title;   ///< New title.
  std::optional<std::string_view> slug;    ///< New slug.
  std::optional<std::string_view> body;    ///< New body.
  std::optional<status>           status_; ///< New status; validated against the transition matrix.
  std::optional<std::int64_t>     plan_id; ///< New owning plan.
  std::optional<std::int64_t>     task_id; ///< New owning task.
  std::optional<anchor_fields>    anchor;  ///< Full anchor replacement.
  std::optional<std::string_view> scope;   ///< New scope-ref slug.
};

/// @brief Filter for `list` (and, in the same shape, for every `bulk-*`
/// leaf). Mirrors zig's `annotation.ListFilter`.
export struct list_filter {
  std::optional<std::string_view> anchor_path; ///< Exact `anchor_path` match.
  std::optional<status>           status_;     ///< Exact status match.
  std::optional<std::int64_t>     plan_id;     ///< Exact `plan_id` match.
  std::optional<std::int64_t>     task_id;     ///< Exact `task_id` match.
  std::optional<std::string_view> vendor;      ///< Exact `vendor` match.
  std::optional<std::string_view> tag;         ///< Rows carrying this tag.
  std::optional<std::string_view> scope;       ///< Scope-ref slug.
};

/// @brief Error surface for this module. Mirrors zig's
/// `annotation.Error`.
export enum class annotation_error : std::uint8_t {
  not_found,         ///< No annotation with that id or slug.
  unsupported_scope, ///< The scope-ref form is not supported.
  slug_not_found,    ///< The scope-ref slug did not resolve.
  terminal_status,   ///< The requested lifecycle transition is refused.
  slug_conflict,     ///< The slug is already taken (annotations.slug is UNIQUE).
  empty_tag,         ///< A tag that is empty after trimming.
  query_failed,      ///< An underlying SQL statement failed.
};

/// @brief Parse a status from its stored text.
/// @param s The status text.
/// @return The status, or `std::nullopt` for an unrecognized value.
export auto status_from_text(std::string_view s) -> std::optional<status>;

/// @brief Render a status as its stored text.
/// @param s The status.
/// @return The status text.
export auto status_to_text(status s) -> std::string_view;

/// @brief Parse a scope kind from its stored text.
/// @param s The scope-kind text.
/// @return The scope kind, or `std::nullopt` for an unrecognized value.
export auto scope_kind_from_text(std::string_view s) -> std::optional<scope_kind>;

/// @brief Render a scope kind as its stored text.
/// @param k The scope kind.
/// @return The scope-kind text.
export auto scope_kind_to_text(scope_kind k) -> std::string_view;

/// @brief True when `s` cannot accept a `resolve` or `dismiss`
/// transition — i.e. it is already at or past an outcome state. Mirrors
/// zig's `Status.isTerminal`.
///
/// Do NOT read this as "has no outgoing edges": `archived` is the only
/// status with none. Its sole caller-side meaning is the `bulk-resolve` /
/// `bulk-dismiss` pre-skip.
/// @param s The status to test.
/// @return `true` for resolved, dismissed and archived.
export auto is_terminal(status s) -> bool;

/// @brief Insert an annotation, its de-duplicated tags, and return it as
/// stored.
///
/// Tags are trimmed of ASCII whitespace, empties are dropped, and
/// duplicates collapse — verified against the oracle: `--tags "x, y ,x"`
/// stored exactly `["x","y"]`.
///
/// @param conn An open, migrated database connection.
/// @param args The annotation to create.
/// @return The stored annotation, or `annotation_error::slug_conflict`
/// when the slug is taken.
export auto create(db::connection& conn, const create_args& args) -> std::expected<annotation, annotation_error>;

/// @brief Read one annotation by id, tags included.
/// @param conn An open, migrated database connection.
/// @param id The annotation row id.
/// @return The annotation, or `annotation_error::not_found`.
export auto show(db::connection& conn, std::int64_t id) -> std::expected<annotation, annotation_error>;

/// @brief Read one annotation by its (optional) slug.
/// @param conn An open, migrated database connection.
/// @param slug The slug to look up.
/// @return The annotation, or `annotation_error::not_found`.
export auto show_by_slug(db::connection& conn, std::string_view slug) -> std::expected<annotation, annotation_error>;

/// @brief List annotations matching `filter`, ordered by id ascending.
/// @param conn An open, migrated database connection.
/// @param filter The filter to apply; all-unset lists everything.
/// @return The matching annotations, tags included.
export auto list(db::connection& conn, const list_filter& filter) -> std::expected<std::vector<annotation>, annotation_error>;

/// @brief Apply a patch. A status change is validated against the
/// annotation transition matrix (see
/// `planar.engine.planning.transitions`), and a refusal surfaces as
/// `annotation_error::terminal_status`.
/// @param conn An open, migrated database connection.
/// @param id The annotation to patch.
/// @param patch The fields to change.
/// @return The patched annotation.
export auto update(db::connection& conn, std::int64_t id, const update_args& patch)
    -> std::expected<annotation, annotation_error>;

/// @brief Delete an annotation. Tag rows cascade; the FTS5 delete trigger
/// evicts the index entry.
/// @param conn An open, migrated database connection.
/// @param id The annotation to delete.
/// @return Success, or `annotation_error::not_found`.
export auto remove(db::connection& conn, std::int64_t id) -> std::expected<void, annotation_error>;

/// @brief Transition an annotation to `resolved`.
/// @param conn An open, migrated database connection.
/// @param id The annotation to transition.
/// @return The transitioned annotation, or
/// `annotation_error::terminal_status`.
export auto resolve(db::connection& conn, std::int64_t id) -> std::expected<annotation, annotation_error>;

/// @brief Transition an annotation to `dismissed`.
/// @param conn An open, migrated database connection.
/// @param id The annotation to transition.
/// @return The transitioned annotation, or
/// `annotation_error::terminal_status`.
export auto dismiss(db::connection& conn, std::int64_t id) -> std::expected<annotation, annotation_error>;

/// @brief Transition an annotation to `archived`.
/// @param conn An open, migrated database connection.
/// @param id The annotation to transition.
/// @return The transitioned annotation, or
/// `annotation_error::terminal_status`.
export auto archive(db::connection& conn, std::int64_t id) -> std::expected<annotation, annotation_error>;

/// @brief Attach a tag. Trimmed; an empty result is
/// `annotation_error::empty_tag`. A duplicate `(annotation, tag)` pair is
/// absorbed as a no-op.
/// @param conn An open, migrated database connection.
/// @param ann_id The annotation to tag.
/// @param tag The tag text.
/// @return Success, `annotation_error::empty_tag`, or
/// `annotation_error::not_found` when the annotation does not exist.
export auto add_tag(db::connection& conn, std::int64_t ann_id, std::string_view tag) -> std::expected<void, annotation_error>;

/// @brief Detach a tag. Removing a tag that is not attached is a no-op —
/// and, matching the Zig original, so is removing one from an annotation
/// that does not exist (unlike `add_tag`, this path performs NO existence
/// check).
/// @param conn An open, migrated database connection.
/// @param ann_id The annotation to untag.
/// @param tag The tag text.
/// @return Success, or `annotation_error::empty_tag`.
export auto remove_tag(db::connection& conn, std::int64_t ann_id, std::string_view tag) -> std::expected<void, annotation_error>;

/// @brief List an annotation's tags, lexicographically ascending.
/// @param conn An open, migrated database connection.
/// @param ann_id The annotation to read.
/// @return The tags.
export auto list_tags(db::connection& conn, std::int64_t ann_id) -> std::expected<std::vector<std::string>, annotation_error>;

// ---------------------------------------------------------------------------
// bulk-resolve / bulk-dismiss / bulk-archive
// ---------------------------------------------------------------------------

/// @brief Which transition a `bulk_apply` pass performs. Mirrors
/// handlers/annotate/bulk.zig's `Action`.
export enum class bulk_action : std::uint8_t {
  resolve, ///< `annotate bulk-resolve`.
  dismiss, ///< `annotate bulk-dismiss`.
  archive, ///< `annotate bulk-archive`.
};

/// @brief Apply `action` to every annotation matching `filter`, returning
/// the number of rows that actually transitioned.
///
/// **THIS IS NOT TRANSACTIONAL, and that is the oracle's own behavior,
/// established by experiment rather than assumed.** The probe (hazard 1 in
/// task 6094's brief), reproducible verbatim:
///
/// ```
/// $Z annotate add --anchor-path z.txt --title B1   # x4, ids 1..4
/// sqlite3 p3.db "create trigger boom before update on annotations
///                when new.id = 3 begin select raise(abort,'boom'); end;"
/// $Z annotate bulk-archive --anchor-path z.txt --json
///   -> exit 1, `error: annotate bulk-archive: QueryFailed`
/// sqlite3 p3.db "select id,status from annotations order by id"
///   -> 1|archived   2|archived   3|active   4|active
/// ```
///
/// The prefix STAYS APPLIED. There is no `BEGIN`, no rollback, and the
/// rows after the failure are never attempted. Each row also carried a
/// distinct `updated_at`, confirming one UPDATE statement per row rather
/// than a single set-update.
///
/// Per-row skip rules, preserved exactly:
///   - a row already in the target state is skipped (not counted);
///   - for `resolve`/`dismiss`, a row at ANY outcome state
///     (`is_terminal`) is skipped;
///   - for `archive`, no pre-skip — `resolved` and `dismissed` legally
///     progress to `archived` under the retention-tier model;
///   - a `terminal_status` error from the per-row transition is swallowed
///     and the row skipped; ANY OTHER error aborts the pass and is
///     returned, leaving the prefix applied.
///
/// @param conn An open, migrated database connection.
/// @param filter The selection; `bulk-resolve`/`bulk-dismiss` pass
/// `status_ = active`, `bulk-archive` passes no status filter at all.
/// @param action Which transition to apply.
/// @return The number of rows that transitioned, or the first
/// non-`terminal_status` error encountered.
export auto bulk_apply(db::connection& conn, const list_filter& filter, bulk_action action)
    -> std::expected<std::size_t, annotation_error>;

/// @brief Render the `--json` envelope the three `bulk-*` leaves share.
/// Oracle-captured: `{"ok":true,"action":"archived","count":2}` — note the
/// action value is the PAST-PARTICIPLE noun the handler passes
/// (`resolved`/`dismissed`/`archived`), not the verb name.
/// @param verb_name The participle: `"resolved"`, `"dismissed"` or `"archived"`.
/// @param count The transitioned-row count.
/// @return The single-line JSON object, with no trailing newline.
export auto render_bulk_json(std::string_view verb_name, std::size_t count) -> std::string;

/// @brief Render the text line the three `bulk-*` leaves share.
/// Oracle-captured: `archived: 2 annotation(s)`.
/// @param verb_name The participle (see `render_bulk_json`).
/// @param count The transitioned-row count.
/// @return The text line, with no trailing newline.
export auto render_bulk_text(std::string_view verb_name, std::size_t count) -> std::string;

// ---------------------------------------------------------------------------
// sweep
// ---------------------------------------------------------------------------

/// @brief Select the ids `annotate sweep` would archive: status in
/// `{resolved, dismissed}` AND `updated_at` older than `since_days` days,
/// compared through SQLite's own `julianday`. Mirrors
/// handlers/annotate/sweep.zig's SELECT verbatim, including the STRICT
/// `>` (a row exactly `since_days` old is NOT swept).
/// @param conn An open, migrated database connection.
/// @param since_days The staleness cutoff in days; must be >= 0.
/// @return The ids to archive, in table order.
export auto sweep_candidates(db::connection& conn, std::int64_t since_days)
    -> std::expected<std::vector<std::int64_t>, annotation_error>;

/// @brief Archive every `sweep_candidates` row, returning the count that
/// transitioned. Like `bulk_apply` this is NOT transactional and swallows
/// per-row `terminal_status` (the defensive guard for a row archived
/// concurrently between the SELECT and the UPDATE).
/// @param conn An open, migrated database connection.
/// @param since_days The staleness cutoff in days.
/// @return The number of rows archived.
export auto sweep(db::connection& conn, std::int64_t since_days) -> std::expected<std::size_t, annotation_error>;

/// @brief Render `annotate sweep --json`. Oracle shape:
/// `{"ok":true,"action":"sweep","since_days":30,"swept":0}`.
/// @param since_days The cutoff that was applied.
/// @param swept The archived-row count.
/// @return The single-line JSON object, with no trailing newline.
export auto render_sweep_json(std::int64_t since_days, std::size_t swept) -> std::string;

/// @brief Render `annotate sweep`'s text line. Oracle shape:
/// `sweep: archived 0 annotation(s) older than 30 day(s)`.
/// @param since_days The cutoff that was applied.
/// @param swept The archived-row count.
/// @return The text line, with no trailing newline.
export auto render_sweep_text(std::int64_t since_days, std::size_t swept) -> std::string;

// ---------------------------------------------------------------------------
// verify
// ---------------------------------------------------------------------------

/// @brief An anchor's freshness verdict. Mirrors handlers/annotate/
/// verify.zig's `VerifyState`.
export enum class verify_state : std::uint8_t {
  fresh,   ///< File readable and (hash absent or hash matches).
  drifted, ///< File readable but the stored hash does not match.
  stale,   ///< File missing or unreadable.
};

/// @brief One row of `annotate verify`'s report.
export struct verify_row {
  std::int64_t id{};                       ///< Annotation id.
  std::string  anchor_path;                ///< The anchor path as stored.
  verify_state state{verify_state::stale}; ///< The verdict.
};

/// @brief Classify one anchor given the file's CURRENT contents, or
/// `std::nullopt` when the file could not be read.
///
/// Mirrors verify.zig's `classify` exactly:
///   - unreadable -> `stale`;
///   - no stored `text_hash` -> `fresh` (no drift signal to compare);
///   - otherwise SHA-256 the file body, take the first `text_hash.size()`
///     hex characters of the digest, and compare — a stored PREFIX
///     therefore matches, which is deliberate.
///
/// Taking the contents as a parameter rather than reading the file here is
/// the one place this module departs from the Zig shape, and it is
/// deliberate: it keeps filesystem access out of an engine module, and it
/// makes the classifier a pure function that can be tested without
/// fixtures on disk. The caller (a future `cmd/` handler) supplies the
/// read, exactly as it will supply the cwd.
///
/// @param stored_text_hash The annotation's `anchor.text_hash`.
/// @param file_contents The file's current bytes, or unset if unreadable.
/// @return The verdict.
export auto classify_anchor(std::string_view stored_text_hash, std::optional<std::string_view> file_contents) -> verify_state;

/// @brief Render the state as the text `verify` prints and stores.
/// @param s The verdict.
/// @return `"fresh"`, `"drifted"` or `"stale"`.
export auto verify_state_to_text(verify_state s) -> std::string_view;

/// @brief Render `annotate verify --json`. Oracle shape:
/// `{"ok":true,"rows":[{"id":1,"anchor_path":"a.txt","state":"fresh"}]}`.
/// @param rows The classified rows.
/// @return The single-line JSON object, with no trailing newline.
export auto render_verify_json(const std::vector<verify_row>& rows) -> std::string;

/// @brief Render `annotate verify`'s text output — one line per row, or
/// the literal `(no active annotations)` when there are none. Includes the
/// trailing newline on every line, since it is multi-line output.
/// @param rows The classified rows.
/// @return The full text block.
export auto render_verify_text(const std::vector<verify_row>& rows) -> std::string;

// ---------------------------------------------------------------------------
// show / list rendering
// ---------------------------------------------------------------------------

/// @brief Render one annotation as the `--json` object `annotate add`,
/// `show`, `update`, `resolve`, `dismiss` and `archive` all emit.
/// Byte-identical to the oracle's, including key order and the fixed
/// `anchor` sub-object.
/// @param a The annotation.
/// @return The single-line JSON object, with no trailing newline.
export auto render_json(const annotation& a) -> std::string;

/// @brief Render a list as `annotate list --json` emits it: a bare JSON
/// array of `render_json` objects, `[]` when empty.
/// @param items The annotations.
/// @return The single-line JSON array, with no trailing newline.
export auto render_list_json(const std::vector<annotation>& items) -> std::string;

/// @brief Render one annotation as `annotate show` prints it — a
/// label-aligned block in which every optional field is OMITTED when
/// absent rather than printed empty. Includes trailing newlines.
/// @param a The annotation.
/// @return The text block.
export auto render_text(const annotation& a) -> std::string;

/// @brief Render a list as `annotate list` prints it: `%5d  %-10s  %s`
/// per row (title falling back to the anchor path), or the literal
/// `(no annotations)` when empty. Includes trailing newlines.
/// @param items The annotations.
/// @return The text block.
export auto render_list_text(const std::vector<annotation>& items) -> std::string;

/// @brief Render `annotate tag --json`. Oracle shape:
/// `{"ok":true,"id":1,"tag":"zz","action":"add"}`.
/// @param id The annotation id.
/// @param tag The tag text.
/// @param removing True when `--remove` was passed.
/// @return The single-line JSON object, with no trailing newline.
export auto render_tag_json(std::int64_t id, std::string_view tag, bool removing) -> std::string;

/// @brief Render `annotate tag`'s text line. Oracle shape:
/// `annotation 1: added tag 'zz'`.
/// @param id The annotation id.
/// @param tag The tag text.
/// @param removing True when `--remove` was passed.
/// @return The text line, with no trailing newline.
export auto render_tag_text(std::int64_t id, std::string_view tag, bool removing) -> std::string;

/// @brief Render `annotate remove --json`. Oracle shape:
/// `{"ok":true,"id":3}`.
/// @param id The removed annotation's id.
/// @return The single-line JSON object, with no trailing newline.
export auto render_remove_json(std::int64_t id) -> std::string;

/// @brief Render `annotate remove`'s text line. Oracle shape:
/// `annotation 3 removed`.
/// @param id The removed annotation's id.
/// @return The text line, with no trailing newline.
export auto render_remove_text(std::int64_t id) -> std::string;

} // namespace planar::engine::planning::annotation
