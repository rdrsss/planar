/// @file templates.cppm
/// @brief `planar.engine.config.templates` — disk + embedded template
/// resolution (tech-spec § "engine buckets", plan 996 task 6032).
///
/// Behavior-preserving port (D2) of `zig/src/engine/templates/loader.zig`'s
/// resolution chain for `load`:
///
///   1. `<root>/<set>/<system>/<kind>.json`   (user-chosen set on disk)
///   2. `<root>/default/<system>/<kind>.json` (baseline set on disk)
///   3. embedded defaults                     (always present in the binary)
///
/// The first readable file wins (JSON validity is NOT re-checked here —
/// zig's `Template.fields` decode step and the `render`/`validate`
/// surfaces that consume it are a separate, not-yet-ported task; this
/// task's scope is resolution, not template rendering).
///
/// Not ported from loader.zig: `listEntries`'s exact unsorted OS-iteration
/// order (the zig oracle never sorts disk-discovered entries — only the
/// embedded set is build-time sorted). `list_disk_entries` below DOES sort
/// its result for determinism; this is a deliberate, harmless improvement
/// over the oracle (no scenario/cmd surface pins the zig ordering yet) —
/// see this module's .cpp file comment.
module;

export module planar.engine.config.templates;

import std;

namespace planar::engine::config {

/// @brief Where a resolved/listed template came from.
export enum class template_source : std::uint8_t {
  disk,     ///< Read from a file under the operator's `templates.dir`.
  embedded, ///< Read from the compiled-in `templates_embed` set.
};

/// @brief One resolved or listed template. `raw` is populated by
/// `load_template` and left empty by the `list_*` enumerators (mirrors
/// zig's `Template` vs `ListEntry` split, collapsed into one type here
/// since this port has no manual-allocation cost to avoid duplicating the
/// smaller shape).
export struct template_entry {
  std::string     set_name; ///< The template set this entry belongs to (or "default" for an embedded entry).
  std::string     system;   ///< External system slug (e.g. "github-issues", "jira").
  std::string     kind;     ///< Kind within the set (e.g. "issue", "epic").
  template_source source_;  ///< Where this entry came from — disk or the embedded set.
  std::string     path;     ///< Filesystem path (disk) or `"embedded:<system>/<kind>.json"` (embedded).
  std::string     raw;      ///< Verbatim JSON content — only populated by `load_template`.
};

/// @brief Error surface for `load_template`.
export enum class template_error : std::uint8_t {
  not_found, ///< No file matched at any of the three resolution levels.
};

/// @brief Resolve and read a template using the three-level fallback
/// chain (see this file's header comment). `root` is the operator's
/// templates directory; empty `root` skips straight to the embedded
/// defaults (steps 1-2 both require a non-empty root, mirroring zig).
/// `set_name == "default"` skips step 1 (avoids checking the same disk
/// path twice), mirroring zig's own guard.
/// @param set_name The template set to prefer.
/// @param system External system slug.
/// @param kind Kind within the set.
/// @param root The operator's templates directory (`~/.planar/templates`
/// by default, per `config::templates::dir`), or empty for "no override
/// root configured".
/// @return The resolved template, or `template_error::not_found` if no
/// level of the chain has a matching file.
export auto load_template(std::string_view set_name, std::string_view system, std::string_view kind, std::string_view root)
    -> std::expected<template_entry, template_error>;

/// @brief Enumerate every `<root>/<set>/<system>/<kind>.json` triple on
/// disk. A nonexistent or non-directory `root` (including an empty one)
/// yields an empty result rather than an error — mirrors zig's
/// `listEntries`. Sorted by `(set_name, system, kind)` for a deterministic
/// result (see this file's header comment on the deliberate improvement
/// over the zig oracle's raw OS-iteration order).
/// @param root The operator's templates directory.
/// @return Every disk-discovered template triple, sorted.
export auto list_disk_entries(std::string_view root) -> std::vector<template_entry>;

/// @brief Enumerate every embedded template. Order matches
/// `embedded_templates()`'s own build-time-sorted `(system, kind)` order
/// (mirrors zig's `listEmbeddedEntries`, which walks the codegen'd `embed.all`
/// array in its already-sorted order).
/// @return Every embedded template, in ascending `(system, kind)` order.
export auto list_embedded_entries() -> std::vector<template_entry>;

} // namespace planar::engine::config
