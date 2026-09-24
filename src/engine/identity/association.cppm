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
/// The auto-detection surface (`detectProposals`, `proposalsFromSignals`,
/// `enrichProposals`, `applyProposals`) landed with task 6325 — see the
/// "Auto-detection" section at the bottom of this file. Its fourth Zig
/// `policy.audit.record` call site (the `auto-link project_id=N →
/// association '<slug>'` row inside `applyProposals`) is ported with it,
/// so this module's audit coverage is now complete.
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

/// @brief The `list` predicate. Mirrors zig's `ListFilter` (plan 996,
/// task 6279).
///
/// One field, and the oracle's struct has exactly the same one — this is
/// not a trimmed port. An unset `kind` composes no `where` term at all, so
/// `list(conn, {})` and `list_all(conn)` run the identical statement and
/// `list_all` is now a call through to it.
export struct list_filter {
  std::optional<association_kind> kind; ///< Restrict to this kind; unset lists every kind.
};

/// @brief List associations matching `filter`, ordered by slug.
///
/// The kind term binds `association_kind_to_text(k)` — the HYPHENATED wire
/// form for `ad_hoc` — because that is what the `kind` column stores
/// (zig binds `@tagName(k)`, which prints `ad-hoc`). Binding the C++
/// enumerator's spelling `ad_hoc` would match no row and the verb would
/// answer `(no associations)` for a kind that exists.
/// @param conn An open, migrated database connection.
/// @param filter The predicate; a default-constructed one lists everything.
/// @return The rows, or `association_error::unknown_kind` /
/// `association_error::query_failed`.
export auto list(db::connection& conn, const list_filter& filter) -> std::expected<std::vector<association>, association_error>;

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

/// @brief Render `list()`'s result as the operator-facing association
/// table (plan 996, task 6279).
///
/// Columns are `{:<20}  {:<12}  {}` — slug padded to twenty, kind padded
/// to TWELVE, then the name unpadded. The kind column's width is twelve
/// and not the twenty its neighbour uses; the widest kind text is
/// `personal` at eight, so a fixture built only from short kinds cannot
/// tell twelve from any larger number and the value has to come from the
/// oracle's format string rather than from measuring output.
///
/// The third column is `name`, NOT `root_path` — this renderer's sibling
/// `render_member_list_text` prints a PATH in its second column, and the
/// two are easy to cross-wire because both are "the wide trailing column".
/// @param items The rows, in the order `list()` returned them (slug ascending).
/// @return The complete block, INCLUDING its trailing newline — or the
/// literal `"(no associations)\n"` when empty, which is a WORD and not zero
/// bytes, and is spelled differently from the member renderer's
/// `"(no members)\n"`. The caller writes it verbatim and appends nothing.
export auto render_list_text(std::span<const association> items) -> std::string;

/// @brief Render `list()`'s result as the single-line JSON array.
///
/// Each element is exactly `render_json`'s object — the oracle's JSON path
/// is `std.json.Stringify.value` over the SLICE, which serializes each
/// element by the same declaration-order rules the singular renderer
/// mirrors. The empty list renders `[]`, NOT the text renderer's
/// `(no associations)` sentence.
/// @param items The rows, in the order given.
/// @return The JSON array with NO trailing newline — a fragment the caller
/// terminates, same contract as `render_json`.
export auto render_list_json(std::span<const association> items) -> std::string;

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

// ===========================================================================
// Auto-detection (proposals, enrichment, apply) — plan 996, task 6325
// ===========================================================================

/// @brief A candidate association derived from repository signals.
/// Mirrors zig's `Proposal`.
///
/// `assoc_exists` / `member_exists` are NOT filled by the producing
/// function; `enrich_proposals` COMPUTES them (clearing first, task 6327),
/// and both default false so an
/// un-enriched proposal reads as "will create" rather than as a lie about
/// existing state.
export struct proposal {
  std::string       slug;                  ///< The proposed association slug, already sanitized (e.g. `host:github.com`).
  association_kind  kind;                  ///< The kind implied by the signal that produced it.
  add_member_source source;                ///< The `project_associations.source` value `apply_proposals` will record.
  std::string       reason;                ///< Human-readable provenance (e.g. `"from git remote host"`).
  bool              assoc_exists  = false; ///< Set by `enrich_proposals`: an `associations` row with this slug already exists.
  bool              member_exists = false; ///< Set by `enrich_proposals`: the project is already linked to it.
};

/// @brief The raw signals `proposals_from_signals` turns into proposals.
/// Mirrors zig's `Signals`.
///
/// Split out from `detect_proposals` for the same reason the oracle splits
/// it: the mapping from signals to proposals is pure, so it can be tested
/// without a git repository, a filesystem fixture, or a subprocess.
export struct detect_signals {
  std::optional<std::string> git_remote;      ///< Raw `git remote get-url origin` output, already trimmed.
  std::optional<std::string> parent_basename; ///< Basename of the cwd's PARENT directory.
  std::optional<std::string> lang;            ///< Ecosystem tag (`go`/`rust`/`javascript`/`python`).
};

/// @brief Host and owner extracted from a git remote URL. Views borrow
/// from the string passed to `parse_remote`. Mirrors zig's `RemoteParts`.
export struct remote_parts {
  std::string_view host;  ///< The remote host, or empty when unrecognized.
  std::string_view owner; ///< The owning org/user, or empty when absent.
};

/// @brief Extract `(host, owner)` from an SSH or HTTPS git remote URL.
/// Behavior-preserving port of zig's `parseRemote`.
///
/// Recognizes exactly two shapes, tried in this order:
///   - SCP-style `git@host:owner/repo.git` (matched on the literal `git@`
///     PREFIX, so `ssh://git@host/...` does NOT take this branch).
///   - URL-style `scheme://[user@]host[:port]/owner/repo.git`.
///
/// Anything else — notably a bare local path like `/srv/git/repo.git` —
/// yields two EMPTY fields and therefore no proposals at all. A remote is
/// not required to be a forge URL, and the oracle declines rather than
/// guessing; both empty-field cases were probed against the oracle.
///
/// The `.git` suffix is stripped from the OWNER, which only matters for
/// the degenerate `https://host/owner.git` shape. The host keeps its port
/// stripped but its dots intact.
/// @param remote The raw remote URL.
/// @return The parts; either or both may be empty views into `remote`.
export auto parse_remote(std::string_view remote) -> remote_parts;

/// @brief Turn a signal bundle into proposals. Pure: no DB, no filesystem,
/// no subprocess. Behavior-preserving port of zig's `proposalsFromSignals`.
///
/// The heuristic is PRESENCE-based, not threshold-based — there is no
/// score, no cutoff, and no minimum signal count anywhere in the oracle.
/// Each present signal contributes its proposals unconditionally.
///
/// Order is the append order and is therefore fixed and reproducible:
/// **host, org, path, lang**. It is NOT sorted and NOT derived from any
/// hash container (the concern task 6274 raised for the routing table does
/// not apply here — the oracle appends to a plain list), so pinning it in a
/// test pins a real contract rather than an implementation artifact.
///
/// There is no dedup pass. None is needed: each of the four arms emits at
/// most one proposal and each uses a distinct slug prefix, so two proposals
/// can never collide. Idempotency across RUNS is `apply_proposals`'
/// job, not this function's.
///
/// Two signals can be present and still contribute nothing, and both were
/// probed:
///   - a remote whose host or owner `parse_remote` could not find;
///   - a `parent_basename` that sanitizes to the empty string (a directory
///     named `+++`), which is skipped rather than emitted as a bare `path:`.
/// `parent_basename` is additionally skipped for the literals `.` and `/`.
/// @param sig The gathered signals.
/// @return The proposals, in host/org/path/lang order; empty when nothing matched.
export auto proposals_from_signals(const detect_signals& sig) -> std::vector<proposal>;

/// @brief Gather signals from `dir` and return the proposals they imply.
/// Behavior-preserving port of zig's `detectProposals`.
///
/// Three probes, all best-effort — a failure contributes no proposal
/// rather than failing the call, which is why this returns a plain vector
/// and not an `expected`:
///   - `git -C dir remote get-url origin` (via `planar.git`'s
///     `probe_origin_url`, whose exit-status and empty-stdout handling is
///     already the oracle's);
///   - the basename of `dir`'s PARENT — note the parent, not `dir` itself;
///   - the first matching top-level ecosystem marker file.
///
/// This is the ONLY arm that can return empty in practice. The parent-
/// directory signal fires for essentially every real cwd, so `detect` on a
/// registered repository always proposes at least `path:<parent>`; the
/// empty answer requires a directory with no parent (`/`) or one whose
/// parent name sanitizes away. A fixture that produces no proposals makes
/// every downstream assertion vacuous, so tests must assert non-emptiness
/// explicitly before comparing.
/// @param dir The directory to inspect; should be absolute.
/// @return The proposals, possibly empty.
export auto detect_proposals(const std::filesystem::path& dir) -> std::vector<proposal>;

/// @brief Fill `assoc_exists` / `member_exists` on each proposal from the
/// current DB state. Behavior-preserving port of zig's `enrichProposals`.
///
/// Read-only — this NEVER writes, which is what makes `assoc detect`
/// without `--apply` a safe preview. An unregistered `root_path` is not an
/// error here (unlike in `apply_proposals`): the project id resolves to
/// zero, the membership probe is skipped, and every proposal reports
/// `assoc_exists` alone. That asymmetry is deliberate in the oracle so the
/// operator can preview before running `planar init`.
///
/// The two flags are WRITE-ONLY: this sets them and never clears them. Two
/// paths skip the membership probe and leave whatever was already on the
/// proposal — an unregistered `root_path`, and a proposal whose
/// association does not exist (the loop skips it before either
/// assignment). Re-enriching one slice against a second root therefore
/// does NOT reset it, and stale `true`s survive. Operators never see this
/// because the handler builds a fresh slice per invocation; anything that
/// reuses a slice must not rely on enrichment to correct a flag downward.
/// @param conn An open, migrated database connection.
/// @param proposals The proposals to annotate, mutated in place.
/// @param root_path The project root to resolve membership against.
/// @return Success, or `association_error::query_failed`.
export auto enrich_proposals(db::connection& conn, std::span<proposal> proposals, std::string_view root_path)
    -> std::expected<void, association_error>;

/// @brief Create the associations and membership rows the proposals name.
/// Behavior-preserving port of zig's `applyProposals`.
///
/// The mutation path, and its REFUSAL is the first thing about it: the
/// project at `root_path` must already be registered or the whole call
/// fails with `not_found` before any row is written — including when
/// `proposals` is empty, because the project lookup precedes the loop.
///
/// The association endpoint gets the opposite treatment: it is CREATED on
/// demand via `insert or ignore`, never validated. So this verb does not
/// belong to the "validates its link endpoints" family — it validates the
/// project endpoint and auto-vivifies the association endpoint. Captured
/// from the oracle, not designed here.
///
/// `insert or ignore` also means an association a human created earlier
/// survives untouched: its `name` and its `auto_detected = 0` are both
/// preserved rather than being overwritten with the slug and `1`. The
/// MEMBERSHIP row, by contrast, upserts its `source` on conflict, so a
/// re-run refreshes provenance to the current `auto:*` value. Both halves
/// were probed against the oracle with a hand-created `org:` row.
///
/// The whole apply runs in one transaction so a mid-loop failure cannot
/// leave an association created but unlinked.
/// @param conn An open, migrated database connection.
/// @param proposals The proposals to apply.
/// @param root_path The project root; must already be a registered `projects` row.
/// @return Success, `association_error::not_found` when no project is
/// registered at `root_path`, `association_error::audit_write_failed`, or
/// `association_error::query_failed`.
export auto apply_proposals(db::connection& conn, std::span<const proposal> proposals, std::string_view root_path)
    -> std::expected<void, association_error>;

/// @brief The operator-facing verdict for one proposal.
///
/// Three states, tested most-specific first: `member_exists` wins over
/// `assoc_exists`, because a proposal whose association exists AND whose
/// membership exists is `"already a member"`, not `"already exists, will
/// add"`. Reversing the two tests still produces a plausible-looking
/// label for every case, which is why the order is called out.
/// @param p The (enriched) proposal.
/// @return `"already a member"`, `"already exists, will add"`, or `"will create"`.
export auto proposal_action_label(const proposal& p) -> std::string_view;

/// @brief Render the proposals as the operator-facing block.
///
/// Layout is `"  {:<24} ({})  [{}]"` under a `"proposed associations:"`
/// header — slug padded to TWENTY-FOUR (not the twenty this file's other
/// renderers use), then the reason parenthesized, then the action label
/// bracketed.
///
/// The inter-column spacing is ASYMMETRIC: ONE space after the padded slug
/// and TWO after the closing paren. Measuring it off sample output does not
/// settle it, because every slug in a normal fixture is shorter than 24 and
/// the padding hides the difference; it has to come from the oracle's
/// format string. A two-and-two transcription survived the build and was
/// caught only by the byte-level differential.
/// @param proposals The enriched proposals, in `detect_proposals` order.
/// @return The complete block INCLUDING its trailing newline, or the
/// literal `"no proposed associations\n"` when empty — a SENTENCE, and a
/// different one from this file's parenthesized `(no associations)`.
export auto render_detect_text(std::span<const proposal> proposals) -> std::string;

/// @brief Render the proposals as the `--json` payload.
///
/// NEWLINE-DELIMITED JSON — one object per line, NOT a JSON array, so the
/// payload as a whole is not parseable by a single `JSON.parse` even though
/// each LINE is. An EMPTY result renders ZERO BYTES: N lines for N results,
/// with N allowed to be zero.
///
/// The empty case used to render the single object `{"proposals":[]}`, a
/// different shape entirely, naming a key the non-empty form never emits.
/// Both shapes were probed directly against the oracle and reproduced under
/// D2, whose reason to exist was the runtime differential lane. Decision
/// 1090 authorises the change here, applying the reasoning 1067 applied to
/// its own nine rows: once the oracle was deleted D2's rule no longer
/// decides divergences with real consequences, and task 6326 is one: a consumer written against
/// either shape broke on the other, and the empty case is the one people
/// write their parser against first because it is the easy fixture. The
/// same rule was applied to `scope suggest --json` (6257) and confirmed on
/// `links list --json` (6270), which already behaved this way.
/// @param proposals The enriched proposals, in `detect_proposals` order.
/// @return The payload, EVERY line terminated including the last — this
/// renderer owns its terminators and the caller appends nothing. That is
/// what lets the empty case be genuinely empty rather than a lone newline,
/// and it is a DELIBERATE break from this file's other JSON renderers,
/// which are still fragments their caller terminates.
export auto render_detect_json(std::span<const proposal> proposals) -> std::string;

} // namespace planar::engine::identity
