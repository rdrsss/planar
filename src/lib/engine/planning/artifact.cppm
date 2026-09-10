/// @file artifact.cppm
/// @brief `planar.engine.planning.artifact` — the `artifacts` table's CRUD
/// half: create, show, list and update, plus the four renderers the
/// `planar artifact` leaves print through (plan 996 roadmap M12 item 10,
/// task 6196).
///
/// Port target: zig/src/engine/planning/artifact.zig.
///
/// `artifact` is the LAST unported planning family. Everything below was
/// captured by RUNNING `zig/zig-out/bin/planar` in a pinned scratch arena,
/// not read off the oracle's source or inherited from `question`,
/// `decision` or `scenario` — and on nearly every axis where an
/// implementer would expect the four families to agree, they do not. The
/// disagreements are enumerated here because each one is a defect waiting
/// for the next person who assumes the sibling shape transfers.
///
/// ## The empty `--status` filter means `{draft, active}`
///
/// A FOURTH distinct answer from four sibling families, and the one most
/// likely to be copied wrong:
///
///   - `question`'s empty arm means `open`
///   - `decision`'s means `{proposed, accepted}`
///   - `scenario`'s means EVERY status, `retired` included
///   - `artifact`'s means `{draft, active}` — the two TERMINAL statuses,
///     `superseded` and `retired`, are off-screen unless named
///
/// None of the four is derivable from any other. Captured by running a
/// bare `artifact list` over a seeded set and watching the terminal rows
/// not come back.
///
/// ## `--kind` and `--status` are comma-split AND validated, at exit 2
///
/// `artifact list --kind tech_spec,adr` returns both. An unknown token in
/// EITHER flag refuses the whole call at **exit 2** with `error: unknown
/// kind 'nosuch'` / `error: unknown status 'final'` — where the
/// character-identical refusal on `scenario list --status` exits **1**,
/// because that one dies with an `error.InvalidStatus` that has no arm in
/// zig's `codeFor`. One flag name, two families, two exit codes. That
/// validation lives in the HANDLER, ahead of this engine, which is why
/// this module's enums have no "unknown" member: an unparseable token
/// never reaches it.
///
/// The EMPTY string is accepted by both and is not an error, but it does
/// NOT mean the same thing on each. `--kind ""` yields no tokens and
/// applies NO kind predicate. `--status ""` also yields no tokens, and an
/// empty status set is the `{draft, active}` default arm — so it behaves
/// exactly like omitting the flag, NOT like "every status". Two flags on
/// one leaf, two meanings for the same empty input; both re-probed against
/// a fixture holding terminal rows, which is the only fixture that can
/// tell the two apart.
///
/// ## `--scope` is comma-split into a read SET
///
/// `artifact list --scope global,project:proj` returns the union, ordered
/// by id. An unresolvable member refuses the whole call
/// (`error: artifact list: SlugNotFound`) rather than being skipped — a
/// read verb that silently drops one member of its scope set returns a
/// short list that looks complete.
///
/// Note the refusal's SHAPE: `artifact list: SlugNotFound` carries the
/// verb path and the bare zig error tag, where `artifact show`'s
/// not-found is the prose `no artifact with id 999`. Both are
/// oracle-captured; they are genuinely different formats from the same
/// family.
///
/// ## `--plan` is EXISTENCE-CHECKED here, unlike `scenario`'s
///
/// `scenario add --plan 4242` succeeds and leaves a DANGLING
/// `entity_links` edge. `artifact add --plan 999` refuses at exit 1 with
/// `error: artifact add: NotFound` and writes nothing. The check runs
/// BEFORE the insert. Two sibling families, one flag name, opposite
/// answers — captured on both.
///
/// On success the edge is `('artifact', <id>, 'plan', <plan>,
/// 'derives-from')`. The `from_kind` is the bare `artifact`, and it is the
/// spelling editflow's anchor resolver queries.
///
/// ## `artifact update --status` walks a TRANSITION matrix
///
/// Not a free assignment. All sixteen edges were run against the oracle:
///
///   draft      -> {draft, active}
///   active     -> {draft, active, superseded, retired}
///   superseded -> terminal
///   retired    -> terminal
///
/// `active -> draft` IS legal — an artifact can be walked back to draft,
/// which no sibling family permits of its own second state. `draft ->
/// superseded` and `draft -> retired` are NOT: a draft must be activated
/// before it can be laid to rest. Every refusal is exit 1 / `error:
/// artifact update: IllegalTransition`. Identity transitions always
/// succeed.
///
/// The matrix lives in `planar.engine.planning.transitions` under a NEW
/// `transition_kind::artifact` arm added by this task. That module's
/// header already CLAIMED to enforce `.artifact`; the enum had no such
/// member and no arm existed. The claim was stale prose, and this task
/// made it true rather than deleting it.
///
/// ## `update` with no field at all is a refusal, not a no-op
///
/// `artifact update 1 --json` with no mutable flag exits 1 with
/// `error: at least one field must be specified for update` — prose, not a
/// tag. That refusal is raised by the HANDLER; `no_fields` exists in this
/// module's error surface to mirror the zig engine's own arm, which the
/// CLI never reaches.
///
/// ## The scope a bare `artifact add` writes into
///
/// It cwd-DERIVES, like `question add` / `decision add` / `scenario add`
/// and unlike `plan create`, which refuses inside a project with no
/// association. In a project with no association the row lands `global`;
/// once the project's repo path is registered under one, the same command
/// lands `association`. Both captured in the same arena, before and after
/// `assoc add`.
///
/// ## `body` is nullable and NULL is distinguishable from empty
///
/// An absent `--body` binds SQL NULL and renders `"body":null`; `--body ""`
/// binds `''` and renders `"body":""`. `render_text` omits the `body:` line
/// entirely when NULL and prints it when present — including when present
/// and empty.
///
/// ## `slug` is a real column this port does NOT read
///
/// `artifacts.slug` exists (migration 00013) under a partial unique index.
/// The oracle's `readRow` does not select it and no `artifact` leaf emits
/// it, so neither does this port. Adding it to the JSON would be a silent
/// contract change.
module;

export module planar.engine.planning.artifact;

import std;
import planar.db;

namespace planar::engine::planning {

/// @brief Which kind of scope an artifact belongs to.
export enum class artifact_scope_kind : std::uint8_t {
  global,      ///< Not owned by any repo or association.
  association, ///< Owned by an association.
  repo,        ///< Owned by a single repo.
};

/// @brief The `artifacts.kind` CHECK set, in the column's declared order.
///
/// Fourteen values as of migration 00013. The order matters only for
/// readability — nothing iterates it positionally.
export enum class artifact_kind : std::uint8_t {
  tech_spec,
  adr,
  design_note,
  summary,
  readme,
  generated,
  other,
  product_spec,
  roadmap,
  research,
  getting_started,
  changelog_entry,
  glossary_term,
  test_spec,
};

/// @brief The `artifacts.status` CHECK set.
///
/// The COLUMN's default is `active`; the CLI's `--status` default is
/// `draft`. The two genuinely differ, and this engine always binds the
/// status explicitly rather than letting the column default apply — which
/// is what makes `artifact add` land `draft`.
export enum class artifact_status : std::uint8_t {
  draft,
  active,
  superseded,
  retired,
};

/// @brief Parse a `kind` token.
/// @param s The token.
/// @return The kind, or unset when `s` is not one of the fourteen.
export auto artifact_kind_from_text(std::string_view s) -> std::optional<artifact_kind>;

/// @brief Render a kind as its column text.
/// @param k The kind.
/// @return The column text.
export auto artifact_kind_to_text(artifact_kind k) -> std::string_view;

/// @brief Parse a `status` token.
/// @param s The token.
/// @return The status, or unset when `s` is not one of the four.
export auto artifact_status_from_text(std::string_view s) -> std::optional<artifact_status>;

/// @brief Render a status as its column text.
/// @param s The status.
/// @return The column text.
export auto artifact_status_to_text(artifact_status s) -> std::string_view;

/// @brief One `artifacts` row, as every `planar artifact` leaf sees it.
///
/// `slug` is deliberately absent — see this file's header.
export struct artifact {
  std::int64_t                id;         ///< The row's id.
  artifact_scope_kind         scope_kind; ///< Which kind of scope this artifact belongs to.
  std::optional<std::int64_t> scope_id;   ///< The scope's row id, unset for global.
  artifact_kind               kind;       ///< The document kind.
  std::string                 title;      ///< Display title.
  /// @brief The document body, when there is one. NULL and `""` are
  /// distinct and both are operator-visible.
  std::optional<std::string> body;
  /// @brief Where the document came from on disk, when recorded.
  std::optional<std::string> source_path;
  artifact_status            status;     ///< Current lifecycle status.
  std::string                created_at; ///< Row creation timestamp.
  std::string                updated_at; ///< Row last-update timestamp.
};

/// @brief Arguments to `create_artifact`. Mirrors zig's artifact.zig
/// `CreateArgs`.
export struct artifact_create_args {
  std::string   title; ///< The artifact's title (required).
  artifact_kind kind;  ///< The document kind (required — `--kind` has no default).
  /// @brief The document body. Unset binds SQL NULL; `""` binds `''`.
  std::optional<std::string> body;
  std::optional<std::string> source_path; ///< Origin path, when recorded.
  /// @brief The status to write. Defaults to `active` HERE, matching zig's
  /// `CreateArgs`; the CLI overrides it to `draft`, which is why a bare
  /// `artifact add` lands `draft` and not the column default.
  artifact_status status = artifact_status::active;
  /// @brief Optional plan to link this artifact to through an
  /// `entity_links` (`artifact -> plan`, relationship `derives-from`) edge.
  ///
  /// CHECKED for existence before any write, unlike `scenario`'s
  /// same-named flag — see this file's header.
  std::optional<std::int64_t> plan_id;
  std::optional<std::string>  scope; ///< Scope slug accepted by `planar.scope_ref::resolve`.
};

/// @brief Patch for `update_artifact`. Every member unset is the
/// `no_fields` refusal.
export struct artifact_update_args {
  std::optional<std::string>     title;       ///< New title.
  std::optional<std::string>     body;        ///< New body.
  std::optional<artifact_status> status;      ///< New status; walks the transition matrix.
  std::optional<std::string>     source_path; ///< New origin path.
  std::optional<std::string>     scope;       ///< New scope slug.
};

/// @brief Filter for `list_artifacts`. Mirrors zig's artifact.zig
/// `ListFilter`.
export struct artifact_list_filter {
  /// @brief Match any of these statuses. **EMPTY MEANS `{draft, active}`** —
  /// not every status, and not `open`. See this file's header; this is the
  /// single most copy-prone value in the family.
  std::vector<artifact_status> statuses;
  /// @brief Match any of these kinds. Empty applies NO kind predicate.
  std::vector<artifact_kind> kinds;
  /// @brief Restrict to artifacts carrying a `derives-from` edge to this
  /// plan. Intersects with the scope predicate rather than replacing it.
  std::optional<std::int64_t> plan_id;
  /// @brief A scope slug, OR-ed with every member of `scopes`.
  std::optional<std::string> scope;
  /// @brief Additional scope slugs, OR-ed with `scope`. An empty
  /// combination applies NO scope predicate at all.
  std::vector<std::string> scopes;
};

/// @brief Error surface for every fallible operation in this module.
export enum class artifact_error : std::uint8_t {
  not_found,         ///< No such artifact — or, on create, no such `--plan`.
  unsupported_scope, ///< The scope slug's grammar is not one this build resolves.
  slug_not_found,    ///< The scope slug's grammar parsed but named no row.
  /// @brief The status move is not in the matrix. Zig spelling:
  /// `IllegalTransition`, propagated UNFOLDED — the operator sees the bare
  /// tag in `error: artifact update: IllegalTransition`.
  illegal_transition,
  /// @brief `update` was called with every field unset. Zig spelling:
  /// `NoFields`. Unreachable through the CLI, which raises its own prose
  /// refusal first; mirrored so the mapping is total.
  no_fields,
  query_failed,       ///< A prepare/bind/step failed, or a stored enum column is unparseable.
  audit_write_failed, ///< The `audit_log` row could not be written. Zig spelling: `WriteFailed`.
};

/// @brief Resolve `--body`'s `@path` grammar. Mirrors zig's
/// `artifact.readBody`.
///
/// A value whose FIRST byte is `@` names a file to read RAW; anything else
/// is the literal body. The read is byte-for-byte — there is NO front-matter
/// stripping at this layer, and none anywhere on the `artifact update --body
/// @file` path. A file that opens with a `---` front-matter block reaches
/// the column WITH that block intact (oracle-captured). Any stripping the
/// workbench does happens in the editflow layer, which is not this one and
/// is not on this path.
///
/// A bare `@` (nothing after it) reads the empty path and fails.
/// @param value The raw `--body` argument.
/// @return The body bytes, or unset when `value` named a file that could
/// not be read.
export auto read_body(std::string_view value) -> std::optional<std::string>;

/// @brief Create a new artifact.
///
/// Order of operations is the oracle's and is observable: resolve the
/// scope (an unresolvable slug refuses BEFORE any write), CHECK `--plan`
/// exists (a missing one refuses, also before any write), INSERT, `create`
/// audit row, then the `--plan` edge.
/// @param conn An open, migrated database connection.
/// @param args The artifact's title and kind plus the optional rest.
/// @return The created row, or `not_found` (nonexistent `--plan`),
/// `slug_not_found` / `unsupported_scope`, `audit_write_failed`, or
/// `query_failed`.
export auto create_artifact(db::connection& conn, const artifact_create_args& args) -> std::expected<artifact, artifact_error>;

/// @brief Read one artifact by id.
/// @param conn An open, migrated connection.
/// @param id The artifact id.
/// @return The row, or `not_found`.
export auto show_artifact(db::connection& conn, std::int64_t id) -> std::expected<artifact, artifact_error>;

/// @brief List artifacts matching `filter`, ordered by id.
/// @param conn An open, migrated connection.
/// @param filter The status/kind/plan/scope predicates.
/// @return The matching rows, or the first resolution/query failure.
export auto list_artifacts(db::connection& conn, const artifact_list_filter& filter)
    -> std::expected<std::vector<artifact>, artifact_error>;

/// @brief Apply `patch` to one artifact and re-read it.
///
/// A `status` member walks the transition matrix and refuses with
/// `illegal_transition` when the edge is absent. Every other member is a
/// free assignment. `updated_at` is bumped on any successful update,
/// including an identity status move.
/// @param conn An open, migrated connection.
/// @param id The artifact to patch.
/// @param patch The fields to change.
/// @return The updated row, or `not_found` / `no_fields` /
/// `illegal_transition` / a resolution or write failure.
export auto update_artifact(db::connection& conn, std::int64_t id, const artifact_update_args& patch)
    -> std::expected<artifact, artifact_error>;

/// @brief Render one artifact as the `artifact show` text block.
///
/// Labels are padded to THIRTEEN columns (`id:` plus ten spaces), where
/// `scenario`'s are padded to twelve. The `body:` line is conditional and
/// sits between `scope:` and `created:`.
/// @param a The artifact.
/// @return The rendered block, newline-terminated.
export auto render_text(const artifact& a) -> std::string;

/// @brief Render one artifact as its JSON object.
/// @param a The artifact.
/// @return The JSON document.
export auto render_json(const artifact& a) -> std::string;

/// @brief Render a list as the `artifact list` text table.
///
/// The empty list is `(no artifacts)` — WITH parentheses, matching
/// `question` and `scenario` and not `decision`'s bare `no decisions`.
/// @param items The rows.
/// @return The rendered table, or the empty-list line.
export auto render_list_text(std::span<const artifact> items) -> std::string;

/// @brief Render a list as a JSON array.
/// @param items The rows.
/// @return The JSON array.
export auto render_list_json(std::span<const artifact> items) -> std::string;

} // namespace planar::engine::planning
