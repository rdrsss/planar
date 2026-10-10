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
# scripts/get_cpm.sh does the same thing for command-line/CI use. Pinned to
# the EXACT version + hash of the CPM.cmake actually committed at
# cmake/CPM.cmake (CURRENT_CPM_VERSION below) via its versioned GitHub
# release tag — never releases/latest, which is a moving target that could
# silently bootstrap a different CPM version than the one every other
# CPMAddPackage() call in this file was verified against (task 6049, F8).
# SHA256 re-verified independently against the versioned release asset.
set(_planar_cpm_version "0.43.1")
set(_planar_cpm_sha256 "1c40fc102ce9625d7de7eb14f541cab30cc3138dca627f0b0ec40293ce6c2934")
set(_planar_cpm_file "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CPM.cmake")
if(NOT EXISTS "${_planar_cpm_file}")
  message(STATUS "Downloading pinned CPM.cmake v${_planar_cpm_version} -> ${_planar_cpm_file}")
  file(DOWNLOAD
    "https://github.com/cpm-cmake/CPM.cmake/releases/download/v${_planar_cpm_version}/CPM.cmake"
    "${_planar_cpm_file}"
    STATUS _planar_cpm_status
    EXPECTED_HASH SHA256=${_planar_cpm_sha256}
    TLS_VERIFY ON)
  list(GET _planar_cpm_status 0 _planar_cpm_status_code)
  if(NOT _planar_cpm_status_code EQUAL 0)
    file(REMOVE "${_planar_cpm_file}")
    list(GET _planar_cpm_status 1 _planar_cpm_status_msg)
    message(FATAL_ERROR "Failed to download CPM.cmake v${_planar_cpm_version}: ${_planar_cpm_status_msg}")
  endif()
endif()
unset(_planar_cpm_file)
unset(_planar_cpm_version)
unset(_planar_cpm_sha256)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/CPM.cmake")

# Which TLS backend curl's vendored build resolves — see the libcurl block
# below for why this one dependency's TLS provider comes from the platform
# rather than from a second vendored archive.
if(APPLE)
  set(PLANAR_CURL_USE_SECTRANSP ON)
  set(PLANAR_CURL_USE_OPENSSL OFF)
else()
  set(PLANAR_CURL_USE_SECTRANSP OFF)
  set(PLANAR_CURL_USE_OPENSSL ON)
endif()

# --- Dependencies land here as the ports that need them arrive -------------
#
# Per the tech-spec's initial dependency set, in the order later tasks are
# expected to add them:
#
#   * Catch2   3.15.3 — tests (cmake/module.cmake's planar_module(); task 6023) [vendored]
#   * SQLite   3.53.3 (amalgamation) — storage [vendored]
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
#
# Pinned to 3.50.2. The pin ORIGINALLY matched zig/vendor/sqlite's own
# (zig/vendor/manifest.zon) exactly, because the parity premise required
# both trees to open the same on-disk format under test — an earlier cycle
# bumped this to 3.53.4 ahead of the Zig tree and broke that. SHA256 was
# re-verified independently against
# https://www.sqlite.org/2025/sqlite-amalgamation-3500200.zip (task 6049, F7).
#
# THAT CONSTRAINT IS NOW LIFTED: the M10 cutover (task 6045) deleted zig/,
# so there is no second tree to stay format-compatible with and sqlite is
# free to move independently.
#
# Bumped to 3.53.3 at task 6496 (plan 1033 M1). SHA256 verified
# independently by downloading the archive from sqlite.org and running
# `shasum -a 256`. Re-verify again before any bump.
CPMAddPackage(
  NAME sqlite
  VERSION 3.53.3
  URL https://sqlite.org/2026/sqlite-amalgamation-3530300.zip
  URL_HASH SHA256=646421e12aac110282ef8cc68f1a62d4bb15fc7b8f09da0b53e29ee690500431
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

# --- CLI11 (D21 — replaces the hand-rolled parser half of planar.cli) ---------
#
# Ships a real C++20 module (`export module cli11;`, src/modules/CLI11.cppm)
# wired through FILE_SET CXX_MODULES, which is why this is a viable swap at
# all. NOTE it is a header-WRAPPING module: the .cppm opens a global module
# fragment and `#include <CLI/CLI.hpp>` into it, so the whole header still
# lands in the TU. That interacts with `import std` and is the thing to watch
# if a build ever goes strange here.
#
# Scope of the swap: CLI11 owns TOKENIZATION and VALUE COERCION only. Planar
# keeps its own help renderer, schema-catalog emitter, exit-code mapping and
# completion generator, because those are pinned byte-for-byte against the
# Zig oracle and CLI11 does not emit them.
CPMAddPackage(
  NAME CLI11
  URL https://github.com/CLIUtils/CLI11/archive/refs/tags/v2.7.2.tar.gz
  URL_HASH SHA256=46eef3101da70852ec7af026e09d485ccee81813331c8c6052d39344443b83da
  SYSTEM YES
  EXCLUDE_FROM_ALL YES
  OPTIONS
    "CLI11_MODULES ON"
    "CLI11_PRECOMPILED ON"
    "CLI11_BUILD_TESTS OFF"
    "CLI11_BUILD_EXAMPLES OFF"
    "CLI11_BUILD_DOCS OFF"
    "CLI11_INSTALL OFF"
)

# --- libcurl (plan 996, task 6041 — M7's external plane) ----------------------
#
# The HTTP transport behind `planar.http`'s `curl_transport`, which the Jira
# and GitHub Issues adapters send every request through. Taken the same way
# every other application dependency here is taken — a pinned release archive
# by URL + SHA256 through CPM, never `find_package(CURL)`. A system libcurl
# IS present on this machine (8.7.1, SecureTransport/LibreSSL), and taking it
# would have been one line; it is deliberately NOT taken, because the whole
# point of the rule at the top of this file is that a configured build
# reproduces from committed sources rather than from whatever the host
# happens to ship. SHA256 computed by downloading the archive and running
# `shasum -a 256` on it.
#
# 8.7.1 is chosen to match the version already installed on the development
# machine, so a divergence between the vendored client and a hand-run `curl`
# reproduction is never a version difference.
#
# ## The TLS backend is platform-conditional, and that is not a hedge
#
# curl needs a TLS backend and CANNOT vendor one from here: every candidate
# (OpenSSL, mbedTLS, wolfSSL) is a second large C dependency with its own
# build, and the OS already ships a usable one. On Apple platforms
# SecureTransport is part of the SDK (Security.framework), so `CURL_USE_SECTRANSP`
# needs nothing fetched or found. Elsewhere curl's own build resolves OpenSSL.
# This is a TRANSITIVE dependency of a vendored package resolving a platform
# TLS provider — categorically different from taking an APPLICATION dependency
# by `find_package`, which is what the rule at the top of this file forbids.
#
# Everything curl can be built without is turned off: no `curl` executable, no
# shared library, no tests, no install rules, no libpsl/libssh2/zlib/brotli/
# zstd/libidn2/nghttp2. What is left is an HTTP/HTTPS client, which is all the
# adapter boundary asks for. The build is ~170 objects and finishes in a few
# seconds; configure costs ~30s once, then caches.
#
# The consumer target is `CURL::libcurl` (curl's own alias for whichever of
# the static/shared libraries its build selected — here always the static
# one, since BUILD_SHARED_LIBS is OFF).
#
# VERSION is stated explicitly (task 6496): CPM cannot parse one out of the
# `curl-8_7_1` tag and recorded this package as version "1", so any other
# CPMAddPackage(curl ...) request would compare against a meaningless number.
set(_planar_curl_portable_options "")
if(PLANAR_PORTABLE AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  # Portable Linux bundles carry OpenSSL rather than requiring the host's
  # libssl/libcrypto ABI. TLS fixes therefore require a new Planar release.
  # Trust comes from the runtime host, never a configure-host bundle path.
  # CPM scopes these options to curl's subdirectory, including its OpenSSL
  # discovery; ordinary builds and macOS SecureTransport keep their defaults.
  list(APPEND _planar_curl_portable_options
    "OPENSSL_USE_STATIC_LIBS ON"
    "CURL_CA_BUNDLE none"
    "CURL_CA_PATH /etc/ssl/certs"
    "CURL_CA_FALLBACK ON")
  # OpenSSL 3.5's pkg-config static dependencies include bare `zstd` link
  # items. FindOpenSSL resolves its own archives and zlib statically, but
  # leaves these items as -lzstd, which would select a shared library.
  # Declare this directory-scoped target before curl so its TLS targets
  # resolve those existing items to an archive. Hosts without zstd need no
  # target; the post-discovery check below refuses a missing required archive.
  find_library(_planar_openssl_zstd_archive NAMES libzstd.a NO_CACHE)
  if(_planar_openssl_zstd_archive)
    add_library(zstd STATIC IMPORTED)
    set_target_properties(zstd PROPERTIES
      IMPORTED_LOCATION "${_planar_openssl_zstd_archive}")
  endif()
  unset(_planar_openssl_zstd_archive)
endif()
CPMAddPackage(
  NAME curl
  VERSION 8.7.1
  URL https://github.com/curl/curl/archive/refs/tags/curl-8_7_1.tar.gz
  URL_HASH SHA256=0e46c856f517602c347bb5fe5b73174f8ee798bc87f1a97235c95761f75fcc28
  SYSTEM YES
  EXCLUDE_FROM_ALL YES
  OPTIONS
    "BUILD_CURL_EXE OFF"
    "BUILD_SHARED_LIBS OFF"
    "BUILD_STATIC_LIBS ON"
    "BUILD_TESTING OFF"
    "CURL_DISABLE_INSTALL ON"
    "CURL_USE_LIBPSL OFF"
    "CURL_USE_LIBSSH2 OFF"
    "CURL_ZLIB OFF"
    "CURL_BROTLI OFF"
    "CURL_ZSTD OFF"
    "USE_LIBIDN2 OFF"
    "USE_NGHTTP2 OFF"
    "CURL_ENABLE_SSL ON"
    "CURL_USE_OPENSSL ${PLANAR_CURL_USE_OPENSSL}"
    "CURL_USE_SECTRANSP ${PLANAR_CURL_USE_SECTRANSP}"
    ${_planar_curl_portable_options}
)
unset(_planar_curl_portable_options)

if(PLANAR_PORTABLE AND CMAKE_SYSTEM_NAME STREQUAL "Linux"
    AND "zstd" IN_LIST _OPENSSL_STATIC_LIBRARIES AND NOT TARGET zstd)
  message(FATAL_ERROR "Portable OpenSSL requires its static dependency libzstd.a")
endif()

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

# --- Lua 5.5.0 (plan 996, task 6042 — M8's planar-execute workflow host) ------
#
# The sandboxed script engine `planar-execute` runs deterministic workflows
# in. Taken as a pinned release archive by URL + SHA256 through CPM, like
# every other application dependency here — and, like SQLite, DOWNLOAD_ONLY,
# because Lua ships a hand-written Makefile and no CMake build of its own.
#
# ## The version was derived, not chosen
#
# Pinned to 5.5.0 to match `zig/vendor/manifest.zon`'s Lua entry EXACTLY
# (same URL, same SHA256 — re-verified independently here with
# `shasum -a 256` against the downloaded archive). That oracle was deleted at
# the M10 cutover (task 6045), but the pin STAYS: the version is now load-
# bearing on its own account, because the sandbox's observable surface —
# pinned by `planar-execute`'s own tests — is version-sensitive
# in ways that are easy to miss: the oracle's live `_VERSION` reads
# "Lua 5.5", its `math` table carries `acos`/`asin`/`atan`/`frexp`/`ldexp`
# (absent from a default 5.4 build) and its `table` carries `create` (new in
# 5.5). A different Lua would change what `surface.t.cpp`'s sandbox
# enumeration sees while every host function still "worked", which is exactly
# the class of drift that milestone's frozen-manifest test exists to catch.
#
# ## The source list mirrors the Zig build's, and excludes two files
#
# `lua.c` (the standalone interpreter's main) and `luac.c` (the compiler's
# main) are NOT compiled in: each defines its own `main`, and linking either
# into a static library that a Planar binary consumes would collide with that
# binary's entry point. The remaining set is CORE_O + LIB_O from Lua's own
# Makefile. `linit.c` IS included — it defines `luaL_openlibs`, which
# planar-execute deliberately never calls (it opens a curated subset itself;
# see engine/execute/execute.cppm § sandbox), but the object is harmless and
# excluding it would be a divergence from the oracle's link for no gain.
#
# LUA_USE_MACOSX / LUA_USE_LINUX / LUA_USE_POSIX mirror `zig/build.zig`'s
# platform switch verbatim. Note what those defines turn ON: `dlopen`-backed
# dynamic loading for `package.loadlib`. That is NOT a hole here, because the
# `package` library is never opened — the oracle's live global table has no
# `package`, no `require`, no `loadlib`, and `surface.t.cpp` asserts that
# against a real state rather than trusting this comment.
CPMAddPackage(
  NAME lua
  VERSION 5.5.0
  URL https://www.lua.org/ftp/lua-5.5.0.tar.gz
  URL_HASH SHA256=57ccc32bbbd005cab75bcc52444052535af691789dba2b9016d5c50640d68b3d
  DOWNLOAD_ONLY YES
  EXCLUDE_FROM_ALL YES
  SYSTEM YES
)
if(lua_ADDED)
  set(PLANAR_LUA_SOURCES "")
  foreach(_lua_unit IN ITEMS
      lapi lauxlib lbaselib lcode lcorolib lctype ldblib ldebug ldo ldump
      lfunc lgc linit liolib llex lmathlib lmem loadlib lobject lopcodes
      loslib lparser lstate lstring lstrlib ltable ltablib ltm lundump
      lutf8lib lvm lzio)
    list(APPEND PLANAR_LUA_SOURCES "${lua_SOURCE_DIR}/src/${_lua_unit}.c")
  endforeach()
  add_library(lua_static STATIC ${PLANAR_LUA_SOURCES})
  add_library(lua::lua ALIAS lua_static)
  target_include_directories(lua_static SYSTEM PUBLIC "${lua_SOURCE_DIR}/src")
  if(APPLE)
    target_compile_definitions(lua_static PRIVATE LUA_USE_MACOSX)
  elseif(UNIX)
    target_compile_definitions(lua_static PRIVATE LUA_USE_LINUX)
    target_link_libraries(lua_static PUBLIC ${CMAKE_DL_LIBS} m)
  else()
    target_compile_definitions(lua_static PRIVATE LUA_USE_POSIX)
  endif()
endif()

# --- tree-sitter Zig parser (task 6189 — derived closure extraction) ------
# Both sources are pinned release tag archives.  The grammar is built as C
# rather than through its optional Node tooling; the closure engine consumes
# only the stable C parser API.
CPMAddPackage(
  NAME tree_sitter
  URL https://codeload.github.com/tree-sitter/tree-sitter/tar.gz/refs/tags/v0.25.10
  URL_HASH SHA256=ad5040537537012b16ef6e1210a572b927c7cdc2b99d1ee88d44a7dcdc3ff44c
  DOWNLOAD_ONLY YES EXCLUDE_FROM_ALL YES SYSTEM YES)
CPMAddPackage(
  NAME tree_sitter_zig
  URL https://codeload.github.com/tree-sitter-grammars/tree-sitter-zig/tar.gz/refs/tags/v1.1.2
  URL_HASH SHA256=612d67059faa90ec7691e5d786d70d8f7c2c8b15b83de901b9b801122ad4cf25
  DOWNLOAD_ONLY YES EXCLUDE_FROM_ALL YES SYSTEM YES)
if(tree_sitter_ADDED)
  add_library(tree_sitter_static STATIC "${tree_sitter_SOURCE_DIR}/lib/src/lib.c")
  # `lib/include` is the PUBLIC surface; `lib/src` has to be on the search
  # path too, PRIVATEly, because tree-sitter's own `lib/src/unicode/utf8.h`
  # includes `"unicode/umachine.h"` -- a path that only resolves with
  # `lib/src` as a base. A quoted include is searched relative to the
  # INCLUDING file (`lib/src/unicode/`), so without this the header looks
  # for `lib/src/unicode/unicode/umachine.h`, which does not exist. macOS
  # tolerated the omission; Linux does not, and failed with
  # "'unicode/umachine.h' file not found" (measured building the Linux
  # gate, task 6936). tree-sitter's own build adds this directory.
  target_include_directories(tree_sitter_static SYSTEM PUBLIC "${tree_sitter_SOURCE_DIR}/lib/include")
  target_include_directories(tree_sitter_static SYSTEM PRIVATE "${tree_sitter_SOURCE_DIR}/lib/src")
  add_library(tree_sitter::tree_sitter ALIAS tree_sitter_static)
endif()
if(tree_sitter_zig_ADDED)
  add_library(tree_sitter_zig_static STATIC "${tree_sitter_zig_SOURCE_DIR}/src/parser.c")
  target_include_directories(tree_sitter_zig_static SYSTEM PUBLIC "${tree_sitter_zig_SOURCE_DIR}/src")
  target_link_libraries(tree_sitter_zig_static PUBLIC tree_sitter::tree_sitter)
  add_library(tree_sitter::zig ALIAS tree_sitter_zig_static)
endif()
