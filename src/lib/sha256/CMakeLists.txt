# `planar.sha256` is the layer-1 FIPS 180-4 digest primitive shared by
# independent engines (D19).  It deliberately has no storage or filesystem
# dependencies, so importer and external-sync can share identical bytes.
planar_module(sha256 INTERFACE sha256.cppm SOURCES sha256.cpp)
