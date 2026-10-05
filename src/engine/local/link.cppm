/// @file link.cppm
/// @brief `planar.engine.local.link` — project, remove, list and reconcile the
/// vendor copies behind `planar local {link, unlink, list}` (plan 996 task
/// 6109, retargeted by plan 1104 task 7220).
///
/// ## HOME safety
///
/// Every function here takes an explicit `home_dir`. Nothing in this module
/// reads the environment, so it cannot find a real `~/.claude` on its own — see
/// manifest.cppm's header for why that matters more in this bucket than
/// anywhere else in the tree, and for the `PLANAR_LOCAL_HOME` trap it defuses.
/// The one environment-derived input, an explicit `CODEX_HOME`, arrives as
/// `codex_home` from the caller.
///
/// ## Where a projection lands
///
/// A local source is PROJECTED, never linked: the destination holds a copy, and
/// the operator's source under `~/.planar/local/` is never edited. Every
/// destination is named `planar-local-<name>`.
///
/// A skill is a copy of its whole directory with the frontmatter `name`
/// rewritten to `planar-local-<name>` (the Agent Skills spec requires name ==
/// directory):
///
///     claude       <home>/.claude/skills/planar-local-<name>/
///     agents       <home>/.agents/skills/planar-local-<name>/        shared root
///     antigravity  <home>/.gemini/antigravity-cli/skills/planar-local-<name>/
///
/// An agent is one file per present vendor:
///
///     claude       <home>/.claude/agents/planar-local-<name>.md
///     gemini       <home>/.gemini/agents/planar-local-<name>.md
///     antigravity  <home>/.gemini/antigravity-cli/agents/planar-local-<name>.md
///     copilot      <home>/.copilot/agents/planar-local-<name>.agent.md
///     opencode     <home>/.config/opencode/agents/planar-local-<name>.md   (reduced frontmatter)
///     codex        <codex_home or <home>/.codex>/agents/planar-local-<name>.toml
///
/// The Markdown forms carry the source with `name` rewritten (or added). The
/// OpenCode form is the installer's `opencode_derive`: frontmatter reduced to
/// `description` and `mode: subagent`, body unchanged. The Codex form is the
/// three-key TOML of `scripts/render-codex-agents.py` (`name`, `description`,
/// `developer_instructions`, every value a fully escaped basic string),
/// reimplemented here so the engine does not depend on a checkout; a test
/// compares it byte for byte with the script on the shipped agents.
///
/// ## Presence rule
///
/// A vendor directory is written only when its marker exists, as in
/// `install.sh`: `~/.claude/`; `~/.codex/` or an explicit `CODEX_HOME`;
/// `~/.copilot/`; `~/.gemini/settings.json`; `~/.gemini/antigravity-cli/`;
/// `~/.config/opencode/`. The shared `~/.agents/skills` root is written when
/// Codex, Copilot, Gemini CLI or OpenCode is present. A skill's `vendors:`
/// list narrows skills only: `claude` gates the Claude copy, `codex` and
/// `copilot` gate the shared and Antigravity copies. An agent ignores it.
///
/// ## Ownership rule
///
/// A destination that exists and differs from the fresh projection is replaced
/// only when it is a prior Planar projection: recorded in a link manifest, or a
/// symlink into `<home>/.planar/local/`. Anything else is foreign and is
/// refused (see find_conflict()); a byte-identical destination is simply
/// unchanged.
///
/// ## Liveness
///
/// `list` recomputes a state per record from disk:
///
///   `live`     the installed copy equals a fresh projection of the source
///   `stale`    the copy exists and differs (the source or a copy was edited)
///   `missing`  nothing is at the destination
///   `broken`   the recorded source is gone
///   `legacy`   an OLD projection (symlink or copy under `~/.claude/commands`,
///              `~/.codex/skills`, `~/.copilot/skills`, `~/.planar/agents`);
///              `link --reconcile` migrates it
///
/// ## Reconcile
///
/// reconcile() makes the disk match the sources: it writes absent projections,
/// refreshes stale ones, removes the legacy projections it owns (symlinks into
/// the sandbox, or copies equal to the recorded source), and drops manifest
/// entries whose source is gone. It compares bytes before writing, so a second
/// run changes nothing, and it checks every destination for ownership before
/// it writes any of them.
///
/// ## Actions
///
///   `created`    the destination did not exist
///   `updated`    it existed, was a prior projection, and differed
///   `unchanged`  it already equalled the projection
///   `skipped`    excluded by `--vendor`, or the source cannot take this form
///   `dry-run`    `--dry-run`; nothing touched the disk
///
/// `skipped` and `dry-run` records are EXCLUDED from the manifest, so a
/// filtered run NARROWS the recorded set rather than merging into it.

module;

export module planar.engine.local.link;

import std;
import planar.engine.local.manifest;

namespace planar::engine::local::link {

/// @brief The prefix every projection's name and directory carries.
export inline constexpr std::string_view projection_prefix = "planar-local-";

/// @brief The name a source is projected under.
/// @param name The source's name.
/// @return `planar-local-<name>`.
export auto projected_name(std::string_view name) -> std::string;

/// @brief What a destination holds.
export enum class layout {
  skill_dir,      ///< A copy of the whole skill directory, `SKILL.md` name rewritten.
  agent_md,       ///< One Markdown file: the source with `name` rewritten (Claude, Gemini, Antigravity).
  agent_copilot,  ///< As agent_md, named `*.agent.md`.
  agent_opencode, ///< One Markdown file with the frontmatter reduced to `description` and `mode: subagent`.
  agent_toml,     ///< One Codex TOML file: `name`, `description`, `developer_instructions`.
};

/// @brief One place a source should be projected.
export struct target {
  std::string  vendor;                           ///< `claude`, `shared` (the shared skills root), `antigravity`, `gemini`, ...
  link::layout layout = link::layout::skill_dir; ///< What the destination holds.
  std::string  target_path;                      ///< Where the projection goes.
  std::string  link_target;                      ///< The source path recorded for it.
};

/// @brief One projection attempt's outcome, as reported and recorded.
export struct target_record {
  std::string                   vendor;      ///< As in target::vendor.
  std::string                   target_path; ///< Where it went.
  std::string                   source_path; ///< The sandbox source it was projected from.
  std::optional<manifest::mode> mode;        ///< NULLOPT for `skipped`; renders as `""`.
  std::string                   action;      ///< created/updated/unchanged/skipped/dry-run/removed.
  std::string                   warning;     ///< Empty when there is nothing to say.
  std::string                   linked_at;   ///< Empty for skipped and dry-run.
};

/// @brief What `link()` did for one source.
export struct link_result {
  std::string                name;                         ///< The source's name.
  manifest::kind             kind = manifest::kind::skill; ///< Its kind.
  std::vector<target_record> records;                      ///< One per target, INCLUDING skipped ones.
};

/// @brief Replace (or add) the frontmatter `name` of a Markdown source.
///
/// Only the `name:` lines at column 0 inside the leading frontmatter block are
/// touched; every other byte is preserved. A block with no `name:` gains one as
/// its first line. Content with no frontmatter is returned unchanged.
/// @param content The source's bytes.
/// @param name The value to write.
/// @return The rewritten bytes.
export auto rewrite_name(std::string_view content, std::string_view name) -> std::string;

/// @brief The OpenCode form of an agent, as `install.sh`'s `opencode_derive` prints it.
///
/// Frontmatter reduced to `description` and `mode: subagent`; the body is
/// unchanged and ends in a newline. A description YAML would misread is
/// double-quoted.
/// @param content The agent source's bytes.
/// @return The derived bytes, or nullopt when there is no closed frontmatter or no description.
export auto opencode_form(std::string_view content) -> std::optional<std::string>;

/// @brief The Codex TOML form of an agent, as `scripts/render-codex-agents.py` writes it.
///
/// `name`, `description` and `developer_instructions` (the Markdown after the
/// closing `---` with at most one leading newline removed), each a one-line
/// fully escaped TOML basic string.
/// @param name The value for `name`.
/// @param content The agent source's bytes.
/// @return The file's bytes, or nullopt when there is no closed frontmatter or no description.
export auto codex_toml(std::string_view name, std::string_view content) -> std::optional<std::string>;

/// @brief Where a source is projected, and from what environment.
export struct target_query {
  std::filesystem::path                home_dir;    ///< The sandbox root's parent.
  std::optional<std::filesystem::path> codex_home;  ///< An explicit `CODEX_HOME`; also Codex's presence marker.
  std::string                          source_path; ///< The source file, recorded in each target.
  std::string                          name;        ///< The source's name.
  manifest::kind                       source_kind = manifest::kind::skill; ///< Skill or agent.
  std::vector<std::string>             vendors;                             ///< Resolved `vendors:`; consulted for skills only.
};

/// @brief Compute a source's projection targets for the vendors present.
///
/// Sorted by vendor, so output order does not depend on filesystem or author
/// order. Vendors whose presence marker is absent contribute nothing.
/// @param query The home, source and vendors.
/// @return The targets; empty for an empty home or name.
export auto targets(const target_query& query) -> std::vector<target>;

/// @brief Whether a record's vendor is selected by a `--vendor` filter.
///
/// An exact match, or the shared skills root (`shared`) for a vendor that reads
/// it: `codex`, `copilot`, `gemini` and `opencode`.
/// @param record_vendor The record's vendor.
/// @param filter The operator's filter.
/// @return True when selected.
export auto vendor_matches(std::string_view record_vendor, std::string_view filter) -> bool;

/// @brief Options for link().
export struct link_options {
  std::filesystem::path                home_dir;        ///< The sandbox root's parent.
  bool                                 dry_run = false; ///< Report without touching disk.
  std::optional<std::string>           vendor_filter;   ///< `--vendor`; non-matching targets are `skipped`.
  std::string                          now;             ///< The `linked_at` stamp; see below.
  std::optional<std::filesystem::path> codex_home;      ///< An explicit `CODEX_HOME`.
};

/// @brief Find a destination link() would refuse to overwrite.
///
/// Checks every projection target of every source: one that exists, differs
/// from the fresh projection, and is not a prior Planar projection (recorded in
/// a link manifest, or a symlink into the sandbox) is foreign. Callers run this
/// over the whole batch before writing anything.
/// @param files The parsed sources.
/// @param options Where, and the vendor filter.
/// @return The offending path (a file inside a skill copy when that is what
/// differs), or nullopt when every destination is safe.
export auto find_conflict(std::span<const manifest::sandbox_file> files, const link_options& options)
    -> std::optional<std::string>;

/// @brief Project one source into every one of its targets.
///
/// `now` is passed IN rather than read from a clock, which keeps this function
/// deterministic: the timestamp reaches both the manifest and the JSON output.
/// The CLI supplies utc_now_stamp().
///
/// A destination that find_conflict() would flag is left untouched and
/// reported as `refused`. The manifest is rewritten unless `dry_run` (and
/// unless its bytes would not change), replacing this source's entry wholesale.
/// An `unchanged` record keeps the stamp it already had.
/// @param file The parsed source.
/// @param options Where to project and how.
/// @return One record per target, in target order.
export auto link(const manifest::sandbox_file& file, const link_options& options) -> link_result;

/// @brief Options for unlink().
export struct unlink_options {
  std::filesystem::path home_dir;      ///< The sandbox root's parent.
  bool                  purge = false; ///< Also delete the sandbox SOURCE.
};

/// @brief What `unlink()` did.
export struct unlink_result {
  std::string                name;                         ///< The name unlinked.
  manifest::kind             kind = manifest::kind::skill; ///< Which manifest was touched.
  std::vector<target_record> removed;                      ///< One per removed install, action `removed`.
  std::string                purged_file;                  ///< The deleted source path; empty without `--purge`.
  std::vector<target_record> skipped; ///< Recorded installs left in place because the operator replaced them; action `skipped`.
};

/// @brief Remove every recorded projection of one source.
///
/// Driven entirely by the MANIFEST, not by a filesystem scan: a projection the
/// manifest does not know about is not removed, and a manifest entry whose
/// target is already gone still produces a `removed` record. That asymmetry is
/// what makes `unlink` idempotent.
///
/// `purge` deletes the sandbox source too, and reports its path in
/// `purged_file` EVEN IF the delete failed or the file was already gone: it
/// means "this is the path purge targeted", not "this was deleted".
/// @param name The source to unlink.
/// @param unlink_kind Which manifest to read.
/// @param options Where, and whether to purge.
/// @return The removals, or nullopt when the manifest could not be read.
export auto unlink(std::string_view name, manifest::kind unlink_kind, const unlink_options& options)
    -> std::optional<unlink_result>;

/// @brief One row of `local list`.
export struct list_record {
  std::string         name;                         ///< The source's name.
  manifest::kind      kind = manifest::kind::skill; ///< Which manifest it came from.
  link::target_record record;                       ///< The projection, with `action` recomputed as its state.
};

/// @brief Every recorded projection across both kinds.
///
/// Sorted by (kind, name, vendor), so skills precede agents. Each record's
/// `action` is the LIVE state (`live` / `stale` / `missing` / `broken` /
/// `legacy`), recomputed from disk rather than read from the manifest.
/// @param home_dir The sandbox root's parent.
/// @return The rows, or nullopt when either manifest could not be read.
export auto list(const std::filesystem::path& home_dir) -> std::optional<std::vector<list_record>>;

/// @brief Whether a recorded install is an OLD projection (pre plan 1104).
///
/// Old projections are symlinks, or live under `~/.claude/commands`,
/// `~/.codex/skills`, `~/.copilot/skills` or `~/.planar/agents` as `local-*`.
/// @param row The recorded install.
/// @return True for a legacy projection.
export auto is_legacy(const manifest::manifest_record& row) -> bool;

/// @brief The live state of one recorded projection.
///
/// Exposed so the state rules can be pinned directly.
/// @param source_kind Whether the record belongs to a skill or an agent.
/// @param name The source's name.
/// @param row The recorded install.
/// @return `live`, `stale`, `missing`, `broken` or `legacy`.
export auto classify_existing(manifest::kind source_kind, std::string_view name, const manifest::manifest_record& row)
    -> std::string_view;

/// @brief One group of changes `link --reconcile` made (or would make).
export struct reconcile_action {
  std::string              name;                         ///< The source's name.
  manifest::kind           kind = manifest::kind::skill; ///< Which manifest.
  std::string              reason;      ///< `source-missing`, `legacy`, `stale`, `target-missing`, `not-owned` or `write-failed`.
  std::string              source_path; ///< The recorded or walked source.
  std::vector<std::string> removed_targets; ///< The paths changed: removed for `source-missing`/`legacy`, written otherwise.
};

/// @brief Options for reconcile().
export struct reconcile_options {
  std::filesystem::path                home_dir;        ///< The sandbox root's parent.
  bool                                 dry_run = false; ///< Report without touching disk.
  std::string                          now;             ///< Stamp for written records; see link().
  std::optional<std::filesystem::path> codex_home;      ///< An explicit `CODEX_HOME`.
};

/// @brief Why reconcile() did nothing.
export struct reconcile_error {
  /// @brief The failure class.
  enum class cause {
    manifest_unreadable, ///< A link manifest exists but does not parse.
    conflict,            ///< A destination is foreign; see `path`.
  };
  cause       why = cause::manifest_unreadable; ///< The failure class.
  std::string path;                             ///< The foreign destination, for `conflict`.
};

/// @brief Bring the projections, the sandbox and the manifest into agreement.
///
/// Four kinds of change, one group per (source, reason):
///
///   `source-missing` — the sandbox source is gone: its recorded projections are
///                      deleted and the entry dropped.
///   `legacy`         — an old projection the source still has is removed when
///                      owned (a symlink into the sandbox, or a copy equal to
///                      the recorded source); a foreign one is left alone.
///   `stale`          — a prior projection differs from a fresh one: rewritten.
///   `target-missing` — a projection is absent: written.
///   `not-owned`      — a `source-missing` projection the operator has since
///                      replaced with their own content: left in place.
///   `write-failed`   — a projection could not be written: not recorded, and the
///                      caller should treat the run as failed.
///
/// Nothing is written until every destination has been checked for ownership.
/// A second run with nothing to change returns no actions and writes nothing.
/// @param options Where, and whether to act.
/// @return The actions taken, or why nothing was done.
export auto reconcile(const reconcile_options& options) -> std::expected<std::vector<reconcile_action>, reconcile_error>;

/// @brief The current UTC time as `YYYY-MM-DDTHH:MM:SSZ`.
///
/// Second precision, always `Z`, never a local offset. The one clock read in
/// this bucket, kept out of link()/reconcile() so those stay deterministic.
/// @return The formatted stamp.
export auto utc_now_stamp() -> std::string;

/// @brief Format a UTC timestamp for tests, from a Unix epoch second count.
///
/// Split from utc_now_stamp() so the formatting can be pinned without a clock.
/// @param epoch_seconds Seconds since 1970-01-01T00:00:00Z; negative yields an
/// empty string, matching the Zig original's refusal.
/// @return The formatted stamp, or empty.
export auto format_utc_stamp(std::int64_t epoch_seconds) -> std::string;

} // namespace planar::engine::local::link
