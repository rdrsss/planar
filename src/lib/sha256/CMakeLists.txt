# `planar.sha256` is the layer-1 FIPS 180-4 digest primitive shared by
# independent engines (D19).  It deliberately has no storage or filesystem
# dependencies, so importer and external-sync can share identical bytes.
#
# Interface-only (no companion sha256.cpp): doxygen 1.18.0's C++20 modules
# parser cannot associate hex()'s doc comment with the merged declaration
# when the declaration and definition are split across the interface unit
# and a separate implementation file for a module this small — confirmed
# by isolated repro, see sha256.cppm's header. Matches core/version.cppm's
# existing interface-only precedent in this tree.
planar_module(sha256 INTERFACE sha256.cppm)
