/// @file version.cppm
/// @brief `planar.cli.version` — build-metadata resolution and the
/// `planar version` text rendering (task cpp-cli-output-logging).
///
/// Behavior-preserving in *contract*, not in literal wording, of
/// zig/src/cmd/planar/handlers/version.zig: same opt-in build-metadata
/// contract (a dev build embeds the sentinel `"dev"` for sha and date and
/// never bakes in a live git sha/dirty flag — CLAUDE.md § Build And Test
/// documents this as a deliberate Zig-side build-cache fix, and regressing
/// it here would be a real defect, not a cosmetic gap), same sha-truncation
/// and dirty-marker shape, same whitespace-splittable five-token line
/// layout. The runtime tag differs by construction: Zig's line ends
/// `zig <zig-version>`; this port has no Zig runtime to report, so it ends
/// `cxx <compiler-version>` instead — the field *position* and *count*
/// match (a script splitting on whitespace still finds five tokens), only
/// the literal word for "which toolchain built this" differs. See
/// version.cpp's header comment for the oracle capture this was checked
/// against (`./zig/zig-out/bin/planar version` → `planar dev dev zig
/// 0.16.0`).
module;

export module planar.cli.version;

import std;

namespace planar::cli {

/// @brief Resolved build-time metadata for one binary. Mirrors the three
/// `build_options` fields zig/build.zig exposes to
/// `handlers/version.zig` (`git_sha`, `build_date`, `git_dirty`).
export struct build_info {
  /// @brief Git commit sha (up to 40 hex chars), or the sentinel `"dev"`
  /// / `"unknown"` when metadata resolution is disabled or failed.
  std::string sha = "dev";
  /// @brief ISO8601 build date, or the sentinel `"dev"` / `"unknown"`.
  std::string date = "dev";
  /// @brief Whether the working tree had uncommitted changes at build
  /// time. Always `false` when metadata resolution is disabled — a dev
  /// build never claims dirty state it did not actually resolve.
  bool dirty = false;
};

/// @brief Truncate a 40-char git sha to 12 hex characters, matching Go's
/// `buildVersion()` helper and zig's `shortenSha`. Short shas, sentinel
/// values like `"unknown"`/`"dev"`, and anything else under 12 chars pass
/// through unchanged.
/// @param sha The sha (or sentinel) to shorten.
/// @return The first 12 characters of `sha`, or `sha` itself if shorter.
export auto shorten_sha(std::string_view sha) -> std::string_view {
  if (sha.size() <= 12) {
    return sha;
  }
  return sha.substr(0, 12);
}

/// @brief Return this process's compiled-in build metadata. Reads the
/// configure-time macros `PLANAR_GIT_SHA` / `PLANAR_BUILD_DATE` /
/// `PLANAR_GIT_DIRTY` (set by src/lib/cli/CMakeLists.txt only when the
/// `PLANAR_VERSION_META` CMake option is explicitly enabled — default
/// off, mirroring zig's `-Dversion-meta` opt-in). When the option is off
/// (the default dev-build configuration), every field is the `"dev"`
/// sentinel and `dirty` is `false` — the build cache never thrashes on a
/// commit or a clean<->dirty flip because these macros are not
/// recompiled unless the option itself changes.
/// @return The resolved build_info for this compiled binary.
export auto current_build_info() -> build_info;

/// @brief Render `info` as the one-line `planar version` text form:
/// `"planar <sha><dirty-marker> <date> cxx <compiler-version>\n"`.
/// `sha` is truncated via `shorten_sha`; `dirty-marker` is `"+dirty"` when
/// `info.dirty` is true, empty otherwise. `compiler_version` is a
/// caller-supplied string (`compiler_version_string()` below) so this
/// function stays pure/testable without depending on the actual
/// compiling toolchain's preprocessor state.
/// @param info The build metadata to render.
/// @param compiler_version The compiler/runtime version string to print
/// after the literal `"cxx "` tag.
/// @return The rendered line, including the trailing newline.
export auto render_version_text(build_info const& info, std::string_view compiler_version) -> std::string {
  std::string out = "planar ";
  out += shorten_sha(info.sha);
  if (info.dirty) {
    out += "+dirty";
  }
  out += " ";
  out += info.date;
  out += " cxx ";
  out += compiler_version;
  out += "\n";
  return out;
}

/// @brief The compiler identifier + version string this translation unit
/// was itself compiled with (e.g. `"Clang 22.1.8"`). Resolved via
/// preprocessor macros in version.cpp; exported as a function (not a
/// constant) so it is trivially fakeable in tests without needing a
/// second build.
/// @return The compiler name and version, space-separated.
export auto compiler_version_string() -> std::string;

} // namespace planar::cli
