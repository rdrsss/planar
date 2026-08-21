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
# Per the tech-spec's initial dependency set, in the order later tasks are
# expected to add them:
#
#   * Catch2   3.15.3 — tests (cmake/module.cmake's planar_module(); task 6023) [vendored]
#   * SQLite   3.53.4 (amalgamation) — storage [vendored]
#   * Lua      5.5    — planar-execute sandbox
#   * Glaze    8.1.0  — JSON / YAML front matter / TOML config (D8, D12) [vendored]
#   * libcurl          — HTTP for Jira/GitHub adapters
#   * spdlog   1.17.0 — logging (D11) [vendored]
#
# Adding a not-yet-vendored dependency from the list above means one
# CPMAddPackage() block, one configure to populate vendor/, then committing
# the new tree — that happens in the commit that first needs the library,
# not ahead of it.
#
# Every archive below is pinned by `URL` + `URL_HASH SHA256=...`, computed
# by downloading the archive and running `shasum -a 256` on it (tech-spec §
# Toolchain and conventions: release archives only, no GIT_REPOSITORY /
# submodules / FetchContent / find_package for application deps). SYSTEM YES
# + EXCLUDE_FROM_ALL YES on every package: third-party headers are exempt
# from -Werror, and their install rules/optional targets stay out of the
# default build.

# --- Catch2 v3 (task 6023) --------------------------------------------------
#
# cmake/module.cmake's planar_module() looks for the Catch2::Catch2WithMain
# target and, once found, appends PLANAR_CATCH2_SOURCE_DIR/extras to
# CMAKE_MODULE_PATH so `include(Catch)` (catch_discover_tests) resolves.
CPMAddPackage(
  NAME Catch2
  URL https://github.com/catchorg/Catch2/archive/refs/tags/v3.15.3.tar.gz
  URL_HASH SHA256=b0299ae552918220a7a6e21e7de5b714777f4e8c883fb70c4bb23fe01df8c6e3
  SYSTEM YES
  EXCLUDE_FROM_ALL YES
)
if(Catch2_ADDED)
  set(PLANAR_CATCH2_SOURCE_DIR "${Catch2_SOURCE_DIR}" CACHE INTERNAL
    "Catch2 source tree, for extras/Catch.cmake (cmake/module.cmake)")
endif()

# --- SQLite (amalgamation) --------------------------------------------------
#
# No CMake build of its own (it is a single amalgamated .c/.h pair) — vendor
# the archive DOWNLOAD_ONLY and compile it ourselves as a small static
# library with the same flags the Zig tree used
# (docs/architecture.md / tech-spec § Dependency set): SQLITE_THREADSAFE=1,
# SQLITE_ENABLE_FTS5, SQLITE_ENABLE_JSON1, SQLITE_DQS=0,
# SQLITE_DEFAULT_FOREIGN_KEYS=1, SQLITE_USE_URI=1.
CPMAddPackage(
  NAME sqlite
  VERSION 3.53.4
  URL https://www.sqlite.org/2026/sqlite-amalgamation-3530400.zip
  URL_HASH SHA256=1e71ddf93849c6a6ecf58b827c0692073d2dd7ee40196158068f7b29f422e87d
  DOWNLOAD_ONLY YES
  EXCLUDE_FROM_ALL YES
  SYSTEM YES
)
if(sqlite_ADDED)
  add_library(sqlite3 STATIC "${sqlite_SOURCE_DIR}/sqlite3.c")
  add_library(SQLite::SQLite3 ALIAS sqlite3)
  target_include_directories(sqlite3 SYSTEM PUBLIC "${sqlite_SOURCE_DIR}")
  target_compile_definitions(sqlite3 PRIVATE
    SQLITE_THREADSAFE=1
    SQLITE_ENABLE_FTS5
    SQLITE_ENABLE_JSON1
    SQLITE_DQS=0
    SQLITE_DEFAULT_FOREIGN_KEYS=1
    SQLITE_USE_URI=1)
endif()

# --- Glaze (D8, D12) ---------------------------------------------------------
#
# Header-only reflection-based JSON / YAML / TOML library; the one
# serialization layer for `--json` I/O, workbench front matter, and
# ~/.planar/config.toml (tech-spec § Dependency set).
CPMAddPackage(
  NAME glaze
  URL https://github.com/stephenberry/glaze/archive/refs/tags/v8.1.0.tar.gz
  URL_HASH SHA256=8cc479b53e4612fad2b4b74a079421d5ea897944e54a9ab5d1207cd68cf52b9b
  SYSTEM YES
  EXCLUDE_FROM_ALL YES
)

# --- spdlog (D11) -------------------------------------------------------------
#
# Replaces std.log's scoped logging; keep text/JSON dual mode per the
# tech-spec. Its own bundled tests/examples/bench are disabled — Planar
# supplies neither.
CPMAddPackage(
  NAME spdlog
  VERSION 1.17.0
  URL https://github.com/gabime/spdlog/archive/refs/tags/v1.17.0.tar.gz
  URL_HASH SHA256=d8862955c6d74e5846b3f580b1605d2428b11d97a410d86e2fb13e857cd3a744
  SYSTEM YES
  EXCLUDE_FROM_ALL YES
  OPTIONS
    "SPDLOG_BUILD_EXAMPLE OFF"
    "SPDLOG_BUILD_TESTS OFF"
    "SPDLOG_INSTALL OFF"
)
