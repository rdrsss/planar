/// @file version.cpp
/// @brief Implementation of `planar.cli.version` (see version.cppm).
///
/// Oracle capture this module's text-rendering shape was checked against
/// (task brief: derive expected values by running the reference binary,
/// never hand-assumed):
///
///   $ ./zig/zig-out/bin/planar version
///   planar dev dev zig 0.16.0
///
/// A dev build (the default — `-Dversion-meta` not passed) embeds the
/// sentinel `"dev"` for both the sha and date fields and never resolves a
/// live git sha, matching this port's `PLANAR_VERSION_META` default-off
/// contract below.
module;

module planar.cli.version;

import std;

namespace planar::cli {

auto current_build_info() -> build_info {
  build_info info{};
#if defined(PLANAR_GIT_SHA)
  info.sha = PLANAR_GIT_SHA;
#endif
#if defined(PLANAR_BUILD_DATE)
  info.date = PLANAR_BUILD_DATE;
#endif
#if defined(PLANAR_GIT_DIRTY)
  info.dirty = (PLANAR_GIT_DIRTY) != 0;
#endif
  return info;
}

auto compiler_version_string() -> std::string {
#if defined(__clang__)
  return "Clang " + std::to_string(__clang_major__) + "." + std::to_string(__clang_minor__) + "." +
         std::to_string(__clang_patchlevel__);
#elif defined(__GNUC__)
  return "GCC " + std::to_string(__GNUC__) + "." + std::to_string(__GNUC_MINOR__) + "." + std::to_string(__GNUC_PATCHLEVEL__);
#else
  return "unknown";
#endif
}

} // namespace planar::cli
