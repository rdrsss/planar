# `planar.engine.synthesize` owns only the filesystem/cache half of the
# synthesize protocol. Planning and workbench writes meet it at layer 3.
planar_module(engine_synthesize
  INTERFACE synthesize.cppm
  SOURCES synthesize.cpp
  DEPENDS json_text json_dom sha256)
