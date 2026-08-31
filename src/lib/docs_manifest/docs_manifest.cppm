/// @file docs_manifest.cppm
/// @brief `planar.docs_manifest` — the `.manifest-docs` xxh64 merkle digest
/// (plan 996, task 6364).
///
/// Behavior-preserving port (D2) of the `build()` / `write()` / `normalize()`
/// half of zig/src/engine/docs/manifest.zig — the half `workspace regenerate`
/// (zig/src/engine/workspace/regenerate.zig) actually calls.
///
/// ## Layer 1, not `engine_docs`
///
/// The Zig original lives in its own `engine/docs/` bucket and is reached
/// from `engine/workspace/regenerate.zig` via a plain relative import — Zig
/// has no layering rule to violate. D15/D18/D20 (this tree's own layering
/// contract, `cmake/architecture.cmake`) forbid an `engine_* -> engine_*`
/// edge, so standing this up as `engine_docs` and depending on it from
/// `engine_workspace` would FATAL the layer walk. It is a pure content-hash
/// utility over a directory of markdown files — no SQLite handle, no
/// environment lookup, no scope resolution — so it fits the same
/// `src/lib/<name>/` shape as `json_text` and `scope_ref`: a layer-1 base
/// library, extracted downward per D19, one `engine_*` consumer today and
/// available to any future one without a second copy.
///
/// ## What is deliberately NOT here
///
/// The oracle's `manifest.zig` also exports `load`, `diff`, `verify`, and a
/// `SourceOverlay` mechanism that lets a caller attach provenance rows to a
/// doc before hashing it. `regenerate()` calls none of them — it calls
/// `manifest.build(layout.dir, allocator)`, the zero-overlay form, and
/// `manifest.write`. No ported call site reaches the other four, so they
/// are not transcribed; a future leaf that needs drift detection
/// (`diff`/`verify`) or documenter provenance (`SourceOverlay`) gets them
/// then, against its own oracle capture, rather than carried here unused
/// and untested. Every `entry`'s `sources` is therefore always empty in
/// this port, which is the zero-overlay oracle behavior exactly.
///
/// ## `generated_at` is the literal string `"now"`, not a timestamp
///
/// This looks like a bug and IS one, and D2 requires reproducing it exactly
/// rather than silently fixing it forward:
///
/// ```zig
/// return .{
///     .version = version,
///     .algo = try allocator.dupe(u8, algo),
///     .root = root_hash,
///     .generated_at = try allocator.dupe(u8, "now"),   // <-- literal
///     .entries = try rows.toOwnedSlice(allocator),
/// };
/// ```
///
/// Every `.manifest-docs` file the oracle ever writes carries the four ASCII
/// bytes `now` in its `generated_at` field, never an actual timestamp. This
/// port reproduces that byte-for-byte; see `build()` below and its unit
/// test. Flagged for a follow-up task row rather than fixed here — fixing it
/// would silently change the manifest format's contract for anything that
/// reads `generated_at` expecting oracle parity.
///
/// ## Entry paths are NOT relativized
///
/// The oracle's `walkMarkdown(root, root, ...)` builds each entry's `path`
/// by joining `dir_path` (starting equal to `root`) with the file name, and
/// stores that joined string directly — despite the local variable being
/// named `rel`, it is the FULL path passed to `build()`, unchanged. Since
/// `regenerate()` calls `build(layout.dir, ...)` with an absolute directory,
/// every manifest entry key is an absolute path. Reproduced as observed;
/// `build()` below takes the same un-relativized approach.
module;

export module planar.docs_manifest;

import std;

namespace planar::docs_manifest {

/// @brief The digest algorithm name stamped into every manifest — always
/// `"xxh64"`. Not a format choice this module makes; a fixed oracle constant.
export inline constexpr std::string_view algo = "xxh64";

/// @brief The manifest format version stamped into every manifest.
export inline constexpr std::int64_t version = 1;

/// @brief The canonical on-disk file name a manifest is written to,
/// relative to the directory it describes: `.manifest-docs`.
export inline constexpr std::string_view file_name = ".manifest-docs";

/// @brief One provenance row inside an entry's `sources` map. Always empty
/// in this port — see this file's header.
export struct source_row {
  std::string ref;  ///< The source's reference key.
  std::string hash; ///< The source's own digest.
};

/// @brief One markdown file's computed digests.
export struct entry {
  std::string             doc_hash;     ///< xxh64 of the NORMALIZED file body.
  std::vector<source_row> sources;      ///< Always empty; see this file's header.
  std::string             sources_hash; ///< xxh64 of the canonical (empty) sources JSON.
  std::string             entry_hash;   ///< xxh64 of `"<doc_hash>|<sources_hash>"`.
};

/// @brief One manifest row: a file path paired with its computed `entry`.
export struct entry_row {
  std::string path; ///< The file's path exactly as passed to `build()`'s root, un-relativized.
  entry       value;
};

/// @brief A built `.manifest-docs` document.
export struct manifest {
  std::int64_t           version = 0;  ///< Always `docs_manifest::version` (the namespace constant, shadowed here).
  std::string            algo;         ///< Always `docs_manifest::algo` (the namespace constant, shadowed here).
  std::string            root;         ///< xxh64 over every entry's `path\0entry_hash\n`, sorted by path.
  std::string            generated_at; ///< Always the literal string `"now"` — see this file's header.
  std::vector<entry_row> entries;      ///< Sorted ascending by `path`, byte-wise.
};

/// @brief Fold `\r\n` and lone `\r` to `\n`, strip a `regenerated_at:` /
/// `source_versions:` line from a `---`-delimited front-matter block if
/// present, right-trim every line, collapse trailing blank lines to one
/// newline, and guarantee a trailing newline.
///
/// Behavior-preserving port of `manifest.zig`'s `normalize()`. This is what
/// `build()` hashes — never the file's raw bytes — so two files differing
/// only in line-ending style or trailing whitespace hash identically.
/// @param content The raw file bytes.
/// @return The normalized text.
export auto normalize(std::string_view content) -> std::string;

/// @brief xxh64 of `content`, lowercase hex, zero-padded to 16 digits.
/// @param content The bytes to hash.
/// @return The hex digest.
export auto hash_hex(std::string_view content) -> std::string;

/// @brief Recursively hash every `*.md` file (case-insensitive extension)
/// under `root`, in the zero-overlay form `regenerate()` uses.
///
/// Directory order does not matter to the RESULT — entries are sorted by
/// path before `root` is computed — but note `root` itself is an absolute
/// path if the caller passes one; see this file's header.
/// @param root The directory to walk. Read via `std::filesystem`.
/// @return The built manifest, or `std::nullopt` if the directory could not
/// be opened or a file inside it could not be read.
export auto build(const std::filesystem::path& root) -> std::optional<manifest>;

/// @brief Write a manifest to `path` as the exact hand-rolled JSON shape
/// the oracle's `manifest.write` emits (D2 — not `std::json`'s default
/// encoding; this tree's `planar.json_text` escape table, matching field
/// order, no incidental whitespace beyond the oracle's own).
/// @param path Destination file path.
/// @param value The manifest to serialize.
/// @return True on success.
export auto write(const std::filesystem::path& path, const manifest& value) -> bool;

} // namespace planar::docs_manifest
