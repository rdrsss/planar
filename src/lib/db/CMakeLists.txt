# planar.db — RAII connection/statement/transaction wrappers over the
# vendored SQLite amalgamation (plan 996, task cpp-db-wrapper). Layer-1
# base module: no DEPENDS on engine/cmd (tech-spec § "db module", D15).

planar_module(db
  INTERFACE db.cppm
  SOURCES db.cpp)

target_link_libraries(planar_db PRIVATE SQLite::SQLite3)

# The test binary links SQLite directly too — it exercises open()/
# open_read_only() against real on-disk databases and needs the same
# vendored amalgamation.
if(TARGET planar_db_tests)
  target_link_libraries(planar_db_tests PRIVATE SQLite::SQLite3)
endif()
