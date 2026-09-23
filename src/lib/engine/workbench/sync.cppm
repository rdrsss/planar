/// @file sync.cppm
/// @brief `planar.engine.workbench.sync` — bidirectional DB <-> filesystem
/// sync for one feature tree (plan 996, task 6037).
///
/// Behavior-preserving port (D2) of zig/src/engine/workbench/sync.zig.
///
/// ## One engine, four verbs
///
/// `status`, `pull`, `push` and `sync` are the SAME traversal under
/// different write permissions. The traversal classifies every projected
/// file, then each mode decides whether to apply its class or merely count
/// it as pending:
///
///   class            status   pull      push      sync
///   ---------------  -------  --------  --------  --------
///   no_op            -        -         -         -
///   db_to_fs         pending  pending   APPLY     APPLY
///   fs_to_db         pending  APPLY     pending   APPLY
///   deleted_on_fs    pending  APPLY     pending   APPLY
///   new_on_fs        pending  APPLY*    pending   APPLY*
///   conflict         records a sync_events row in every mode
///   malformed        counted; never applied in any mode
///
///   * only for a file whose front matter says `entity_kind: task`. Any
///     other kind is reported as `new_on_fs` and left alone — the workbench
///     auto-creates tasks and nothing else.
///
/// ## How a class is decided
///
/// With a manifest row present, the FS side changed if the file's hash
/// differs from `content_hash` and the DB side changed if the entity's
/// `updated_at` differs from `db_updated_at`. One side changed -> that
/// direction. BOTH changed -> `conflict`, unless the two contents happen to
/// have converged to the same bytes, which is `no_op`. With no manifest row
/// the file is new to sync: absent on disk -> `db_to_fs`, identical to the
/// rendered bytes -> `no_op`, otherwise -> `fs_to_db`.
///
/// A file that does not PARSE short-circuits all of that to `malformed`,
/// and a malformed file makes the whole run exit non-zero. That is the
/// rejection behavior `planar.engine.workbench.parse` exists to define.
///
/// ## The terminal filter is push-only
///
/// `push` (and `restore`) drop entities whose status is terminal under the
/// active mode; the anchor plan is always exempt, because dropping it would
/// break feature-tree navigation. `pull` deliberately does NOT filter — a
/// terminal entity's file is still a legitimate input. Oracle-confirmed:
/// with one cancelled task, `push` reports `1 filtered (mode=failures)`
/// while `pull` reports nothing.
///
/// A filtered entity that ALREADY has a file on disk is counted as
/// `pre_existing_terminal` and reported with a remediation line; passing
/// `apply_cleanup` removes those files (and their manifest rows) in the
/// same pass. Both counts oracle-captured.
///
/// ## Root is a PARAMETER, never resolved here
///
/// Unlike the Zig original — which calls `resolveWorkbenchRoot` from inside
/// `run`, reaching `std.c.environ` — every entry point below takes the
/// resolved root explicitly. `planar.engine.workbench.root` is the only
/// module in this bucket that decides where the workbench lives, and it
/// takes an injected environment. That is what keeps a test from deleting
/// the developer's real `~/.planar/workbench/`.
module;

export module planar.engine.workbench.sync;

import std;
import planar.db;
import planar.engine.workbench.manifest;
import planar.engine.workbench.terminal;

namespace planar::engine::workbench::sync {

/// @brief Which of the four verbs is running.
export enum class mode : std::uint8_t { status, pull, push, sync };

/// @brief How one projected file relates to its entity.
export enum class classification : std::uint8_t {
  no_op,
  fs_to_db,
  db_to_fs,
  conflict,
  new_on_fs,
  deleted_on_fs,
  malformed,
};

/// @brief The operator-visible spelling of a class.
///
/// These exact snake_case bytes appear in `--json` output's
/// `entries[].class`, produced by zig's `std.json.Stringify` over the enum.
/// @param value The class.
/// @return The JSON spelling.
export auto classification_name(classification value) -> std::string_view;

/// @brief One classified file.
export struct entry {
  classification value = classification::no_op; ///< The class; serialized as `class`.
  std::string    file_path;                     ///< ROOT-relative stored path.
  std::string    entity_kind;                   ///< Canonical kind; EMPTY for an unparseable new file.
  std::int64_t   entity_id   = 0;               ///< The entity id; 0 when not yet created.
  std::int64_t   conflict_id = 0;               ///< The `sync_events` row id; 0 unless `conflict`.
  std::string    parse_error;                   ///< The Zig error tag; empty unless `malformed`.
};

/// @brief One malformed file, surfaced separately so callers need not
/// re-scan `entries`.
export struct malformed_file {
  std::string path;        ///< ROOT-relative stored path.
  std::string parse_error; ///< The Zig error tag.
};

/// @brief One file `pull`/`sync` refused because a NON-BODY field it
/// cannot round-trip was edited on disk (task 6910).
///
/// `pull_to_db` writes only `body` (plus `status` for `task`/`plan`); a
/// decision's `## Rationale` section and a question's `**Answer:**` line
/// are rendered TO disk but never read back FROM it. Before task 6910 an
/// operator edit there was silently discarded on every pull. This is the
/// per-entity refusal that replaces the silence: the whole entity's pull
/// is skipped (not just the field), the entity is left untouched, and the
/// refusal is surfaced here rather than merely counted as `pending` —
/// `pending` alone does not tell the operator WHY, or that the fix is
/// `decision edit` / `question answer`, not another pull.
export struct field_edit_refusal {
  std::string  path;          ///< ROOT-relative stored path.
  std::string  entity_kind;   ///< `decision` or `question`.
  std::int64_t entity_id = 0; ///< The entity id.
  std::string  field;         ///< `rationale` or `answer`.
};

/// @brief The outcome of one run. Field order matches the `--json` payload.
export struct result {
  std::size_t                     applied   = 0;                      ///< Changes written.
  std::size_t                     pending   = 0;                      ///< Changes this mode declined to write.
  std::size_t                     conflicts = 0;                      ///< Conflicting files.
  std::size_t                     malformed = 0;                      ///< Unparseable files.
  std::vector<malformed_file>     malformed_files;                    ///< One per unparseable file, in `entries` order.
  std::size_t                     filtered              = 0;          ///< Terminal entities excluded (push only).
  std::size_t                     pre_existing_terminal = 0;          ///< Filtered entities that still had a file.
  std::size_t                     cleaned               = 0;          ///< Of those, how many `apply_cleanup` removed.
  std::string                     filter_mode           = "failures"; ///< The active mode's label.
  std::vector<entry>              entries;                            ///< Every classified file, in enumeration order.
  std::size_t                     field_edit_refused = 0;             ///< Count of `field_edit_refusals` (task 6910).
  std::vector<field_edit_refusal> field_edit_refusals;                ///< One per refused entity.
};

/// @brief One anchor plan with a workbench tree, as `workbench list` shows it.
export struct active_feature {
  std::int64_t plan_id = 0;         ///< The top-level plan.
  std::string  slug;                ///< Its slug.
  std::string  status;              ///< Its status.
  std::string  assoc_slug;          ///< Its association, or the literal `global`.
  std::string  plan_key;            ///< Its `external_id`, else `p<id>`.
  bool         has_fs_tree = false; ///< Whether its feature directory exists on disk.
};

/// @brief A rendered entity: where it goes and what it contains.
export struct rendered_entity {
  std::string rel_path; ///< FEATURE-relative path, forward-slashed.
  std::string content;  ///< The complete file bytes.
};

/// @brief An anchor plan's identity, as the layout needs it.
export struct anchor {
  std::int64_t                id = 0;     ///< The anchor plan id.
  std::string                 slug;       ///< The plan slug.
  std::string                 assoc_slug; ///< The association slug, EMPTY for a global-scope plan.
  std::optional<std::int64_t> assoc_id;   ///< The association row id, unset for a global-scope plan.
  std::string                 plan_key;   ///< The plan's `external_id`, else `p<id>`.
};

/// @brief Which side of a conflict to keep.
export enum class conflict_resolution : std::uint8_t { fs, db };

/// @brief Failure surface for this module.
export enum class sync_error : std::uint8_t {
  not_found,     ///< No such anchor plan, entity, or sync event.
  query_failed,  ///< SQLite refused an operation.
  invalid_input, ///< A sync event that is not an unresolved workbench conflict.
  io_failed,     ///< A filesystem write or delete failed.
};

/// @brief Read an anchor plan's identity.
///
/// Only a TOP-LEVEL plan (`parent_plan_id is null`) is an anchor; a child
/// plan id reports `not_found`, the same as a nonexistent one.
/// @param conn The database connection.
/// @param anchor_plan_id The candidate anchor plan.
/// @return The anchor, or the failure.
export auto fetch_anchor(db::connection& conn, std::int64_t anchor_plan_id) -> std::expected<anchor, sync_error>;

/// @brief Resolve a `<plan>` positional, which may be an id or a slug.
///
/// A numeric argument is looked up by id; anything else by slug. A numeric
/// argument below 1 is `invalid_input`, not `not_found` — the oracle
/// distinguishes them (`invalid plan '0'` at exit 2 vs `plan not found: 999`
/// at exit 1) and so does this.
/// @param conn The database connection.
/// @param argument The positional as typed.
/// @return The anchor, or the failure.
export auto resolve_plan_argument(db::connection& conn, std::string_view argument) -> std::expected<anchor, sync_error>;

/// @brief The absolute feature directory for an anchor.
/// @param root The resolved workbench root.
/// @param value The anchor.
/// @return The directory path.
export auto feature_dir_for(std::string_view root, const anchor& value) -> std::string;

/// @brief Render one entity to its path and bytes.
///
/// This is the function `spec ingest --apply`, `import` and `synthesize`
/// (task 6119) need alongside `feature_dir_for` and the parser.
/// @param conn The database connection.
/// @param anchor_plan_id The owning anchor plan, written into front matter.
/// @param kind The entity kind.
/// @param id The entity id.
/// @return The rendered entity, or the failure.
export auto render_entity(db::connection& conn, std::int64_t anchor_plan_id, std::string_view kind, std::int64_t id)
    -> std::expected<rendered_entity, sync_error>;

/// @brief Report drift without writing anything.
/// @param conn The database connection.
/// @param anchor_plan_id The anchor plan.
/// @param root The resolved workbench root.
/// @return The run result, or the failure.
export auto status(db::connection& conn, std::int64_t anchor_plan_id, std::string_view root) -> std::expected<result, sync_error>;

/// @brief Apply FS -> DB changes; report DB -> FS drift.
/// @param conn The database connection.
/// @param anchor_plan_id The anchor plan.
/// @param root The resolved workbench root.
/// @return The run result, or the failure.
export auto pull(db::connection& conn, std::int64_t anchor_plan_id, std::string_view root) -> std::expected<result, sync_error>;

/// @brief Apply DB -> FS changes; report FS -> DB drift.
/// @param conn The database connection.
/// @param anchor_plan_id The anchor plan.
/// @param root The resolved workbench root.
/// @param filter_mode Which terminal entities to exclude.
/// @param apply_cleanup Also remove pre-existing files for excluded entities.
/// @return The run result, or the failure.
export auto push(db::connection& conn, std::int64_t anchor_plan_id, std::string_view root, terminal::mode filter_mode,
                 bool apply_cleanup) -> std::expected<result, sync_error>;

/// @brief Apply both directions.
/// @param conn The database connection.
/// @param anchor_plan_id The anchor plan.
/// @param root The resolved workbench root.
/// @return The run result, or the failure.
export auto sync_both(db::connection& conn, std::int64_t anchor_plan_id, std::string_view root)
    -> std::expected<result, sync_error>;

/// @brief Settle one recorded conflict by keeping one side.
/// @param conn The database connection.
/// @param root The resolved workbench root.
/// @param event_id The `sync_events` row id.
/// @param prefer Which side to keep.
/// @return Success, or the failure.
export auto resolve_conflict(db::connection& conn, std::string_view root, std::int64_t event_id, conflict_resolution prefer)
    -> std::expected<void, sync_error>;

/// @brief Delete a feature's on-disk tree and its manifest rows.
///
/// There is no archive STORE — the tree is removed, not packaged. The Zig
/// signature accepts a `filter_mode` for symmetry with push/restore and
/// ignores it; this one omits the parameter rather than accept-and-discard,
/// and the layer-3 flag stays declared so `--help` is unchanged.
/// @param conn The database connection.
/// @param anchor_plan_id The anchor plan.
/// @param root The resolved workbench root.
/// @return The removed feature directory, or the failure. Removing an
/// ALREADY-absent tree succeeds and still reports the path (oracle-probed:
/// `workbench archive` twice in a row exits 0 both times).
export auto archive(db::connection& conn, std::int64_t anchor_plan_id, std::string_view root)
    -> std::expected<std::string, sync_error>;

/// @brief Re-materialize a feature's tree from the database.
/// @param conn The database connection.
/// @param anchor_plan_id The anchor plan.
/// @param root The resolved workbench root.
/// @param filter_mode Which terminal entities to skip; the anchor plan is exempt.
/// @return The feature directory, or the failure.
export auto restore(db::connection& conn, std::int64_t anchor_plan_id, std::string_view root, terminal::mode filter_mode)
    -> std::expected<std::string, sync_error>;

/// @brief Every top-level plan, with whether it has a workbench tree.
/// @param conn The database connection.
/// @param root The resolved workbench root.
/// @return The features in id order, or the failure.
export auto list_active(db::connection& conn, std::string_view root) -> std::expected<std::vector<active_feature>, sync_error>;

/// @brief Strip a rendered file's generated header back to the operator's
/// own prose.
///
/// Drops leading `# ` headings, leading `**Label:**` lines and leading blank
/// lines, then trims the remainder. It removes the header every renderer
/// shares; the per-kind pieces (`## Content`, `## Body` / `## Rationale`,
/// trailing `**Answer:**` / `**Next action:**`) are `extract_entity_body`'s
/// job, and `pull` goes through that.
///
/// It does NOT strip a `---` line, which is why the double-wrap case
/// (`artifact update --body @<canonical-workbench-file>`) is sticky: the
/// nested front matter is body text, survives the round trip, and re-renders
/// inside the outer block. Oracle-reproduced end to end and pinned in
/// sync.t.cpp.
/// @param body The body as read from disk.
/// @return The operator's prose.
export auto extract_body_text(std::string_view body) -> std::string_view;

/// @brief The operator's prose for one entity KIND: `extract_body_text`
/// plus the per-kind pieces of the wrapper the matching renderer writes
/// (task 6881).
///
/// `push` renders more than the heading-and-labels prefix: an artifact's
/// `## Content` heading, a decision's `## Body` heading and trailing
/// `## Rationale` section, an answered question's trailing `**Answer:**` /
/// `**Answered at:**` lines, a task's trailing `**Next action:**` line. A
/// prefix-only strip stored every one of those in the body, where the next
/// push re-wrapped them. This is the inverse `pull` applies so a
/// push/edit/pull/push round trip is byte-stable. The trailing sections are
/// cut at the FIRST line that starts with their label -- prose that itself
/// begins a line with `## Rationale`, `**Answer:**` or `**Next action:**` is
/// cut there too. `plan` and `scenario` render nothing beyond the prefix and
/// fall through to `extract_body_text`.
/// @param kind The entity kind as `pull_to_db` receives it (`artifact`,
///             `decision`, `question`, `task`, `scenario`, `plan`).
/// @param body The body as read from disk.
/// @return The operator's prose, trimmed.
export auto extract_entity_body(std::string_view kind, std::string_view body) -> std::string_view;

} // namespace planar::engine::workbench::sync
