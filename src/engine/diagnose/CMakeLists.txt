# planar_engine_diagnose — engine/diagnose bucket (plan 1132, tech spec 689
# § Module boundaries): the read-only diagnosis framework behind
# `planar-watch diagnose` and its later callers.
#
# Layer-2 `engine_*` module. It imports only layer-1 modules: `db` for the
# connection, `incident_model` for the shared value types, `json_text` for the
# one JSON string escape and `core` for the invariant guard. It has no edge to
# any other `engine_*` bucket, to `engine_incidents_*` or to a `cmd_*` target
# (D15/D18); plan descendants and claim liveness are resolved with its own SQL.
# Individual checks (plan 1132 M1 tasks 7374 onward) are added to the catalog in
# its family's `checks_<family>.cpp`; this bucket's tests use synthetic catalogs.

planar_module(engine_diagnose
  INTERFACE diagnose.cppm
  SOURCES
    diagnose.cpp
    checks_claims.cpp
    checks_dispatch.cpp
    checks_cli.cpp
    checks_records.cpp
    checks_queue.cpp
  DEPENDS
    core
    db
    incident_model
    json_text
  TEST_DEPENDS
    json_dom)

if(TARGET planar_engine_diagnose_tests)
  # The suite opens real on-disk scratch planar.db files and takes locks from
  # a second connection, as engine/hostqueue's does.
  target_link_libraries(planar_engine_diagnose_tests PRIVATE SQLite::SQLite3)
  # sources.t.cpp scans this directory's own sources for SQL that would
  # limit before filtering (the defect behind task 7333).
  target_compile_definitions(planar_engine_diagnose_tests PRIVATE
    "PLANAR_DIAGNOSE_SOURCE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}\"")
endif()
