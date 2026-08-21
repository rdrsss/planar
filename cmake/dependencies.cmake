# Third-party dependencies (plan 996 tech-spec § Dependency set).
#
# Every `CPMAddPackage(...)` this project ever adds lives here — the single
# home the tech-spec names — pinned by `URL` + `URL_HASH SHA256=...` release
# archives, never `GIT_REPOSITORY`/submodules/`FetchContent`/`find_package`
# for application deps (tech-spec § Toolchain and conventions; supersedes the
# Zig-era `vendor/manifest.zon` + `zig build vendor-sync` mechanism, same
# philosophy, CPM as the tool). Sources are cached under `vendor/` via
# CPM_SOURCE_CACHE and committed, so a configured build never touches the
# network again.

set(CPM_SOURCE_CACHE "${CMAKE_CURRENT_SOURCE_DIR}/vendor" CACHE PATH
  "Where CPM caches and this repository commits vendored dependency sources")

# Bootstrap cmake/CPM.cmake on first configure if it is not checked in yet —
# scripts/get_cpm.sh does the same thing for command-line/CI use.
set(_planar_cpm_file "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CPM.cmake")
if(NOT EXISTS "${_planar_cpm_file}")
  message(STATUS "Downloading latest CPM.cmake -> ${_planar_cpm_file}")
  file(DOWNLOAD
    "https://github.com/cpm-cmake/CPM.cmake/releases/latest/download/CPM.cmake"
    "${_planar_cpm_file}"
    STATUS _planar_cpm_status
    TLS_VERIFY ON)
  list(GET _planar_cpm_status 0 _planar_cpm_status_code)
  if(NOT _planar_cpm_status_code EQUAL 0)
    file(REMOVE "${_planar_cpm_file}")
    list(GET _planar_cpm_status 1 _planar_cpm_status_msg)
    message(FATAL_ERROR "Failed to download CPM.cmake: ${_planar_cpm_status_msg}")
  endif()
endif()
unset(_planar_cpm_file)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/CPM.cmake")

# --- Dependencies land here as the ports that need them arrive -------------
#
# Nothing is vendored yet at this M0 scaffold task (cpp-scaffold-presets):
# adding a dependency means one CPMAddPackage() block, one configure to
# populate vendor/, then committing the new tree — that happens in the
# commit that first needs the library, not ahead of it. Per the tech-spec's
# initial dependency set, in the order later tasks are expected to add them:
#
#   * Catch2   3.7.1  — tests (cmake/module.cmake's planar_module(); task 6023)
#   * SQLite   (amalgamation) — storage
#   * Lua      5.5    — planar-execute sandbox
#   * Glaze            — JSON / YAML front matter / TOML config (D8, D12)
#   * libcurl          — HTTP for Jira/GitHub adapters
#   * spdlog           — logging (D11)
#
# When Catch2 lands, this file also sets PLANAR_CATCH2_SOURCE_DIR (mirroring
# TABULA_CATCH2_SOURCE_DIR in cmake/module.cmake's reference pattern) so
# planar_module() can find extras/Catch.cmake for catch_discover_tests().
