# planar.buildqueue — layer-1 home of the host build queue's database and log
# locations (plan 1163, decision 1438).
#
# Layer 1 by name (cmake/architecture.cmake classifies anything not `cmd_*` /
# `engine_*` as layer 1). DEPENDS nothing: the resolver is a pure function of
# an injected environment lookup, so `planar-build-queue` and Planar's reader
# can resolve the same path through one target edge without either binary
# linking the other's engine.
#
# The module is named `buildqueue` for its target and test label. A later task
# adds `planar.buildqueue.reader` to this directory under the same target.
planar_module(buildqueue
  INTERFACE path.cppm
  SOURCES path.cpp)
