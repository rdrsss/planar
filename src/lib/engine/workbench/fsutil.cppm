/// @file fsutil.cppm
/// @brief `planar.engine.workbench.fsutil` — the filesystem primitives the
/// workbench bucket shares (plan 996, task 6037).
///
/// Behavior-preserving port (D2) of the POSIX helpers zig/src/engine/
/// workbench/{sync,gc,manifest}.zig each `@cImport`ed separately
/// (`writeFileAtomic`, `readFileAlloc`, `makePathAll`, `pathExists`,
/// `collectMarkdownRecursive`, `deleteTreePortable`).
///
/// The Zig originals reach for raw `open`/`write`/`rename`/`opendir` because
/// its `std.Io` surface was mid-migration. `<filesystem>` and `<fstream>`
/// give the same observable behavior here, so this port uses them —
/// except for the one place where the C call is the CONTRACT: the write is
/// tmp-then-`rename`, so a reader never observes a half-written workbench
/// file, and `std::filesystem::rename` is specified to be that same atomic
/// replace.
///
/// A separate module rather than free functions duplicated three times: all
/// three consumers compile into the ONE `planar_engine_workbench` target, so
/// this creates no target edge and nothing for `cmake/architecture.cmake` to
/// police (same arrangement `engine_config` documents for its five modules).
module;

export module planar.engine.workbench.fsutil;

import std;

namespace planar::engine::workbench::fsutil {

/// @brief Read a whole file.
/// @param path The file to read.
/// @return The bytes, or unset when the file is absent or unreadable.
export auto read_file(const std::filesystem::path& path) -> std::optional<std::string>;

/// @brief Write `content` to `path` atomically: to `<path>.tmp` first, then
/// `rename` over the target. Parent directories are created as needed.
/// @param path The destination file.
/// @param content The bytes to write.
/// @return `true` on success.
export auto write_file_atomic(const std::filesystem::path& path, std::string_view content) -> bool;

/// @brief Create `path` and every missing parent.
/// @param path The directory to create.
/// @return `true` when the directory exists afterwards.
export auto make_path_all(const std::filesystem::path& path) -> bool;

/// @brief Whether anything exists at `path`.
/// @param path The path to test.
/// @return `true` when it exists.
export auto path_exists(const std::filesystem::path& path) -> bool;

/// @brief Every `.md` file under `dir`, recursively, as absolute paths.
///
/// An absent directory is an EMPTY list, never an error. The order is
/// unspecified here; callers that need determinism sort (as `lint` does).
/// @param dir The directory to walk.
/// @return The Markdown files found.
export auto collect_markdown(const std::filesystem::path& dir) -> std::vector<std::string>;

/// @brief Delete a directory tree.
///
/// REFUSES anything that is not a directory — including a SYMLINK to one.
/// The Zig original `lstat`s and checks `S_IFDIR` before recursing for
/// exactly this reason: `workbench archive` calls it on a path built from
/// database-supplied slugs, and following a symlink out of the workbench
/// root would delete an unrelated tree.
/// @param dir The directory to remove.
/// @return `true` when the tree was removed, `false` when the path is
/// absent or is not a real directory.
export auto delete_tree(const std::filesystem::path& dir) -> bool;

/// @brief Remove one file.
/// @param path The file to unlink.
/// @return `true` when the file no longer exists.
export auto remove_file(const std::filesystem::path& path) -> bool;

} // namespace planar::engine::workbench::fsutil
