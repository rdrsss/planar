# planar.core: the core bucket. check and thresholds are its interface
# partitions; check.cpp is its implementation unit.
#
# planar_module() globs *.t.cpp automatically, so no SOURCES/test
# wiring is needed here beyond the files existing.

planar_module(core
  INTERFACE check.cppm thresholds.cppm
  SOURCES check.cpp)
