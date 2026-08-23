/// @file importer.cppm
/// @brief `planar.engine.local.import` — copy skills and agents from an
/// external path into the operator's sandbox, behind `planar local import`
/// (plan 996, task 6109).
///
/// Behavior-preserving port (D2) of zig/src/engine/local/import.zig.
///
/// ## HOME safety
///
/// `home_dir` is an explicit parameter; nothing here reads the environment. See
/// manifest.cppm's header for the `PLANAR_LOCAL_HOME` trap this defuses.
///
/// ## Three source shapes, disambiguated by what is on disk
///
/// The single positional argument is overloaded, and which meaning applies is
/// decided by probing the filesystem, not by a flag:
///
///   1. A DIRECTORY containing `SKILL.md`, with `--kind skill` — ONE skill,
///      named for the directory. Checked first.
///   2. Any other DIRECTORY — a COLLECTION. Every `*.md` file becomes an entry
///      (named for its stem), and for `--kind skill` every subdirectory holding
///      a `SKILL.md` becomes one too. Entries are name-sorted.
///   3. A FILE — one entry, named for the stem. Must end in `.md`.
///
/// The order matters: a directory named `foo/` holding `SKILL.md` imports as the
/// single skill `foo`, NOT as a collection that happens to contain a file called
/// `SKILL.md`. Shape 1 is only tried for `--kind skill`, so the same directory
/// under `--kind agent` falls through to shape 2 and yields the entry `SKILL`.
///
/// Collections skip dot-prefixed names and `.link-manifest.json` by name.
/// Subdirectories are ignored entirely for `--kind agent`.
///
/// ## Nothing to import is an ERROR, not an empty result
///
/// An empty collection — or a non-`.md` file — fails outright rather than
/// returning zero records. The CLI turns that into
/// `error: importing <path> failed: NotFound` (or `InvalidInput`), which is why
/// `local import` on an empty directory is a non-zero exit while `local link`
/// on an empty sandbox is a cheerful exit 0. The two leaves genuinely disagree
/// and both spellings are pinned.
///
/// ## Collisions are skipped, not overwritten
///
/// An existing destination is `skipped` with reason `name-collision` unless
/// `--force`. With `--force` the action becomes `overwrote` rather than
/// `imported`, so the operator can see from the output which entries replaced
/// something.
///
/// For a directory-shaped skill, `--force` DELETES the destination tree before
/// copying. That is a genuine data-loss path: files present in the old skill and
/// absent from the new one are gone, not merged.
///
/// ## Warnings survive a skip; parse failures do not warn
///
/// Lint warnings are collected from every entry that PARSED, including ones
/// subsequently skipped for a collision. An entry that failed to parse produces
/// a `skipped` record with a reason code and no warnings — there is no
/// frontmatter to lint.

module;

export module planar.engine.local.importer;

import std;
import planar.engine.local.manifest;

namespace planar::engine::local::import_ {

/// @brief One import attempt's outcome.
export struct record {
  std::string name;        ///< The entry's name.
  std::string source_path; ///< The `.md` file it came from.
  std::string target_path; ///< Where it landed; EMPTY when parsing failed.
  std::string action;      ///< `imported` / `overwrote` / `would-import` / `skipped`.
  std::string reason;      ///< Why it was skipped; empty otherwise.
};

/// @brief One lint finding, tagged with the entry it came from.
export struct warning {
  std::string name;    ///< The entry's name.
  std::string field;   ///< The frontmatter key at fault.
  std::string message; ///< Operator-facing prose.
};

/// @brief What an import pass did.
export struct result {
  std::vector<record>  imported; ///< Copied (or would be), sorted by name.
  std::vector<record>  skipped;  ///< Not copied, sorted by name.
  std::vector<warning> warnings; ///< Lint findings, sorted by name.
};

/// @brief Why an import could not run at all.
///
/// Distinct from a per-entry skip: these abort the whole pass before anything
/// is copied.
export enum class import_error {
  invalid_input, ///< Empty arguments, or a source file that is not `.md`.
  not_found,     ///< The source resolved to zero importable entries.
};

/// @brief Options for import().
export struct options {
  std::filesystem::path home_dir;                        ///< The sandbox root's parent.
  std::filesystem::path source_path;                     ///< The file or directory to import from.
  manifest::kind        kind    = manifest::kind::skill; ///< Which sandbox directory to write into.
  bool                  force   = false;                 ///< Overwrite an existing destination.
  bool                  dry_run = false;                 ///< Report without copying.
};

/// @brief Copy sources into `<home>/.planar/local/{skills,agents}/`.
///
/// Skills land as `skills/<name>/SKILL.md` — a flat source file is promoted into
/// directory shape on the way in, so `local migrate` never has to run on
/// something imported. Agents land as `agents/<name>.md`.
/// @param opts Where from, where to, and how.
/// @return The outcome, or why the pass could not run.
export auto import_sources(const options& opts) -> std::expected<result, import_error>;

/// @brief The destination path an entry of `name` would occupy.
///
/// Exposed so the skill-promotion rule (`<name>/SKILL.md`, not `<name>.md`) can
/// be pinned without a filesystem round trip.
/// @param dest_root `<home>/.planar/local/{skills,agents}`.
/// @param name The entry's name.
/// @param entry_kind Skill or agent.
/// @return The absolute destination.
export auto dest_path(const std::filesystem::path& dest_root, std::string_view name, manifest::kind entry_kind)
    -> std::filesystem::path;

} // namespace planar::engine::local::import_
