/// @file association.cppm
/// @brief `planar.engine.identity.association` — association entity CRUD
/// plus the project-membership surface the cross-scope guard's callers
/// need (tech-spec § "engine buckets", plan 996 task cpp-scope-assoc).
///
/// Behavior-preserving port (D2) of the CRUD + membership subset of
/// zig/src/engine/identity/association.zig:
///   - `associations` table CRUD (`create`, `show_by_slug`, `list_all`).
///   - the `project_associations` join helpers (`add_member`,
///     `remove_member`, `members`) that take an association slug + a repo
///     path, creating a `projects` row on the fly when the path hasn't
///     been registered yet (mirrors zig's `findOrCreateProjectByPath`).
///
/// `policy.audit.record` IS ported, as of task 6100: `create`,
/// `add_member` and `remove_member` each append an `audit_log` row through
/// the layer-1 `planar.policy` module. The fourth Zig call site — the
/// `auto-link project_id=N → association '<slug>'` row inside
/// `applyProposals` — is not, because `applyProposals` itself is not (see
/// below); it lands with that surface.
///
/// NOT ported (out of this task's scope — see the CMakeLists.txt file
/// header comment): the auto-detection surface (`detectProposals`,
/// `proposalsFromSignals`, `applyProposals`, `enrichProposals`).
/// It is independent of the cross-scope guard's needs: the guard only
/// ever reads association/membership rows (via `scope.cppm`'s
/// `derive_from_cwd`/`resolve_slug`/`slug_from_ref`) and this module's
/// `create`/`add_member`/`remove_member`/`members` exist so a test (or a
/// future `cmd/` handler) can set up and exercise that membership state,
/// not because the guard itself calls them.
module;

export module planar.engine.identity.association;

import std;
import planar.db;

namespace planar::engine::identity {

/// @brief An association's kind. Mirrors zig's `Kind`. `ad_hoc` spells
/// the Zig `@"ad-hoc"` identifier without the hyphen (not a valid C++
/// identifier); `association_kind_to_text`/`association_kind_from_text`
/// still round-trip the hyphenated `"ad-hoc"` wire form.
export enum class association_kind : std::uint8_t {
  org,      ///< A workspace / organization grouping.
  project,  ///< A project-scoped grouping (compatibility with older project-association flows).
  client,   ///< A client grouping.
  personal, ///< A personal bucket.
  ad_hoc,   ///< An ad-hoc grouping; the default kind when unset (wire form `"ad-hoc"`).
  host,     ///< Auto-detected: git remote host.
  path,     ///< Auto-detected: parent directory.
  lang,     ///< Auto-detected: language ecosystem.
};

/// @brief Parse a `kind` column value / `--kind` flag value.
/// @param s The raw text to parse (e.g. `"org"`, `"ad-hoc"`).
/// @return The parsed kind, or unset for an unrecognized string.
export auto association_kind_from_text(std::string_view s) -> std::optional<association_kind>;

/// @brief Render `k` as the wire/column text form (mirrors zig's
/// `@tagName(kind)`, which prints `ad-hoc` for `.@"ad-hoc"`).
/// @param k The kind to render.
/// @return The wire/column text form.
export auto association_kind_to_text(association_kind k) -> std::string_view;

/// @brief One row from the `associations` table. Mirrors zig's
/// `Association`.
export struct association {
  std::int64_t               id;            ///< The row's id.
  std::string                slug;          ///< The association's unique slug.
  std::string                name;          ///< Display name.
  association_kind           kind;          ///< The association's kind.
  bool                       auto_detected; ///< True when created by auto-detection rather than an operator.
  std::optional<std::string> config_json;   ///< Optional per-association configuration blob.
  std::string                created_at;    ///< Row creation timestamp.
  std::string                updated_at;    ///< Row last-update timestamp.
};

/// @brief Minimal projection of the `projects` table — enough for
/// `members()`. Mirrors zig's `Project` (deliberately not the fuller
/// `project.zig` CRUD type, which is a separate, not-yet-landed task —
/// see the CMakeLists.txt file header comment).
export struct project_ref {
  std::int64_t               id;        ///< The row's id.
  std::string                slug;      ///< The project's unique slug.
  std::string                name;      ///< Display name.
  std::optional<std::string> root_path; ///< Filesystem root path, when registered.
};

/// @brief Error surface for every fallible operation in this module.
export enum class association_error : std::uint8_t {
  not_found,          ///< No row matched the given slug/id.
  slug_conflict,      ///< `create` was given a slug that already exists.
  unknown_kind,       ///< A stored `kind` column value did not parse.
  already_member,     ///< `add_member` was given a project already linked to the association.
  not_a_member,       ///< `remove_member` was given a project not linked to the association.
  query_failed,       ///< An underlying SQL statement failed.
  audit_write_failed, ///< The `audit_log` row could not be written. Zig spelling: `WriteFailed`.
};

/// @brief Arguments to `create`. Mirrors zig's `CreateArgs`.
export struct create_args {
  std::string                     slug;        ///< The association's slug (required).
  std::optional<std::string>      name;        ///< Defaults to `slug` if unset.
  std::optional<association_kind> kind;        ///< Defaults to `ad_hoc` if unset.
  std::optional<std::string>      config_json; ///< Optional per-association configuration blob.
};

/// @brief Create a new association.
/// @param conn An open, migrated database connection.
/// @param args The slug (required) plus optional name/kind/config_json.
/// @return The created row, or `association_error::slug_conflict` if the
/// slug already exists, or `association_error::query_failed` on a SQL failure.
export auto create(db::connection& conn, const create_args& args) -> std::expected<association, association_error>;

/// @brief Look up an association by id.
/// @param conn An open, migrated database connection.
/// @param id The association's row id.
/// @return The row, or `association_error::not_found`, or
/// `association_error::unknown_kind` if the stored `kind` column doesn't
/// parse, or `association_error::query_failed` on a SQL failure.
export auto show_by_id(db::connection& conn, std::int64_t id) -> std::expected<association, association_error>;

/// @brief Look up an association by slug.
/// @param conn An open, migrated database connection.
/// @param slug The association's slug.
/// @return The row, or `association_error::not_found`, or
/// `association_error::unknown_kind`, or `association_error::query_failed`.
export auto show_by_slug(db::connection& conn, std::string_view slug) -> std::expected<association, association_error>;

/// @brief List every association, ordered by slug.
/// @param conn An open, migrated database connection.
/// @return The rows, or `association_error::unknown_kind` /
/// `association_error::query_failed`.
export auto list_all(db::connection& conn) -> std::expected<std::vector<association>, association_error>;

/// @brief Provenance of a `project_associations` membership row. Mirrors
/// zig's `AddMemberSource`.
export enum class add_member_source : std::uint8_t {
  user,            ///< Explicitly added by an operator.
  auto_git_remote, ///< Auto-derived from the project's git remote.
  auto_path,       ///< Auto-derived from the project's parent directory.
  auto_lang,       ///< Auto-derived from the project's detected language ecosystem.
};

/// @brief Render `s` as the `project_associations.source` column value.
/// @param s The provenance value to render.
/// @return The wire/column text form.
export auto add_member_source_to_text(add_member_source s) -> std::string_view;

/// @brief Add a project (looked up or created from `repo_path`) to the
/// association named by `assoc_slug`. Mirrors zig's `addMember`.
///
/// @param conn An open, migrated database connection.
/// @param assoc_slug The target association's slug.
/// @param repo_path The project's root path; a `projects` row is created
/// on the fly (slug/name derived from the path's basename, with a numeric
/// suffix on a basename collision) if none is registered at this path yet.
/// @param source Membership provenance. Defaults to `add_member_source::user`.
/// @return Success, or `association_error::not_found` if `assoc_slug`
/// doesn't exist, `association_error::already_member` if the project is
/// already linked, or `association_error::query_failed`.
export auto add_member(db::connection& conn, std::string_view assoc_slug, std::string_view repo_path,
                       add_member_source source = add_member_source::user) -> std::expected<void, association_error>;

/// @brief Remove a project (looked up by `repo_path`) from the
/// association named by `assoc_slug`. Mirrors zig's `removeMember`.
/// @param conn An open, migrated database connection.
/// @param assoc_slug The target association's slug.
/// @param repo_path The project's registered root path.
/// @return Success, or `association_error::not_found` if `assoc_slug`
/// doesn't exist, `association_error::not_a_member` if no project is
/// registered at `repo_path` or it isn't linked, or `association_error::query_failed`.
export auto remove_member(db::connection& conn, std::string_view assoc_slug, std::string_view repo_path)
    -> std::expected<void, association_error>;

/// @brief List every project belonging to the association named by
/// `assoc_slug`, ordered by project slug. Mirrors zig's `members`.
/// @param conn An open, migrated database connection.
/// @param assoc_slug The association's slug.
/// @return The member projects, or `association_error::not_found` if
/// `assoc_slug` doesn't exist, or `association_error::query_failed`.
export auto members(db::connection& conn, std::string_view assoc_slug)
    -> std::expected<std::vector<project_ref>, association_error>;

/// @brief Render one association as the operator-facing key/value block.
///
/// Ports zig/src/engine/identity/association.zig's `renderText` byte for
/// byte, including the ten-column label padding (wider than the plan
/// renderer's nine) and the one conditional line: `config:` appears only
/// when `config_json` is set. `auto:` prints `yes`/`no`, not a boolean
/// literal.
/// @param a The association to render.
/// @return The complete block, INCLUDING its trailing newline. The caller
/// writes it verbatim and appends nothing.
export auto render_text(const association& a) -> std::string;

/// @brief Render one association as the single-line JSON object.
///
/// Field order is the `association` struct's declaration order, because
/// the oracle's JSON path is `std.json.Stringify.value` over the Zig
/// `Association` struct and Zig serializes fields in declaration order.
/// `auto_detected` is a JSON boolean; `config_json` renders as `null`
/// when unset and as a JSON STRING (not an inlined object) when set,
/// matching the Zig field's `?[]const u8` type.
/// @param a The association to render.
/// @return The JSON object with NO trailing newline — a fragment the
/// caller terminates (the oracle's `output.emit` prints `"\n"` after
/// stringifying).
export auto render_json(const association& a) -> std::string;

/// @brief Render `members()`'s result as the operator-facing member table
/// (plan 996, task 6188).
///
/// This module's other two renderers are SINGULAR (`const association&`),
/// which is why `assoc members` could not be wired before: `members()`
/// returns `std::vector<project_ref>` and there was nothing to render it
/// with. The generic list path is keyed on an association's own list
/// renderer, so this one is separate rather than an overload of it — the
/// Zig original draws the same line (`renderProjectListText`, called
/// directly by the handler instead of through `emitList`).
///
/// Two column values are NOT the obvious ones:
///   - The second column is `root_path`, NOT `name`. Both are present on
///     `project_ref` and they are equal for a project registered by its
///     own directory name, so the fixture that distinguishes them needs a
///     project whose slug, name and path all differ. One was built to
///     settle it (`assoc members` on a project at `../aaaa…` printed the
///     PATH).
///   - An UNSET `root_path` prints the literal `(no root)`, not an empty
///     column. That is a real state — `projects.root_path` is nullable —
///     and blanking it would make an unregistered project look like one
///     rooted at "".
///
/// Columns are `{:<20}  {}`: slug left-aligned and space-padded to twenty,
/// two spaces, then the path unpadded. A slug wider than twenty is not
/// truncated; it pushes the rest of the line right.
/// @param members The rows to render, in the order `members()` returned
/// them (project slug ascending).
/// @return The complete block, INCLUDING the trailing newline on its last
/// line — or the literal `"(no members)\n"` when empty, which is a WORD
/// and not zero bytes. The caller writes it verbatim and appends nothing.
export auto render_member_list_text(std::span<const project_ref> members) -> std::string;

/// @brief Render `members()`'s result as the single-line JSON array.
///
/// Each element's field order is `project_ref`'s declaration order (`id`,
/// `slug`, `name`, `root_path`), because the oracle's JSON path is
/// `std.json.Stringify.value` over the Zig `Project` struct. `root_path`
/// renders as `null` when unset — the same state `render_member_list_text`
/// spells `(no root)`. The empty list renders `[]`.
/// @param members The rows to render, in the order given.
/// @return The JSON array with NO trailing newline — a fragment the caller
/// terminates, same contract as `render_json`.
export auto render_member_list_json(std::span<const project_ref> members) -> std::string;

} // namespace planar::engine::identity
