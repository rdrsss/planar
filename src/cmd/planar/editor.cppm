/// @file editor.cppm
/// @brief `planar.cmd.planar.editor` — the `$EDITOR` / `$PAGER` subprocess
/// primitive shared by the `edit` and `view` arms of the drafting quartet
/// (plan 996, task 6205).
///
/// Ported from `zig/src/cmd/planar/editor.zig` (342 lines). It implements
/// ONLY the primitive:
///
///   1. Resolve the editor command via `PLANAR_EDITOR` -> `VISUAL` ->
///      `EDITOR` -> `vi`.
///   2. Create a temp file holding the initial content, with a stable
///      extension.
///   3. Exec the editor, INHERITING stdin/stdout/stderr so it owns the TTY.
///   4. Read the resulting file back.
///   5. Return content + temp path + the child's exit code.
///
/// What is deliberately NOT here, matching the Zig original: the dry-run
/// gate, content-hash change detection, front-matter mutation, post-pull DB
/// writes, and conflict detection. Those are `editflow`'s concerns.
///
/// ## THE ENVIRONMENT COMES FROM `context::env()`, NOT `std::getenv`
///
/// The Zig original reads `std.c.environ` directly, in both
/// `editor.zig::getPosixEnv` and `editflow.zig::execPager`. This port routes
/// BOTH through the injected `env_lookup` instead, for one reason that is
/// worth stating plainly because it is the whole reason the spawn is
/// testable at all:
///
///   `src/cmd/planar/CMakeLists.txt` records that `context.cpp`'s
///   `process_env()` is the ONLY function in this target permitted to call
///   `std::getenv`, so that a test can run real handlers against a scratch
///   root with no process-environment mutation. A second `getenv` caller
///   here would put editor resolution outside that seam and leave the
///   `$EDITOR` spawn reachable only by mutating the test process's own
///   environment.
///
/// This is observationally IDENTICAL in the shipped binary: the real
/// `context` is constructed with `process_env()`, which reads the same
/// process environment `std.c.environ` exposes. It differs only under test,
/// which is exactly the point.
///
/// ## Why the PATH search is done here rather than left to `execvp`
///
/// `editflow`'s pager chain is a FALLBACK chain — `$PAGER`, then `less`,
/// then `cat` — and a fallback chain needs to distinguish "could not exec
/// this command" from "the command ran and exited non-zero". After `fork`,
/// a failed `execvp` can only report itself by `_exit`ing with some code,
/// and any code it picks is a code a real pager could also have returned.
/// Zig does not have this problem: `std.process.spawn` resolves the
/// executable BEFORE forking and returns `error.FileNotFound` without ever
/// creating a child.
///
/// `resolve_program` reproduces that ordering, so `spawn_inherit` reports
/// `std::nullopt` for an unresolvable command and a real status otherwise.
/// Conflating the two would make a `$PAGER` that legitimately exits 127
/// silently fall through to `less`.
module;

#include <sys/wait.h>
#include <unistd.h>

export module planar.cmd.planar.editor;

import std;
import planar.cmd.planar.context;

namespace planar::cmd {

/// @brief Failure surface for this module.
export enum class editor_error : std::uint8_t {
  no_editor,       ///< No editor could be resolved and no override was given.
  tempfile_failed, ///< Creating, writing or spawning against the temp file failed.
  read_failed,     ///< Reading the temp file back after the editor exited failed.
  file_too_large,  ///< The file the editor left behind exceeds the 64 MiB cap.
};

/// @brief The largest file accepted back from the editor: 64 MiB.
///
/// Matches the Zig original's cap, which exists so a malicious or buggy
/// editor cannot drive an unbounded allocation.
export inline constexpr std::size_t k_max_edit_file_bytes = 64UL * 1024UL * 1024UL;

/// @brief Optional overrides for `invoke`.
export struct invoke_opts {
  /// @brief When set, this command is used instead of the env-var chain.
  /// Tests inject a stub here; `--editor`-style flags would too.
  std::optional<std::string> editor_override;
  /// @brief Extension for the temp file, controlling editor syntax
  /// highlighting. Defaults to the Zig original's `.md`.
  std::string file_extension = ".md";
};

/// @brief What a completed `invoke` produced.
export struct invoke_result {
  std::string content;      ///< The file's bytes after the editor exited.
  std::string path;         ///< The temp file's absolute path.
  int         exit_code{0}; ///< The editor's exit status; 0 means a normal save.
};

/// @brief Resolve the editor command.
///
/// Precedence, mirroring the Zig original exactly: `override`, then
/// `PLANAR_EDITOR`, `VISUAL`, `EDITOR`, then the literal `vi`. An
/// environment variable set to the EMPTY string is treated as unset, which
/// is the Zig original's `getPosixEnv` semantics and not the more usual
/// "set-but-empty wins".
/// @param env The environment lookup.
/// @param override The explicit override, if any.
/// @return The command to exec. Never empty.
export auto resolve_editor(const env_lookup& env, const std::optional<std::string>& override) -> std::string;

/// @brief Resolve a command name to an executable path.
///
/// A name containing `/` is used as-is; anything else is searched along
/// `PATH`. See this module's header for why this happens before the fork
/// rather than being left to `execvp`.
/// @param env The environment lookup, for `PATH`.
/// @param program The command name.
/// @return The executable path, or unset when nothing executable matches.
export auto resolve_program(const env_lookup& env, std::string_view program) -> std::optional<std::string>;

/// @brief Run `argv` to completion with stdin, stdout and stderr INHERITED.
///
/// Inheriting rather than piping is what lets a full-screen editor own the
/// terminal, and is what the Zig original's `.stdin = .inherit` triple does.
/// @param env The environment lookup, for resolving `argv[0]`.
/// @param argv The full argument vector; `argv[0]` is the program.
/// @return The child's exit status, or unset when `argv[0]` could not be
/// resolved or the fork failed — the two cases a fallback chain must treat
/// as "try the next candidate".
export auto spawn_inherit(const env_lookup& env, std::span<const std::string> argv) -> std::optional<int>;

/// @brief Write `initial_content` to a fresh temp file, open it in the
/// resolved editor, wait, and read the result back.
///
/// A non-zero editor exit is NOT an error here: it is reported in
/// `invoke_result::exit_code` and the file is read back regardless, leaving
/// the cancel/abort decision to the caller. That matches the Zig original
/// and the git convention it follows.
///
/// The editor value is treated as a single `argv[0]`, never shell-parsed,
/// so an editor needing flags (`code --wait`) cannot be configured through
/// these variables — also the Zig original's documented behaviour.
/// @param env The environment lookup.
/// @param initial_content The bytes to seed the temp file with.
/// @param opts The overrides.
/// @return The result, or the failure.
export auto invoke(const env_lookup& env, std::string_view initial_content, const invoke_opts& opts)
    -> std::expected<invoke_result, editor_error>;

} // namespace planar::cmd
