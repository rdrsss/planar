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
# Plus one dependency the tech-spec's initial set did not anticipate:
#
#   * xxHash   0.8.3   — `.manifest-docs` merkle digest for `workspace
#                         regenerate` (plan 996, task 6364) [vendored]
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
# Bumped to 3.53.3 at task 6496 (plan 1033 M1) to match Centurion's pin, so
# that the one `sqlite3` target both trees link is the version both declare
# (cmake/centurion.cmake § One pin per shared package). SHA256 re-verified
# independently by downloading the archive from sqlite.org and running
# `shasum -a 256`; it equals Centurion's. Re-verify again before any bump.
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
# `curl-8_7_1` tag and recorded this package as version "1", which made
# Centurion's own `curl 8.21.0` request compare against a meaningless number.
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

# --- xxHash 0.8.3 (plan 996, task 6364 — `workspace regenerate`'s
# `.manifest-docs` merkle) ----------------------------------------------------
#
# Same shape as SQLite and Lua above: xxHash publishes release archives, but
# its CMake build lives at `cmake_unofficial/CMakeLists.txt` inside the repo
# rather than at the archive root, and that unofficial wrapper builds the
# `xxhsum` CLI, install rules, and a shared-library variant this tree has no
# use for. Rather than fight `SOURCE_SUBDIR` around a build we do not want,
# vendor the archive `DOWNLOAD_ONLY` and compile the two files this tree
# actually needs (`xxhash.c`, plus the `xxh3.h` it internally includes)
# ourselves, exactly as SQLite's amalgamation and Lua's interpreter sources
# are handled above.
#
# Only the STABLE public API (`XXH64`) is used — `engine::docs_manifest`
# calls nothing behind `XXH_STATIC_LINKING_ONLY`, so no experimental-API
# define is set here.
#
# Pinned to the exact tag tarball via `codeload.github.com`, per CLAUDE.md's
# vendoring rule: the friendlier `.../archive/refs/tags/v0.8.3.tar.gz` form
# 302-redirects, which CPM's underlying `file(DOWNLOAD ...)` does not follow.
# SHA256 computed by downloading the archive below and running
# `shasum -a 256` on it directly (task 6364).
CPMAddPackage(
  NAME xxHash
  VERSION 0.8.3
  URL https://codeload.github.com/Cyan4973/xxHash/tar.gz/refs/tags/v0.8.3
  URL_HASH SHA256=aae608dfe8213dfd05d909a57718ef82f30722c392344583d3f39050c7f29a80
  DOWNLOAD_ONLY YES
  EXCLUDE_FROM_ALL YES
  SYSTEM YES
)
if(xxHash_ADDED)
  add_library(xxhash STATIC "${xxHash_SOURCE_DIR}/xxhash.c")
  add_library(xxHash::xxhash ALIAS xxhash)
  target_include_directories(xxhash SYSTEM PUBLIC "${xxHash_SOURCE_DIR}")
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
  target_include_directories(tree_sitter_static SYSTEM PUBLIC "${tree_sitter_SOURCE_DIR}/lib/include")
  add_library(tree_sitter::tree_sitter ALIAS tree_sitter_static)
endif()
if(tree_sitter_zig_ADDED)
  add_library(tree_sitter_zig_static STATIC "${tree_sitter_zig_SOURCE_DIR}/src/parser.c")
  target_include_directories(tree_sitter_zig_static SYSTEM PUBLIC "${tree_sitter_zig_SOURCE_DIR}/src")
  target_link_libraries(tree_sitter_zig_static PUBLIC tree_sitter::tree_sitter)
  add_library(tree_sitter::zig ALIAS tree_sitter_zig_static)
endif()

# --- Mt-KaHyPar (decision 1006, task 6459 — groups recommend --solver mtkahypar) ---
#
# Supersedes the Zig-era Python-wheel bridge (`bin/mtkahypar`,
# `opt/mtkahypar/<v>/venv/`). Task 6457 found that bridge was never a real C
# library seam -- the installed wheel is a CPython extension with no
# `mtkahypar_*` C ABI -- so calling it from C++ would mean embedding a
# CPython interpreter, not linking a library. Decision 1006 (option b)
# instead resources building Mt-KaHyPar from its own C++ source and linking
# `libmtkahypar` directly.
#
# ## The vendor graph, and why it is three blocks rather than five-plus
#
# Mt-KaHyPar's own CMakeLists.txt unconditionally `FetchContent`s four
# further repositories to build even its library target. On THIS platform
# (Apple Silicon / arm64) two of those four never activate:
#
#   * `growt`  -- gated behind `KAHYPAR_USE_GROWT`, which upstream's own
#     CMakeLists.txt only turns on when `KAHYPAR_X86` is true (x86/amd64
#     `CMAKE_SYSTEM_PROCESSOR`). arm64 never sets it -- verified by reading
#     the upstream CMakeLists.txt gate directly, not assumed.
#   * `ParlayLib` -- gated behind `KAHYPAR_DISABLE_PARLAY` (default OFF,
#     i.e. parlay normally fetched). Explicitly disabled here via
#     `KAHYPAR_DISABLE_PARLAY ON` in the OPTIONS below -- upstream's own
#     doc comment says this "might impact running time of deterministic
#     mode on some instances", a performance tradeoff, not a correctness
#     one, and one this port accepts to keep the vendor graph small.
#   * `googletest` -- gated behind `KAHYPAR_ENABLE_TESTING` (default OFF,
#     left OFF). Planar supplies its own Catch2 tests.
#   * TBB's own `FetchContent` arm is gated behind `KAHYPAR_DOWNLOAD_TBB`
#     (default OFF, left OFF): upstream falls through to
#     `find_package(TBB 2021.5 COMPONENTS tbb tbbmalloc)` instead, which is
#     exactly the dynamic-TBB-via-Homebrew path decision 1006 accepts (see
#     below) -- no vendor block needed for TBB either.
#   * `hwloc` -- gated behind `KAHYPAR_DISABLE_HWLOC` (default OFF, i.e.
#     hwloc normally required via a second `find_package`). Decision 1006
#     accepts exactly ONE dynamic system dependency (TBB); a second
#     (hwloc, for NUMA-aware thread pinning this build does not enable
#     anyway -- `KAHYPAR_ENABLE_THREAD_PINNING` is also left at its default
#     OFF) would expand that surface beyond what was decided. Disabled here
#     via `KAHYPAR_DISABLE_HWLOC ON`.
#
# What is left needing a pinned CPM block, because upstream's own
# CMakeLists.txt calls them unconditionally regardless of platform:
#
#   * Mt-KaHyPar itself
#   * `kahypar-shared-resources` (header-only support library)
#   * `WHFC` (larsgottesbueren's fork; header-only flow algorithms)
#
# CLI11 is also fetched unconditionally by upstream, but this tree already
# vendors CLI11 (see the CLI11 block above) -- reused rather than fetched a
# second time.
#
# All three are pinned by tag/commit archive via `codeload.github.com`
# (the friendlier `archive/refs/...` form 302-redirects and CPM's
# `file(DOWNLOAD ...)` does not follow), SHA256 computed by downloading the
# archive and running `shasum -a 256` on it directly.
#
# `cmake/patches/mtkahypar-vendor-network.patch` neutralizes upstream's own
# `FetchContent_Declare(cli11 ...)` / `FetchContent_Populate(kahypar-shared-resources ...)`
# / `FetchContent_Populate(WHFC ...)` calls (the only three that fire
# unconditionally on this platform) so a configured build never reaches the
# network -- verified by configuring from a clean build dir with networking
# blocked (`sandbox-exec -p '(version 1)(deny network*)'`) after `vendor/`
# is populated.
#
# ## TBB: accepted dynamic dependency (decision 1006)
#
# Upstream states TBB does not support static linking. Rather than vendor
# and build oneTBB from source (`KAHYPAR_DOWNLOAD_TBB`, itself a further
# FetchContent), this pins to the system Homebrew `tbb` formula via
# `find_package(TBB)`, matching decision 1006's explicit acceptance of a
# dynamic TBB runtime dependency. `tbb` is added to `install.sh`'s
# `BUILD_DEPS`/`RUN_DEPS` and `README.md` § Prerequisites in the same
# change (CLAUDE.md: an un-manifested dependency silently breaks for users
# who lack it).
if(APPLE)
  execute_process(
    COMMAND brew --prefix tbb
    OUTPUT_VARIABLE PLANAR_TBB_BREW_PREFIX
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE PLANAR_TBB_BREW_RESULT)
  if(PLANAR_TBB_BREW_RESULT EQUAL 0 AND PLANAR_TBB_BREW_PREFIX)
    list(APPEND CMAKE_PREFIX_PATH "${PLANAR_TBB_BREW_PREFIX}")
  endif()
endif()

CPMAddPackage(
  NAME kahypar_shared_resources
  URL https://codeload.github.com/kahypar/kahypar-shared-resources/tar.gz/6d5c8e2444e4310667ec1925e995f26179d7ee88
  URL_HASH SHA256=bace1a64c1298f050085fc1db6163bf331b0731f9087a8df4a8aaf09fe62db0b
  DOWNLOAD_ONLY YES EXCLUDE_FROM_ALL YES SYSTEM YES)

CPMAddPackage(
  NAME WHFC
  URL https://codeload.github.com/larsgottesbueren/WHFC/tar.gz/51b27ff2c27a4794bfbaa4e8189d38159d249312
  URL_HASH SHA256=92307340ba2839214a074ffa329fe1591220b5e514a65af4281690b6666474de
  DOWNLOAD_ONLY YES EXCLUDE_FROM_ALL YES SYSTEM YES)

# These three must be set BEFORE CPMAddPackage(mtkahypar ...): CPM's
# add_subdirectory() for a non-DOWNLOAD_ONLY package runs synchronously
# inside that call, and the patched CMakeLists.txt (see the patch file
# above) reads these variables while it runs.
set(PLANAR_CLI11_TARGET CLI11::CLI11)
set(PLANAR_KAHYPAR_SHARED_RESOURCES_DIR "${kahypar_shared_resources_SOURCE_DIR}")
set(PLANAR_WHFC_DIR "${WHFC_SOURCE_DIR}")

# CUSTOM_CACHE_KEY pins the committed cache directory name (task 6461).
# Without it, CPM derives the directory from a SHA1 of this call's own
# unparsed arguments -- and the PATCHES keyword folds in an ABSOLUTE path
# (this file's own directory plus the system `patch` binary's absolute
# path, both baked in by CPM's cpm_add_patches()/PATCH_COMMAND machinery)
# before that hash is computed. That makes the resolved cache directory
# name depend on the absolute checkout path, not just on the package
# identity/options -- a clone or worktree at a different path re-derives a
# different directory and refetches from the network even though nothing
# about the dependency changed. Confirmed empirically: this worktree
# resolves the un-pinned call to a directory (`1501`) that differs from
# the identical call's directory in the checkout that originally vendored
# it (`3762`), and ON vs OFF `PLANAR_WITH_MTKAHYPAR` make no difference to
# either -- the CPMAddPackage call for mtkahypar itself is unconditional
# (only the *link* in src/engine/grouping/ is gated on the option), so
# the option was never the actual variable. A literal key tied to the pin
# (the released version tag) keeps the directory name stable across every
# checkout location while remaining a real vendored source tree, not a
# second copy of it.
CPMAddPackage(
  NAME mtkahypar
  URL https://codeload.github.com/kahypar/mt-kahypar/tar.gz/refs/tags/v1.6.2
  URL_HASH SHA256=f1e44b160e49d760a54249435a69b452ed4872082f59620edfc6a458e66b60c4
  CUSTOM_CACHE_KEY 1.6.2
  EXCLUDE_FROM_ALL YES
  SYSTEM YES
  PATCHES "${CMAKE_CURRENT_LIST_DIR}/patches/mtkahypar-vendor-network.patch"
  OPTIONS
    # ALWAYS Release, even under a Debug Planar build (task 6425).
    #
    # This dependency was built at ${CMAKE_BUILD_TYPE}, which made a 311MB
    # DEBUG `libmtkahypar.dylib`, and dyld maps it on EVERY exec of `planar`.
    # Measured on one quiet machine, three binaries from the SAME Debug
    # build, so build type is controlled out and the only variable is
    # whether the binary links this library:
    #
    #     planar         36M   links mtkahypar   573 ms/spawn
    #     planar-agent   10M   no                 25 ms/spawn
    #     planar-watch  9.5M   no                 22 ms/spawn
    #
    # 23x, paid by every black-box case that execs `planar` — and
    # `cmd_planar` is 83% of the ctest suite's wall time.
    #
    # Safe because the seam is a C ABI: `mtkahypar.h` is confined to
    # `src/engine/grouping/mtkahypar.cpp`'s translation unit, and the
    # pinned toolchain enables no libc++ hardening or _GLIBCXX_DEBUG mode
    # that would change a layout across the boundary. Nobody debugs into
    # this library; it is SYSTEM and EXCLUDE_FROM_ALL for the same reason.
    "CMAKE_BUILD_TYPE Release"
    "KAHYPAR_ENABLE_TESTING OFF"
    "KAHYPAR_INSTALL_CLI OFF"
    "KAHYPAR_DOWNLOAD_TBB OFF"
    "KAHYPAR_DISABLE_PARLAY ON"
    "KAHYPAR_DISABLE_HWLOC ON"
    "KAHYPAR_PYTHON OFF"
    "KAHYPAR_BUILD_DEBIAN_PACKAGE OFF"
)

# --- First-party dependencies: external/, never committed -------------------
#
# The vendoring rule above (pinned archive, committed under vendor/) exists to
# protect against THIRD-PARTY upstreams moving or vanishing. It does not apply
# to repositories this project's owner controls (decision recorded on plan
# 1033, amending tech-spec D8): they are still pinned by URL + URL_HASH
# SHA-256, so a build is still exact, but their cache lives under the
# gitignored `external/` directory and is NEVER committed. Only the hash is
# the contract; the bytes are re-fetched once per checkout.
#
# Mechanism: CPM derives its cache directory from CPM_SOURCE_CACHE at call
# time, so a NORMAL variable set around a first-party CPMAddPackage call
# shadows the vendor/ cache entry for that call only. A populated
# external/<name>/<CUSTOM_CACHE_KEY> is reused without any download, exactly
# as vendor/ is; an empty one is fetched and hash-checked.
set(PLANAR_EXTERNAL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external" CACHE PATH
  "Where CPM caches first-party (owner-controlled) dependency sources; gitignored, never committed")

# --- Centurion (plan 1033 M1, tasks 6495/6496; tech-spec D8, decision 1041) --
#
# Centurion becomes Planar's workflow engine (decision 1007); `planar-execute`
# links `centurion::client` and NOTHING else of it (tech-spec D1, guarded by
# cmake/architecture.cmake). Pinned to Centurion's first pre-release tag,
# v0.1.0-alpha.4 (commit b98cb4aa). Three decisions this client depends on,
# each closing a place where Centurion assumed an embedding host that ships
# C++: ADR-0055 lets a stock daemon INSTALL Planar's Lua from a configured
# directory, ADR-0056 composes the registry that makes it EVALUATE, and
# ADR-0057 lets the principal that admitted a run CONTROL it without a
# console session — which is what `cancel` needs.
#
# First-party, so external/, not vendor/: the archive is Centurion's WHOLE
# source tree including its own committed vendor/ (gRPC 1.82.1, BoringSSL,
# protobuf 35.1, abseil, ...), ~590 MB expanded (question 984). Centurion
# vendors gRPC deliberately (its ADR-0032 rejects a system gRPC so generated
# wire code and runtime share one pinned protobuf).
#
# The repository is PRIVATE, so the first configure of a checkout needs a
# token (the pinned URL 404s anonymously), exactly as Centurion itself does
# for `etc`:
#   GITHUB_TOKEN=$(gh auth token) cmake --preset debug
# Later configures read external/centurion/v0.1.0-alpha.1 and need none.
# CUSTOM_CACHE_KEY pins that path independently of the auth header (CPM
# otherwise hashes every download argument into the cache key).
#
# DOWNLOAD_ONLY: cmake/centurion.cmake adds the tree as a subdirectory and
# owns its options, the dependency sharing, and the redirect that points
# Centurion at its own vendor/ tree.
#
# The pin is held in PLANAR_CENTURION_* variables (task 6709) because it has a
# second reader: cmake/centurion.cmake writes it to centurion-pin.env in the
# build tree, which install.sh hands to scripts/install-centuriond.sh so the
# installed daemon is built from, and records, this exact pin.
set(PLANAR_CENTURION_TAG "v0.1.0-alpha.4")
set(PLANAR_CENTURION_VERSION "0.1.0-alpha.4")
set(PLANAR_CENTURION_COMMIT "b98cb4aab4c08c4c5820716a7a9c74100795e507")
set(PLANAR_CENTURION_URL
  "https://codeload.github.com/rdrsss/centurion/tar.gz/refs/tags/${PLANAR_CENTURION_TAG}")
set(PLANAR_CENTURION_SHA256 "d60f09c06216770dbbdef177d6b6a197c6f3d54fbdfec80f8a76aaf8ecd42c40")
if(DEFINED ENV{GITHUB_TOKEN})
  set(_planar_centurion_auth HTTP_HEADER "Authorization: Bearer $ENV{GITHUB_TOKEN}")
else()
  set(_planar_centurion_auth "")
endif()
set(CPM_SOURCE_CACHE "${PLANAR_EXTERNAL_DIR}") # normal variable: this call only
CPMAddPackage(
  NAME centurion
  VERSION ${PLANAR_CENTURION_VERSION}
  URL ${PLANAR_CENTURION_URL}
  URL_HASH SHA256=${PLANAR_CENTURION_SHA256}
  CUSTOM_CACHE_KEY ${PLANAR_CENTURION_TAG}
  ${_planar_centurion_auth}
  DOWNLOAD_ONLY YES)
unset(CPM_SOURCE_CACHE) # the vendor/ cache entry is visible again
unset(_planar_centurion_auth)
