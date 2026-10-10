/// @file version.cppm
/// @brief `planar.cliapp.version` — build-metadata resolution and the
/// `planar version` text rendering (task cpp-cli-output-logging).
///
/// The text line has six whitespace-separated positions: 0 program,
/// 1 shortened sha with an optional +dirty suffix, 2 build date, 3 cxx,
/// 4 compiler identifier/version joined by a hyphen, and 5 release tag.
/// An unset release is dev. The first five positions are stable; installers
/// continue to read the sha at position 1 as their build_id.
module;

export module planar.cliapp.version;

import std;
import planar.json_text;

namespace planar::cliapp {

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
  /// @brief Release tag, or dev when PLANAR_RELEASE_VERSION is unset.
  std::string release = "dev";
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
/// `PLANAR_GIT_DIRTY` (set by src/lib/cliapp/CMakeLists.txt only when the
/// `PLANAR_VERSION_META` CMake option is explicitly enabled — default
/// off, mirroring zig's `-Dversion-meta` opt-in). When the option is off
/// (the default dev-build configuration), sha and date use the `"dev"`
/// sentinel and `dirty` is `false` — the build cache never thrashes on a
/// commit or a clean<->dirty flip because these macros are not
/// recompiled unless the option itself changes.
/// @return The resolved build_info for this compiled binary.
export auto current_build_info() -> build_info;

/// @brief Render `info` as the one-line version text form for a NAMED
/// binary: `"<program> <sha><dirty-marker> <date> cxx <compiler-version> <release>\n"`.
///
/// The leading program name is a parameter because it is operator-visible
/// and per-binary, exactly as it is on the Zig side: each of
/// zig/src/cmd/planar/handlers/version.zig,
/// zig/src/cmd/planar-agent/handlers/version.zig and
/// zig/src/cmd/planar-watch/handlers/version.zig hardcodes its OWN
/// literal (`"planar "`, `"planar-agent "`, `"planar-watch "`), and that
/// binary's own version.zig header says the prefix is the stable thing a
/// shell grep keys on to tell the binaries apart. A single hardcoded
/// `"planar "` here would make all three binaries claim to be the
/// operator binary (plan 996, task 6107).
///
/// `sha` is truncated via `shorten_sha`; `dirty-marker` is `"+dirty"` when
/// `info.dirty` is true, empty otherwise. `compiler_version` is a
/// caller-supplied string (`compiler_version_string()` below) so this
/// function stays pure/testable without depending on the actual
/// compiling toolchain's preprocessor state.
/// @param program The binary's own name, e.g. `"planar-watch"`.
/// @param info The build metadata to render.
/// @param compiler_version The compiler/runtime version string to print
/// after the literal `"cxx "` tag.
/// @return The rendered line, including the trailing newline.
export auto render_version_text(std::string_view program, build_info const& info, std::string_view compiler_version)
    -> std::string {
  std::string out(program);
  out += " ";
  out += shorten_sha(info.sha);
  if (info.dirty) {
    out += "+dirty";
  }
  out += " ";
  out += info.date;
  out += " cxx ";
  out += compiler_version;
  out += " ";
  out += info.release;
  out += "\n";
  return out;
}

/// @brief Render `info` as the one-line `planar version` text form:
/// `"planar <sha><dirty-marker> <date> cxx <compiler-version> <release>\n"`.
///
/// The operator binary's spelling of the overload above; kept as its own
/// name so the `planar` handler and this module's existing tests read
/// unchanged.
/// @param info The build metadata to render.
/// @param compiler_version The compiler/runtime version string to print
/// after the literal `"cxx "` tag.
/// @return The rendered line, including the trailing newline.
export auto render_version_text(build_info const& info, std::string_view compiler_version) -> std::string {
  return render_version_text("planar", info, compiler_version);
}

/// @brief Render build metadata as newline-terminated machine-readable JSON.
/// @param info The resolved build metadata; sha retains its full value.
/// @param compiler_version The compiler identifier and version.
/// @return An object with release, sha, date, dirty and compiler fields.
export auto render_version_json(build_info const& info, std::string_view compiler_version) -> std::string {
  return std::format("{{\"release\":{},\"sha\":{},\"date\":{},\"dirty\":{},\"compiler\":{}}}\n",
                     json_text::json_string(info.release), json_text::json_string(info.sha), json_text::json_string(info.date),
                     info.dirty, json_text::json_string(compiler_version));
}

/// @brief The compiler identifier + version string this translation unit
/// was itself compiled with (e.g. `"Clang-23.1.0"`). Resolved via
/// preprocessor macros in version.cpp; exported as a function (not a
/// constant) so it is trivially fakeable in tests without needing a
/// second build.
/// @return The compiler name and version, hyphen-separated.
export auto compiler_version_string() -> std::string;

} // namespace planar::cliapp
