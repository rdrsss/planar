# ------------------------------------------------------------------------------
# Centurion as a source subdirectory (plan 1033 M1, task 6496).
#
# cmake/dependencies.cmake fetches the pinned Centurion archive into external/
# DOWNLOAD_ONLY (decision 1143); this file adds it. Planar consumes
# `centurion::client` ONLY. Nothing here links it yet: the target only has to
# exist, EXCLUDE_FROM_ALL, so a plain build compiles none of it.
#
# ## One pin per shared package
#
# CPM is first-wins by package NAME, and dependencies.cmake runs before this
# file, so every package both trees declare resolves to PLANAR's pin and
# Centurion's own CPMAddPackage for it is a no-op:
#
#   spdlog 1.17.0   same version; Centurion's block is `if(NOT TARGET spdlog::spdlog)`
#   sqlite 3.53.3   same version (Planar bumped to Centurion's pin at this task);
#                   same target names, `sqlite3` / `SQLite::SQLite3`
#   lua    5.5.0    Planar's `lua_static`; Centurion links `Lua::Lua`, aliased below
#   curl   8.7.1    Planar's `CURL::libcurl`. Centurion asks for 8.21.0 and CPM
#                   prints "Requires a newer version of curl" — EXPECTED, and
#                   the one version warning this configure is allowed to emit.
#                   Planar cannot move: curl 8.15 removed SecureTransport, which
#                   is Planar's macOS TLS backend. Centurion's curl is HTTP-only
#                   and only reached from runtime targets, never the client link
#                   set; the audit lives at task 6500.
#   Catch2          never reached: BUILD_TESTING is OFF for Centurion's scope
#
# The rest of Centurion's stack — gRPC, protobuf, abseil, BoringSSL, c-ares,
# re2, zlib, botan, libuv, simdjson, uuidv7, etc — has no Planar counterpart
# and resolves from Centurion's OWN vendor/ tree inside the archive. That tree
# is part of the first-party archive, so it lives under external/ with the rest
# of it and is never committed here (decision 1143). FTXUI is only declared
# when CENTURION_BUILD_CLI is on, so it is absent from this configure entirely.
#
# ## Why every setting is a normal variable inside block()
#
# Centurion declares `set(CPM_SOURCE_CACHE ... CACHE PATH)`; Planar's own cache
# entry (vendor/) already exists, so without the normal-variable override here
# Centurion's packages would be looked up in, and downloaded into, Planar's
# COMMITTED vendor/. Its options (CENTURION_BUILD_*, BUILD_TESTING via
# include(CTest)) are plain option() calls, which honour a normal variable
# under CMP0077. Using normal variables in a block() keeps all of it out of
# Planar's cache and out of Planar's own directory scope.
# ------------------------------------------------------------------------------

if(NOT DEFINED centurion_SOURCE_DIR)
  message(FATAL_ERROR "cmake/centurion.cmake: the centurion package was not "
    "declared; include(dependencies) must run first")
endif()

# Centurion links `Lua::Lua`; Planar's Lua target is `lua_static` (alias
# `lua::lua`). Same archive, same version: expose it under Centurion's name
# rather than letting a second Lua be compiled.
if(NOT TARGET Lua::Lua)
  add_library(Lua::Lua ALIAS lua_static)
endif()

block(SCOPE_FOR VARIABLES)
  set(CENTURION_BUILD_DAEMON OFF)
  set(CENTURION_BUILD_CLI OFF)
  set(BUILD_TESTING OFF)
  set(CPM_SOURCE_CACHE "${centurion_SOURCE_DIR}/vendor")
  add_subdirectory("${centurion_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/_deps/centurion-build"
    EXCLUDE_FROM_ALL SYSTEM)
endblock()

if(NOT TARGET centurion::client)
  message(FATAL_ERROR "cmake/centurion.cmake: centurion::client did not resolve "
    "after add_subdirectory")
endif()
