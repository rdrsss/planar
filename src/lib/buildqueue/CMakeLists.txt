# planar.buildqueue — layer-1 home of the host build queue's database and log
# locations (decision 1438).
#
# Layer 1 by name (cmake/architecture.cmake classifies anything not `cmd_*` /
# `engine_*` as layer 1). The path resolver is a pure function of an injected
# environment lookup, so `planar-build-queue` and Planar's reader can resolve
# the same path through one target edge without either binary linking the
# other's engine.
#
# The module is named `buildqueue` for its target and test label. The reader
# (`planar.buildqueue.reader`) is the one module that imports `planar.db`, the
# layer-1 SQLite wrapper: it opens the queue database read-only and checks
# columns, not versions. DEPENDS db is the only edge the target takes.
planar_module(buildqueue
  INTERFACE
    path.cppm
    reader.cppm
  SOURCES
    path.cpp
    reader.cpp
  DEPENDS db)
