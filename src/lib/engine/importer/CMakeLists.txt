# `planar.engine.importer` owns the filesystem-only half of `planar import`.
# It deliberately depends only on the layer-1 JSON renderer: planning writes
# are composed by the command handler, avoiding a D15 engine-to-engine edge.
planar_module(engine_importer
  INTERFACE importer.cppm
  SOURCES importer.cpp
  DEPENDS json_text json_dom sha256)
