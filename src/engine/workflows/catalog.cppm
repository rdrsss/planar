/// @file catalog.cppm
/// @brief `planar.engine.workflows.catalog` — discovery and `@meta` parsing
/// for the Lua workflow files behind `planar workflow list` and
/// `planar workflow show` (plan 996, task 6096).
///
/// Behavior-preserving port (D2) of
/// zig/src/cmd/planar/handlers/workflow/{dirs,meta,scan}.zig.
///
/// ## HOME safety is structural here, not a test convention
///
/// The Zig original reads `PLANAR_HOME` / `HOME` / `PLANAR_WORKFLOWS_DIR`
/// from the process environment inside `dirs.resolve`. This port does NOT:
/// `resolve_dirs` takes an explicit `env_lookup` callable and every other
/// entry point takes explicit absolute paths. A caller therefore CANNOT
/// accidentally reach the developer's real `~/.planar` — not because the
/// tests remember to redirect, but because this module has no way to find it
/// on its own. The CLI layer supplies the real environment; the tests supply
/// a map over a `std::filesystem::temp_directory_path()` scratch root.
///
/// ## Directory layout, oracle-verified
///
///   shipped: $PLANAR_WORKFLOWS_DIR, else $PLANAR_HOME/workflows/
///   sandbox: $PLANAR_HOME/local/workflows/
///   $PLANAR_HOME itself: $PLANAR_HOME, else $HOME/.planar, else /tmp/.planar
///
/// Neither directory is required to exist; an absent directory is an EMPTY
/// list, never an error. Confirmed against a `PLANAR_HOME` pointing at a
/// directory with no `workflows/` at all: `workflow list` exits 0 with
/// `no shipped + sandbox workflows found`.
///
/// ## The `@meta` block, and how it ACTUALLY parses
///
/// The documented syntax is a Lua long-comment at the top of the file:
///
///     --[[ @meta
///     name: finalize-closeout
///     description: Deterministic closeout gate.
///     phases: closeout
///     seam: planar run start/event/finish, planar plan closeout
///     --]]
///
/// The Zig module's own header comment says the block "MUST start on line 1
/// with `--[[ @meta` (no leading whitespace before the `--[[`)". BOTH halves
/// of that sentence are false in the shipped implementation, because it
/// tokenizes on '\n' (which SKIPS empty tokens) and then trims each line of
/// " \t\r". Verified by probing rather than by trusting the comment — every
/// one of these was accepted:
///
///   - two leading BLANK lines before `--[[ @meta`  -> name `leading-blanks`
///   - three spaces of indent before `--[[ @meta`   -> name `indented-open`
///   - CRLF line endings throughout                 -> name `crlf-name`
///   - a block never closed by `--]]` (parses to EOF) -> name `no-close`
///   - `name:    spaced-value   ` (value trimmed)   -> name `spaced-value`
///   - `description:` with nothing after the colon  -> an empty description
///
/// This port reproduces the implemented behavior, not the documented
/// behavior, and says so here so the next reader does not "fix" it back.
///
/// ## What is NOT ported
///
/// `workflow run` — the third `workflow` leaf. Determined EMPIRICALLY, per
/// the task brief's hazard 5, rather than assumed: it resolves the workflow
/// by name (this module's `find`, which IS ported) and then hands off to the
/// separate `planar-execute` binary. With that binary present on PATH,
/// `workflow run finalize-closeout --phase closeout` exits 0 printing `{}`,
/// and `workflow run mmm-sandbox --phase build` exits 1 with
/// `planar-execute: phase function not found: build` on STDERR — output from
/// planar-execute itself, not from `planar`.
///
/// So the boundary sits exactly at the process spawn: everything up to and
/// including name resolution is in this module, and the execution half needs
/// a process-spawn seam that does not exist anywhere in this tree (the same
/// reason `bench harvest` and `capture commits` were deferred in earlier
/// cycles). Deferred WITH its dependency. Its pre-spawn refusals are already
/// reachable and ARE covered here: `workflow run nope --phase x` fails with
/// `error: workflow 'nope' not found` (exit 1) BEFORE any spawn, and a
/// missing `--phase` is an exit-2 `error: required flag missing: --phase` at
/// the CLI layer.

module;

export module planar.engine.workflows.catalog;

import std;

namespace planar::engine::workflows::catalog {

/// @brief The four fields extracted from a `@meta` block.
///
/// All four default to EMPTY rather than unset: the renderer's contract is
/// "omit an empty field", and an empty string and an absent key are the same
/// state to every consumer. Unknown keys inside the block are silently
/// skipped — a workflow may carry annotations this version does not read.
export struct workflow_meta {
  std::string name;        ///< `name:`; overrides the filename stem when non-empty.
  std::string description; ///< `description:`; free text.
  std::string phases;      ///< `phases:`; a comma-separated list, kept as one string.
  std::string seam;        ///< `seam:`; a summary of what the workflow shells out to.
};

/// @brief A parsed `@meta` block plus whether one was present at all.
///
/// A missing block is NOT an error: legacy and sandbox workflows may omit it,
/// and they still run. `found` is reported separately so `workflow show` can
/// print `meta: absent` rather than silently showing four empty fields.
export struct parse_result {
  workflow_meta meta;          ///< The extracted fields; all empty when `found` is false.
  bool          found = false; ///< Whether a `--[[ @meta` opener was seen.
};

/// @brief Parse the leading `@meta` block out of a Lua workflow source.
///
/// Reproduces the shipped Zig behavior exactly, INCLUDING its tolerance of
/// leading blank lines, indentation, CRLF endings, and an unclosed block —
/// see this module's header for the probes establishing each. The opener must
/// match `--[[ @meta` exactly after trimming (case-sensitive).
/// @param content The whole file's bytes.
/// @return The extracted fields and whether an opener was found.
export auto parse_meta(std::string_view content) -> parse_result;

/// @brief One discovered `*.lua` workflow file.
export struct entry {
  std::string   path;               ///< Absolute path to the `.lua` file.
  std::string   filename;           ///< Base filename, e.g. `finalize_closeout.lua`.
  workflow_meta meta;               ///< Parsed `@meta` fields.
  bool          meta_found = false; ///< Whether a `@meta` block was present.
  bool          is_local   = false; ///< True when sourced from the sandbox directory.
};

/// @brief The name a workflow is displayed and matched under.
///
/// The `@meta` `name:` field when present and non-empty; otherwise the
/// filename with a trailing `.lua` removed. This is why the fixture file
/// `partial.lua` lists as `aaa-partial` while `bare.lua` lists as `bare`.
/// @param value The discovered entry.
/// @return The effective name; a view into `value`, valid while it lives.
export auto effective_name(const entry& value) -> std::string_view;

/// @brief Scan one directory for `*.lua` files and parse each one's `@meta`.
///
/// An absent or unreadable directory yields an EMPTY vector, never an error.
/// Non-`.lua` files and subdirectories are skipped. An individual file that
/// cannot be read still produces an entry, with `meta_found = false` — the
/// workflow exists even if its metadata could not be read, and hiding it
/// would be a worse answer than showing it bare.
///
/// Results are sorted by `effective_name` (bytewise), so output does not
/// depend on directory iteration order.
/// @param dir_path Absolute path to scan.
/// @param is_local Whether this directory is the sandbox.
/// @return Every `.lua` workflow found, sorted by effective name.
export auto scan(const std::filesystem::path& dir_path, bool is_local) -> std::vector<entry>;

/// @brief The two directories workflows are discovered from.
export struct directories {
  std::filesystem::path shipped; ///< `$PLANAR_WORKFLOWS_DIR` or `$PLANAR_HOME/workflows`.
  std::filesystem::path sandbox; ///< `$PLANAR_HOME/local/workflows`.
};

/// @brief Resolve both workflow directories from an EXPLICIT environment.
///
/// Takes a lookup callable rather than reading `std::getenv` itself. That is
/// the whole HOME-safety mechanism of this module — see the header. The
/// callable is invoked with a variable name and returns its value, or
/// `std::nullopt` when unset.
/// @param env_lookup Callable: `std::string_view -> std::optional<std::string>`.
/// @return The shipped and sandbox directories.
export auto resolve_dirs(const std::function<std::optional<std::string>(std::string_view)>& env_lookup) -> directories;

/// @brief Resolve `$PLANAR_HOME`, falling back to `$HOME/.planar`, then
/// `/tmp/.planar`.
///
/// The `/tmp` fallback is the Zig original's (`environ.getPosix("HOME")
/// orelse "/tmp"`), preserved deliberately: it means a process with NO home
/// at all still resolves somewhere harmless rather than to a relative path.
/// @param env_lookup Callable: `std::string_view -> std::optional<std::string>`.
/// @return The resolved Planar home directory.
export auto resolve_planar_home(const std::function<std::optional<std::string>(std::string_view)>& env_lookup)
    -> std::filesystem::path;

/// @brief List workflows across both directories.
///
/// The two directories are scanned and sorted INDEPENDENTLY, then
/// concatenated shipped-first — this is NOT a single merged sort, and the
/// difference is visible. Oracle-verified with a sandbox workflow named
/// `aaa-local` alongside shipped `aaa-partial` / `bare` / `finalize-closeout`:
/// `aaa-local` listed FOURTH, after every shipped entry, not first.
///
/// A name present in both directories is listed TWICE, shipped then local.
/// Confirmed — the same file copied into both showed two `finalize-closeout`
/// rows differing only in `kind`.
/// @param dirs The resolved directories.
/// @param local_only When true, the shipped directory is skipped entirely
/// (`workflow list --local`).
/// @return Shipped entries then sandbox entries, each block name-sorted.
export auto list(const directories& dirs, bool local_only) -> std::vector<entry>;

/// @brief Resolve one workflow by effective name, SHIPPED FIRST.
///
/// Search order matters when a name exists in both directories: the shipped
/// copy wins. Oracle-verified — with `finalize-closeout` present in both,
/// `workflow show finalize-closeout --json` reported
/// `"kind":"shipped"`. (Note this is the opposite of what "sandbox overrides"
/// would suggest, which is exactly why it is pinned rather than assumed.)
/// @param dirs The resolved directories.
/// @param name The effective name to match, compared exactly.
/// @return The matching entry, or `std::nullopt` when no workflow matches.
export auto find(const directories& dirs, std::string_view name) -> std::optional<entry>;

} // namespace planar::engine::workflows::catalog
