# planar.db — RAII connection/statement/transaction wrappers over the
# vendored SQLite amalgamation (plan 996, task cpp-db-wrapper). Layer-1
# base module: no DEPENDS on engine/cmd (tech-spec § "db module", D15).
#
# planar.db.migrations / planar.db.migrate (plan 996, task
# cpp-db-migrations-embed) are additional module interfaces built into
# this SAME `planar_db` CMake target rather than their own planar_module()
# targets: architecture.cmake's D15 layering classifies every non-cmd_/
# engine_ module as layer 1, so a separate `planar_db_migrations` target
# would sit in the same layer as `planar_db` and a `planar_db` ->
# `planar_db_migrations` dependency edge would fail the strictly-downward
# check. Multiple distinct C++ module names compiling into one static
# library target is fine — only the CMake target graph is what
# architecture.cmake walks.
#
# The migrations data (`migrations.generated.cpp`) is generated at
# configure time by cmake/generate_migrations.cmake, straight into the
# build tree (never src/) — see that file for the enumeration + explicit
# sort contract.

include(generate_migrations)

set(_planar_db_migrations_generated "${CMAKE_CURRENT_BINARY_DIR}/generated/migrations.generated.cpp")
planar_generate_migrations(
  MIGRATIONS_DIR "${CMAKE_SOURCE_DIR}/migrations"
  OUT_FILE "${_planar_db_migrations_generated}"
  MODULE_NAME "planar.db.migrations")

planar_module(db
  INTERFACE
    db.cppm
    migrations.cppm
    migrate.cppm
  SOURCES
    db.cpp
    migrate.cpp
    "${_planar_db_migrations_generated}"
  EMBED)

target_link_libraries(planar_db PRIVATE SQLite::SQLite3)

# The test binary links SQLite directly too — it exercises open()/
# open_read_only() against real on-disk databases and needs the same
# vendored amalgamation.
if(TARGET planar_db_tests)
  target_link_libraries(planar_db_tests PRIVATE SQLite::SQLite3)
  # migrate.t.cpp compares the embedded chain with the DIRECTORY it claims
  # to embed, so the test binary knows the stream directory in the source tree.
  target_compile_definitions(planar_db_tests PRIVATE
    "PLANAR_MIGRATIONS_DIR=\"${CMAKE_SOURCE_DIR}/migrations\"")
endif()
