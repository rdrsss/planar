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
# to TEN copies before this module existed.
#
# THE COUNT, corrected (task 6114, review finding F3). The comment that first
# stood here said SEVEN, PRE-EXISTING, and byte-identical. All three were
# wrong:
#
#   - TEN, not seven. Seven spelled `append_json_string`
#     (closure/store.cpp, grouping/load.cpp, ingest/render.cpp,
#     models/render.cpp, planning/test_spec_status.cpp, runs/render.cpp,
#     workflows/render.cpp) and THREE more spelled `json_quote`
#     (planning/annotation.cpp, runtime/capture.cpp, promotion/promotion.cpp).
#     The original comment never mentioned the `json_quote` family at all.
#
#   - Not "pre-existing". Six of the seven were created INSIDE the same union
#     diff (d071fae, 45ccf72) that then deferred sweeping them. Only
#     ingest/render.cpp predates the M4 base. Deferring on the grounds that
#     each rewrite "would be an unexplainable entry in the reviewer's
#     `git diff --stat`" was inverted under barrel-deferred review, where the
#     whole union IS one diff — and the deferral is precisely the mechanism by
#     which the count grew from one to ten.
#
#   - Not byte-identical. The three `json_quote` copies were BROKEN: they
#     handled `"`, `\`, `\n`, `\r`, `\t` and sent everything else below 0x20
#     to the `\u00xx` arm, including 0x08 and 0x0C. The oracle emits the SHORT
#     forms `\b` and `\f` there. Verified against the live binary, not
#     inferred: `annotate add --body <0x08 0x0C 0x01> --json` answered
#     `"bs\bhere\fff\u0001soh"`. So a sweep scoped to "seven byte-identical
#     copies" would have been a safe find-and-replace that left the three
#     actually-broken ones untouched. That was review finding F2, and
#     `planning/annotation.cpp` reached the bug from operator free text and
#     from extracted source, where 0x0C is a real character.
#
# SCOPE: the sweep is DONE. All ten call sites now import this module and the
# tree carries exactly ONE definition of the escape table. Doing it here rather
# than filing it was deliberate: after F2 the remaining seven really were
# byte-identical (each was diffed against this definition before deletion), and
# every one of their buckets carries byte-for-byte oracle pins that fail
# instantly on drift — so the mechanical half is cheap to verify, while leaving
# it filed would have meant keeping a deferral note whose reasoning had just
# been shown to be the thing that let the count reach ten.
#
# A new renderer MUST import `planar.json_text`. There is no eleventh copy to
# copy from.
#
# The escaping table is only half the parity contract; the other half is the
# STDOUT TERMINATOR CONTRACT, stated in each engine bucket's CMakeLists.txt.

planar_module(json_text
  INTERFACE json_text.cppm
  SOURCES json_text.cpp)
