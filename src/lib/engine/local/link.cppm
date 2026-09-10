/// @file link.cppm
/// @brief `planar.engine.local.link` — install, remove, list and reconcile the
/// vendor symlinks behind `planar local {link, unlink, list}` (plan 996, task
/// 6109).
///
/// Behavior-preserving port (D2) of zig/src/engine/local/link.zig.
///
/// ## HOME safety
///
/// Every function here takes an explicit `home_dir`. Nothing in this module
/// reads the environment, so it cannot find a real `~/.claude` on its own — see
/// manifest.cppm's header for why that matters more in this bucket than
/// anywhere else in the tree, and for the `PLANAR_LOCAL_HOME` trap it defuses.
///
/// ## Where an install lands, and the two DIFFERENT layouts
///
/// A skill installs once PER VENDOR, and the vendors do not agree on shape:
///
///     claude   <home>/.claude/commands/local-<name>.md   -> <source>/SKILL.md
///     codex    <home>/.codex/skills/local-<name>         -> <source>/        (a DIRECTORY link)
///     copilot  <home>/.copilot/skills/local-<name>       -> <source>/        (a DIRECTORY link)
///
/// claude gets a FILE link to `SKILL.md`; codex and copilot get a link to the
/// whole skill directory, so a skill's auxiliary files travel with it. All three
/// paths hang off the ONE sandbox root — `CODEX_HOME` is not consulted, verified
/// against the oracle with `CODEX_HOME` pointed elsewhere entirely.
///
/// An agent installs exactly ONCE, and not into a vendor directory at all:
///
///     agents   <home>/.planar/agents/local-<name>.md     -> <source>.md
///
/// with the literal vendor string `agents`. It also ignores the frontmatter
/// `vendors:` list completely — an agent naming `vendors: [claude]` still gets
/// this one target. Oracle-confirmed.
///
/// ## The `local-` prefix, and how `shadow:` removes it
///
/// Installs are normally named `local-<name>`, which is what makes an
/// operator-authored skill visibly distinguishable from a canonical one in the
/// vendor's own listing. `shadow: true` drops the prefix, so the install lands
/// as plain `<name>` and REPLACES any canonical install of that name. That is
/// destructive and deliberate; `lint()` warns about it and `link()` attaches a
/// per-record warning.
///
/// ## Symlink first, copy as a fallback
///
/// `install_one` always tries a symlink and only copies when the filesystem
/// refuses one (Windows without developer mode, some network mounts). A copy
/// gets its own warning, because the whole value proposition — "edit the source
/// and every vendor sees it" — silently stops holding.
///
/// Writes go to `<target>.planar-tmp` first and are renamed into place, so an
/// interrupted install cannot leave a half-written file where a working one was.
///
/// ## The four actions, and which ones reach the manifest
///
///   `created`    the target did not exist
///   `updated`    it existed and pointed somewhere else
///   `unchanged`  it was already a symlink to exactly this source
///   `skipped`    excluded by `--vendor`
///   `dry-run`    `--dry-run`; nothing touched the disk
///
/// `skipped` and `dry-run` records are EXCLUDED from the manifest — recording a
/// link that was never made would make `list` report a live install that does
/// not exist. Oracle-confirmed: after `local link --vendor codex`, the manifest
/// entry held ONLY the codex link, the other two vendors having been dropped
/// rather than retained from the previous run.
///
/// ## Three status words, and the one that is not what it looks like
///
/// `list` recomputes a status per record rather than trusting the manifest:
///
///   `live`     the symlink resolves to the recorded source, and the source exists
///   `broken`   it points somewhere else, OR the source is gone
///   `missing`  nothing is there at all
///
/// The trap is that a target which is NOT a symlink — a real file an operator
/// dropped in by hand — reports `live`, because the check falls back to plain
/// existence when `readlink` fails. That is the oracle's behavior and it is
/// pinned rather than corrected.

module;

export module planar.engine.local.link;

import std;
import planar.engine.local.manifest;

namespace planar::engine::local::link {

/// @brief Whether a target is a single file or a whole directory.
export enum class layout {
  flat,        ///< One file: claude skills, and every agent.
  dir_symlink, ///< The whole skill directory: codex and copilot.
};

/// @brief One place a source should be installed.
export struct target {
  std::string  vendor;                      ///< `claude` / `codex` / `copilot`, or `agents`.
  link::layout layout = link::layout::flat; ///< File or directory.
  std::string  target_path;                 ///< Where the install goes.
  std::string  link_target;                 ///< What it points at; EMPTY for an agent (see below).
};

/// @brief One install attempt's outcome, as reported and recorded.
export struct target_record {
  std::string                   vendor;      ///< As in target::vendor.
  std::string                   target_path; ///< Where it went.
  std::string                   source_path; ///< What it points at.
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

/// @brief Compute a source's install targets.
///
/// Skill targets are sorted by vendor, so output order does not depend on the
/// order the author listed them in.
///
/// An AGENT target's `link_target` is deliberately EMPTY. `link()` reads that
/// emptiness as "point at the source file itself" — the agent case has no
/// separate directory-versus-file distinction to encode, and the empty string is
/// how the Zig original signals it. Preserved because the value flows into the
/// manifest's `source_path` and is therefore observable.
/// @param home_dir The sandbox root's parent.
/// @param source_dir The directory containing the source; the skill directory
/// for a skill, unused for an agent.
/// @param name The source's name.
/// @param source_kind Skill or agent.
/// @param shadow Whether to drop the `local-` prefix.
/// @param vendors The vendors to install into; ignored entirely for an agent.
/// @return The targets, vendor-sorted for a skill, exactly one for an agent.
export auto targets(const std::filesystem::path& home_dir, const std::filesystem::path& source_dir, std::string_view name,
                    manifest::kind source_kind, bool shadow, std::span<const std::string> vendors) -> std::vector<target>;

/// @brief Options for link().
export struct link_options {
  std::filesystem::path      home_dir;           ///< The sandbox root's parent.
  bool                       dry_run = false;    ///< Report without touching disk.
  std::optional<std::string> vendor_filter;      ///< `--vendor`; non-matching targets are `skipped`.
  bool                       force_copy = false; ///< Skip the symlink attempt entirely.
  std::string                now;                ///< The `linked_at` stamp; see below.
};

/// @brief Install one source into every one of its targets.
///
/// `now` is passed IN rather than read from a clock. That keeps this function
/// deterministic and testable — the timestamp reaches both the manifest and the
/// JSON output, so a clock read inside here would make every byte-exactness
/// test unpinnable. The CLI supplies utc_now_stamp().
///
/// The manifest is rewritten unless `dry_run`, replacing this source's entry
/// wholesale — `skipped` and `dry-run` records are excluded, so a filtered run
/// NARROWS the recorded install set rather than merging into it.
/// @param file The parsed source.
/// @param options Where to install and how.
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
};

/// @brief Remove every recorded install of one source.
///
/// Driven entirely by the MANIFEST, not by a filesystem scan: an install the
/// manifest does not know about is not removed, and a manifest entry whose
/// target is already gone still produces a `removed` record. That asymmetry is
/// what makes `unlink` idempotent.
///
/// `purge` deletes the sandbox source too, and — importantly — reports its path
/// in `purged_file` EVEN IF the delete failed or the file was already gone. The
/// Zig original discards the delete's error (`catch {}`), so `purged_file` means
/// "this is the path purge targeted", not "this was deleted".
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
  link::target_record record;                       ///< The install, with `action` recomputed as its live status.
};

/// @brief Every recorded install across both kinds.
///
/// Sorted by (kind, name, vendor), so skills precede agents. Each record's
/// `action` is the LIVE status (`live` / `broken` / `missing`), recomputed from
/// disk rather than read from the manifest — see this module's header for the
/// non-symlink case that reports `live`.
/// @param home_dir The sandbox root's parent.
/// @return The rows, or nullopt when either manifest could not be read.
export auto list(const std::filesystem::path& home_dir) -> std::optional<std::vector<list_record>>;

/// @brief The live status of one recorded install (`live` / `broken` / `missing`).
///
/// Exposed so the status rules can be pinned directly, including the
/// non-symlink-reports-live case.
/// @param row The recorded install.
/// @return One of the three status words.
export auto classify_existing(const manifest::manifest_record& row) -> std::string_view;

/// @brief One stale manifest entry `link --reconcile` acted on.
export struct reconcile_action {
  std::string              name;                         ///< The source's name.
  manifest::kind           kind = manifest::kind::skill; ///< Which manifest.
  std::string              reason;                       ///< `source-missing` or `target-missing`.
  std::string              source_path;                  ///< The recorded source.
  std::vector<std::string> removed_targets;              ///< Removed OR repaired paths; see below.
};

/// @brief Options for reconcile().
export struct reconcile_options {
  std::filesystem::path home_dir;        ///< The sandbox root's parent.
  bool                  dry_run = false; ///< Report without touching disk.
  std::string           now;             ///< Stamp for repaired records; see link().
};

/// @brief Bring the manifest and the filesystem back into agreement.
///
/// TWO different repairs share one result shape, and the field names fit only
/// the first of them:
///
///   `source-missing` — the sandbox source is gone. Every install is DELETED and
///                      the entry is dropped from the manifest.
///   `target-missing` — the source is fine but an install is not `live`. The
///                      install is RE-CREATED and the entry is kept, with a
///                      fresh `mode` and `linked_at`.
///
/// In the second case `removed_targets` holds paths that were REPAIRED, not
/// removed, and the CLI still prints them under "removed N install(s)". That is
/// the oracle's wording and it is preserved rather than corrected — see
/// render.cppm.
/// @param options Where, and whether to act.
/// @return The actions taken, or nullopt when a manifest could not be read.
export auto reconcile(const reconcile_options& options) -> std::optional<std::vector<reconcile_action>>;

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
