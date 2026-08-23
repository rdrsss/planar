# planar.json_text — layer-1 JSON string-escaping primitive (plan 996, tasks
# 6109/6110/6111).
#
# Layer 1 by name (cmake/architecture.cmake classifies anything not `cmd_*` /
# `engine_*` as layer 1), same as `core` / `db` / `cli` / `scope_ref`.
#
# DEPENDS nothing. The whole module is one pure function over
# `std::string_view`; it holds no SQLite handle, reads no environment, and
# touches no filesystem.
#
# WHY IT EXISTS (D19). Every JSON surface in this tree is hand-rolled and
# pinned byte-for-byte against the Zig oracle's `std.json.Stringify`, so every
# renderer needs the identical escape table. D18 forbids `engine_*` ->
# `engine_*` edges, so the only two legal shapes are "duplicate per bucket" or
# "extract downward to layer 1". Duplication was the standing choice and it ran
# to SEVEN byte-identical copies (closure/store.cpp, grouping/load.cpp,
# ingest/render.cpp, models/render.cpp, planning/test_spec_status.cpp,
# runs/render.cpp, workflows/render.cpp) before this module was created;
# `local`, `workspace` and the `models resolve` work would have made ten. The
# comment in engine/workflows/render.cpp committed to extracting "once a THIRD
# bucket needs it" — the count was already well past three when that was
# written. This is that extraction, done once, downward, where both D18 and D19
# are satisfied.
#
# SCOPE OF THE EXTRACTION, stated so the next reader is not misled. The three
# buckets added by tasks 6109/6110/6111 import this module; the seven
# pre-existing copies are NOT rewritten here. Sweeping them would put fifteen
# files of pure churn into a cycle whose subject is twelve CLI leaves, and each
# one would be an unexplainable entry in the reviewer's `git diff --stat`. They
# are byte-identical to this definition, so the sweep is mechanical and belongs
# in its own cycle with its own task row (filed against plan 996). What matters
# now is that the count stopped growing: a new renderer MUST import
# `planar.json_text` rather than adding an eighth copy.

planar_module(json_text
  INTERFACE json_text.cppm
  SOURCES json_text.cpp)
