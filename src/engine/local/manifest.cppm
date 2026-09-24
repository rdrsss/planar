/// @file manifest.cppm
/// @brief `planar.engine.local.manifest` — the operator-local sandbox's file
/// format: frontmatter parsing, the skills/agents walk, the flat->dir-shape
/// migration, and the per-kind link-manifest on disk (plan 996, task 6109).
///
/// Behavior-preserving port (D2) of zig/src/engine/local/manifest.zig and
/// zig/src/cmd/planar/handlers/local/common.zig.
///
/// ## HOME safety, and the trap this module exists to make impossible
///
/// `planar local *` writes symlinks into the operator's REAL vendor
/// directories — `~/.claude/commands`, `~/.codex/skills`, `~/.copilot/skills`.
/// Getting the root wrong does not produce a failing test; it silently edits
/// the developer's machine.
///
/// The Zig original resolves that root in `handlers/local/common.zig:11`:
///
///     PLANAR_LOCAL_HOME, else HOME, else error.HomeNotSet
///
/// **It never consults `PLANAR_HOME`.** Every other Planar surface does — the
/// workflow bucket's `dirs.resolve`, for one — so redirecting only
/// `PLANAR_HOME` (the obvious thing to do) leaves `local link` writing into the
/// real `~/.claude/commands`. That asymmetry is the single most dangerous fact
/// about this bucket.
///
/// It is also, empirically, NOT what the vendor-specific env vars suggest:
/// `CODEX_HOME` is irrelevant here. Oracle-verified with `CODEX_HOME` pointing
/// somewhere else entirely — the codex install still landed under
/// `$PLANAR_LOCAL_HOME/.codex/skills`. The vendor directories are derived from
/// the ONE sandbox root and nothing else.
///
/// This port removes the hazard structurally rather than by convention.
/// `resolve_home_and_root` takes an explicit `env_lookup` callable and every
/// other entry point in this bucket takes explicit absolute paths. No function
/// here calls `std::getenv`, so no caller — and no test — can reach a real home
/// by forgetting a variable. The CLI layer supplies the real environment; tests
/// supply a map over a scratch root.
///
/// ## Layout
///
///     <root>/.planar/local/skills/<name>/SKILL.md      a skill
///     <root>/.planar/local/agents/<name>.md            an agent
///     <root>/.planar/local/skills/.link-manifest.json  installs, per kind
///     <root>/.planar/local/agents/.link-manifest.json
///
/// where `<root>` is `$PLANAR_LOCAL_HOME` or `$HOME`. Note the manifest lives
/// beside the sources, one file per kind, NOT one file for the sandbox.
///
/// ## Frontmatter, as implemented rather than as documented
///
/// The block must open with a literal `---\n` at offset 0 and close with
/// `\n---\n` (or a trailing `\n---` at EOF). Inside it is a deliberately
/// minimal `key: value` reader, not a YAML parser:
///
///   - Recognised keys: description, argument-hint, tier, model, kind, shadow,
///     vendors. Everything else is silently ignored, as is any line with no
///     colon at all.
///   - A later duplicate key WINS; the earlier value is discarded.
///   - Values are trimmed of spaces/tabs, then ONE matching pair of surrounding
///     single or double quotes is stripped.
///   - `shadow` is true only for the exact string `true` after that trimming.
///     `True`, `yes` and `1` are all false.
///   - `vendors` accepts three forms: an inline flow list `[a, b]`, a bare
///     scalar `vendors: claude`, and a block list of following `-` lines.
///     Inline comments (`# ...`) are stripped from list items but NOT from
///     scalar values of the other keys — `description: x # y` keeps `# y`.
///   - The block-list mode ends at the first line that does not start with `-`,
///     which is why a blank line inside a vendors list does NOT end it (blank
///     lines are skipped before the check).
///
/// ## Validation is by DIRECTORY, not by declaration
///
/// A file's kind comes from where it lives. The frontmatter `kind:` field, when
/// present and non-empty, must AGREE with that or the file is rejected
/// (`KindMismatch`). An unknown vendor is `InvalidVendor`. Both are reported as
/// walk errors rather than aborting the walk.
///
/// ## Error names are part of the contract
///
/// `planar local link` prints walk failures as `warning: <path>: <ErrorName>`,
/// where `<ErrorName>` is Zig's `@errorName` — so the identifiers leak into
/// user-visible output and cannot be renamed. Oracle-captured:
///
///     warning: .../nofm/SKILL.md: NoFrontmatter
///     warning: .../nodashes/SKILL.md: MalformedFrontmatter
///     warning: .../badvendor/SKILL.md: InvalidVendor
///     warning: .../kindmm/SKILL.md: KindMismatch
///     warning: .../emptydir/SKILL.md: skill directory missing SKILL.md
///     warning: .../legacyflat.md: legacy flat skill file; run `planar local
///              migrate` to convert to legacyflat/SKILL.md
///
/// `parse_error_name` is the mapping, and it is why the enum's spellings match
/// Zig's rather than this tree's usual naming.

module;

export module planar.engine.local.manifest;

import std;

namespace planar::engine::local::manifest {

/// @brief Which sandbox directory a source lives in.
///
/// The enumerator ORDER is load-bearing: `local list` and `walk_sandbox` both
/// sort by kind first, and the oracle emits every skill before every agent.
/// Reversing these two lines would silently reorder both surfaces.
export enum class kind {
  skill, ///< `<root>/.planar/local/skills/<name>/SKILL.md`.
  agent, ///< `<root>/.planar/local/agents/<name>.md`.
};

/// @brief The three vendors a skill can be installed into, in sorted order.
///
/// This is also the DEFAULT set: a skill whose frontmatter names no vendors is
/// installed into all three. Oracle-confirmed — a skill with no `vendors:` key
/// produced claude, codex and copilot targets.
export inline constexpr std::array<std::string_view, 3> all_vendors{"claude", "codex", "copilot"};

/// @brief The per-kind link manifest's filename.
///
/// Lives INSIDE the sources directory it describes, which is why the sandbox
/// walk and the import collector both skip it by name.
export inline constexpr std::string_view manifest_filename = ".link-manifest.json";

/// @brief The recognised frontmatter fields of a sandbox source.
///
/// Every string field defaults to EMPTY rather than absent: the renderers'
/// contract is "an empty field is an empty string", and `std.json.Stringify`
/// emits `""` for them, so there is no unset state to represent.
export struct frontmatter {
  std::string              description;    ///< `description:`; surfaced by vendors as the summary.
  std::string              argument_hint;  ///< `argument-hint:`; note the HYPHEN in the key.
  std::string              tier;           ///< `tier:`; free text, not validated here.
  std::string              model;          ///< `model:`; free text, not validated here.
  std::string              kind;           ///< `kind:`; must agree with the directory when non-empty.
  bool                     shadow = false; ///< `shadow: true` exactly; drops the `local-` install prefix.
  std::vector<std::string> vendors;        ///< `vendors:`; EMPTY means "all three", not "none".
};

/// @brief The vendors a source actually installs into.
///
/// An empty `vendors` list means all three, NOT none — an empty list would make
/// the skill uninstallable, which is never what an author omitting the key
/// meant. The result is sorted bytewise either way, so target order does not
/// depend on the order the author wrote them in.
/// @param value The parsed frontmatter.
/// @return The vendor names, sorted.
export auto resolved_vendors(const frontmatter& value) -> std::vector<std::string>;

/// @brief Severity of a lint finding.
///
/// Only `warning` is ever produced today. `error_` exists because the Zig
/// original's enum has it and `local import` filters on it (`if (issue.severity
/// != .warning) continue;`) — a filter that is currently a no-op but would stop
/// working silently if the variant were dropped.
export enum class lint_severity {
  warning, ///< Advisory; never blocks an install.
  error_,  ///< Reserved; nothing emits this yet.
};

/// @brief One advisory finding about a source's frontmatter.
export struct lint_issue {
  lint_severity severity; ///< Always `warning` today.
  std::string   field;    ///< The frontmatter key at fault, e.g. `description`.
  std::string   message;  ///< Operator-facing prose.
};

/// @brief Lint one source's frontmatter.
///
/// Two findings, in this fixed order:
///   1. `description` empty — vendors surface it as the skill summary.
///   2. `shadow` true — the install lands as `<name>.md` with NO `local-`
///      prefix and may REPLACE a canonical install of the same name. This is
///      the destructive one, and the message names the exact filename.
/// @param value The parsed frontmatter.
/// @param name The source's name, interpolated into the shadow message.
/// @return The findings, possibly empty.
export auto lint(const frontmatter& value, std::string_view name) -> std::vector<lint_issue>;

/// @brief One parsed sandbox source.
export struct sandbox_file {
  std::string           source_path;                  ///< ABSOLUTE path to the `.md` file itself.
  std::string           name;                         ///< Skill: the containing directory. Agent: the stem.
  manifest::kind        kind = manifest::kind::skill; ///< Derived from the directory, never from frontmatter.
  manifest::frontmatter frontmatter;                  ///< The parsed block.
  std::string           body;                         ///< Everything after the closing `---`, less one leading newline.
};

/// @brief Why a source failed to parse.
///
/// The spellings deliberately mirror Zig's error names because
/// `parse_error_name` renders them into user-visible `warning:` lines.
export enum class parse_error {
  no_frontmatter,        ///< No `---\n` at offset 0.
  malformed_frontmatter, ///< Opened but never closed.
  invalid_vendor,        ///< A `vendors:` entry outside all_vendors.
  kind_mismatch,         ///< Frontmatter `kind:` disagrees with the directory.
  invalid_input,         ///< Not a `.md` file, or an unusable path.
  file_not_found,        ///< The file could not be read.
};

/// @brief The exact identifier the CLI prints for a parse failure.
///
/// These are Zig `@errorName` strings and are therefore FROZEN — they appear
/// verbatim in `planar local link`'s warning lines (see this module's header
/// for the oracle capture). Renaming one is a user-visible break.
/// @param value The failure.
/// @return A stable identifier, e.g. `NoFrontmatter`.
export auto parse_error_name(parse_error value) -> std::string_view;

/// @brief The short reason code `local import` reports for a parse failure.
///
/// A DIFFERENT vocabulary from parse_error_name(), and deliberately so: import
/// emits `skipped  reason: no-frontmatter` while link emits `NoFrontmatter`.
/// Anything not specifically classified collapses to `invalid-frontmatter`, so
/// `malformed_frontmatter`, `invalid_input` and `file_not_found` share one code.
/// @param value The failure.
/// @return A hyphenated reason code.
export auto parse_error_reason(parse_error value) -> std::string_view;

/// @brief Parse one sandbox source file.
///
/// `kind` is supplied by the CALLER from the directory being walked; it is not
/// read from the file. The file's own `kind:` field is only checked for
/// agreement.
///
/// The returned `name` is derived from the path, not the frontmatter: for a
/// file basenamed `SKILL.md` it is the containing directory's name, otherwise
/// the basename with `.md` removed. A non-`.md` file that is not `SKILL.md` is
/// `invalid_input`.
/// @param path Path to the `.md` file. Made absolute in the result.
/// @param file_kind The kind implied by the directory.
/// @return The parsed source, or why it failed.
export auto parse_file(const std::filesystem::path& path, kind file_kind) -> std::expected<sandbox_file, parse_error>;

/// @brief Split frontmatter out of already-read file bytes.
///
/// Exposed separately from parse_file() so the parsing rules can be pinned
/// without touching the filesystem at all.
/// @param content The whole file's bytes.
/// @return The frontmatter and the body, or why it failed. Vendor and kind
/// agreement are NOT checked here — parse_file() does that.
export auto split_frontmatter(std::string_view content) -> std::expected<std::pair<frontmatter, std::string>, parse_error>;

/// @brief One thing the walk could not use, reported rather than thrown.
///
/// A bad source never aborts the walk: `local link` prints each of these as a
/// `warning:` line and links everything that DID parse. That is deliberate —
/// one malformed personal skill must not block installing the other nine.
export struct walk_error {
  std::string path;    ///< The offending path.
  std::string message; ///< Either a parse_error_name() or a full sentence.
};

/// @brief Everything found under a sandbox root.
export struct walk_result {
  std::vector<sandbox_file> files;       ///< Parsed sources, sorted by (kind, name).
  std::vector<walk_error>   walk_errors; ///< Unusable entries, in DIRECTORY ORDER.
};

/// @brief Walk `<root>/skills` and `<root>/agents` for sources.
///
/// Both directories are optional: an absent one contributes nothing and is not
/// an error. Entries whose name begins with `.` are skipped everywhere, which
/// is what keeps `.link-manifest.json` out of the results.
///
/// Under `skills/`, a plain `*.md` FILE is not a skill — it is a legacy
/// flat-shape leftover, and it produces a walk error naming `local migrate`
/// (see migrate()). A subdirectory with no `SKILL.md` produces
/// `skill directory missing SKILL.md`.
///
/// ORDERING. `files` is sorted by kind then name, so it is stable. `walk_errors`
/// is NOT sorted — the Zig original appends in directory-iteration order and so
/// does this port. Iteration order is unspecified on both sides, so the ORDER of
/// warning lines is genuinely not a contract; the SET of them is. Tests here
/// assert membership, never position.
/// @param root The sandbox root, i.e. `<home>/.planar/local`.
/// @return Parsed sources and unusable entries.
export auto walk_sandbox(const std::filesystem::path& root) -> walk_result;

/// @brief One flat-shape skill considered by migrate().
export struct migrate_record {
  std::string name;     ///< The `.md` stem.
  std::string old_path; ///< `skills/<name>.md`.
  std::string new_path; ///< `skills/<name>/SKILL.md`.
  std::string reason;   ///< Empty when migrated; why not, when skipped.
};

/// @brief The outcome of a migrate() pass.
export struct migrate_result {
  std::vector<migrate_record> migrated; ///< Moved (or would be), sorted by name.
  std::vector<migrate_record> skipped;  ///< Left alone, sorted by name.
};

/// @brief Convert legacy flat `skills/<name>.md` files into `<name>/SKILL.md`.
///
/// Skips, without touching anything, when the destination directory already
/// exists — with two DIFFERENT reasons depending on what is in it:
///   - `SKILL.md` present: `already migrated: <new_path> exists`
///   - directory present but no `SKILL.md`:
///     `collision: <new_dir> exists but is not a matching skill dir`
///
/// An absent `skills/` directory is an empty result, not an error.
/// @param skills_root The `<root>/.planar/local` sandbox root.
/// @param dry_run When true, report the moves without performing them.
/// @return What moved and what did not.
export auto migrate(const std::filesystem::path& skills_root, bool dry_run) -> migrate_result;

/// @brief How an install was materialised.
export enum class mode {
  symlink, ///< A symlink to the sandbox source; edits propagate immediately.
  copy,    ///< A byte copy, used when the filesystem refused a symlink.
};

/// @brief One recorded install target in the on-disk link manifest.
export struct manifest_record {
  std::string    vendor;                         ///< `claude` / `codex` / `copilot`, or `agents` for an agent.
  std::string    target_path;                    ///< Where the install landed.
  std::string    source_path;                    ///< What it points at.
  manifest::mode mode = manifest::mode::symlink; ///< How it was materialised.
  std::string    linked_at;                      ///< `YYYY-MM-DDTHH:MM:SSZ`, second precision, always UTC.
};

/// @brief One source's installs, as recorded on disk.
export struct manifest_entry {
  std::string                  name;        ///< The source's name.
  std::string                  source_path; ///< The sandbox source it came from.
  std::vector<manifest_record> links;       ///< One per vendor actually installed.
};

/// @brief A whole per-kind link manifest.
export struct link_manifest {
  std::int64_t                version = 1; ///< Always 1; a 0 read back is normalised to 1.
  std::vector<manifest_entry> entries;     ///< In insertion order; NOT sorted on disk.
};

/// @brief Where a kind's link manifest lives.
/// @param home_dir The sandbox root's PARENT — i.e. `$PLANAR_LOCAL_HOME`, not
/// `<home>/.planar/local`. The `.planar/local/<kind>` suffix is added here.
/// @param manifest_kind Which manifest.
/// @return The absolute path.
export auto manifest_path(const std::filesystem::path& home_dir, kind manifest_kind) -> std::filesystem::path;

/// @brief Read a kind's link manifest.
///
/// A MISSING file is an empty manifest, not an error — that is the first-run
/// state and every caller depends on it. A file that exists but does not parse
/// IS an error, because silently discarding a real manifest would orphan every
/// install it records.
/// @param home_dir The sandbox root's parent.
/// @param manifest_kind Which manifest.
/// @return The manifest, empty if absent, or nullopt if present and unreadable.
export auto load_manifest(const std::filesystem::path& home_dir, kind manifest_kind) -> std::optional<link_manifest>;

/// @brief Write a kind's link manifest atomically.
///
/// Serialised with two-space indentation and a trailing newline, then written
/// to `<path>.tmp` and renamed over the target, so a crash mid-write cannot
/// leave a truncated manifest. Parent directories are created as needed.
/// @param home_dir The sandbox root's parent.
/// @param manifest_kind Which manifest.
/// @param value The manifest to write.
/// @return True on success.
export auto save_manifest(const std::filesystem::path& home_dir, kind manifest_kind, const link_manifest& value) -> bool;

/// @brief Serialise a manifest exactly as save_manifest() writes it.
///
/// Split out so the on-disk format can be pinned byte-for-byte without a
/// filesystem round trip.
/// @param value The manifest.
/// @return The file's complete bytes, trailing newline included.
export auto serialize_manifest(const link_manifest& value) -> std::string;

/// @brief Parse a manifest from bytes, as load_manifest() does.
/// @param content The file's bytes.
/// @return The manifest, or nullopt when it does not parse.
export auto parse_manifest(std::string_view content) -> std::optional<link_manifest>;

/// @brief The sandbox root and the directory it hangs off.
export struct home_and_root {
  std::filesystem::path home_dir;     ///< `$PLANAR_LOCAL_HOME` or `$HOME`.
  std::filesystem::path sandbox_root; ///< `<home_dir>/.planar/local`.
};

/// @brief Resolve the sandbox root from an EXPLICIT environment.
///
/// `PLANAR_LOCAL_HOME` first, then `HOME`, then FAILURE — deliberately NOT
/// `PLANAR_HOME`, which this surface has never consulted. See the module header
/// for why that asymmetry is the most dangerous fact in this bucket, and why
/// this function takes a callable instead of reading the process environment.
/// @param env_lookup Callable: `std::string_view -> std::optional<std::string>`.
/// @return The resolved roots, or nullopt when neither variable is set.
export auto resolve_home_and_root(const std::function<std::optional<std::string>(std::string_view)>& env_lookup)
    -> std::optional<home_and_root>;

/// @brief Guess a sandbox name's kind by looking for its source on disk.
///
/// Skills are checked FIRST, so a name existing as both resolves to the skill.
/// Used by `local unlink`, which falls back to unlinking BOTH kinds when this
/// returns nullopt — a source already deleted still has manifest entries to
/// clean up, and that is the case this exists to handle.
/// @param sandbox_root `<home>/.planar/local`.
/// @param name The name to classify.
/// @return The kind, or nullopt when neither source exists.
export auto lookup_kind_for_name(const std::filesystem::path& sandbox_root, std::string_view name) -> std::optional<kind>;

/// @brief Parse a `--kind` flag value.
///
/// Accepts singular and plural (`skill`/`skills`, `agent`/`agents`), and an
/// ABSENT flag defaults to `skill`. Anything else is rejected.
/// @param raw The flag's value, or nullopt when unset.
/// @return The kind, or nullopt when the value is unrecognised.
export auto parse_kind(std::optional<std::string_view> raw) -> std::optional<kind>;

/// @brief The wire spelling of a kind (`skill` / `agent`).
/// @param value The kind.
/// @return Its lowercase singular name.
export auto kind_name(kind value) -> std::string_view;

/// @brief The directory a kind's sources live in (`skills` / `agents`).
/// @param value The kind.
/// @return Its plural directory name.
export auto kind_dir(kind value) -> std::string_view;

/// @brief The wire spelling of a mode (`symlink` / `copy`).
/// @param value The mode.
/// @return Its lowercase name.
export auto mode_name(mode value) -> std::string_view;

} // namespace planar::engine::local::manifest
