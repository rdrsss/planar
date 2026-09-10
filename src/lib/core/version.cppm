/// @file version.cppm
/// @brief planar.core — M0 scaffold seed module (plan 996, task
/// cpp-scaffold-presets). Its only job is to prove the CMake module-build
/// machinery (cmake/module.cmake's planar_module(), cmake/architecture.cmake's
/// D15 gate) configures and builds cleanly under the pinned toolchain
/// (docs/toolchain-parity.md). Grows into the real `core` bucket, or is
/// absorbed into a later-arriving module, as the engine buckets in the
/// tech-spec's § File-level tree get ported.

export module planar.core;

/// @brief Returns the Planar C++26 rewrite's current version string.
/// @return A statically-allocated, null-terminated version string.
export constexpr auto version() noexcept -> const char* {
  return "0.1.0";
}
