# src/cmd/ — layer-3 command binaries (tech-spec § Target layout; D15's
# `cmd -> engine.* -> {cli, db} -> vendored`).
#
# `planar` (plan 996, task 6105) is the first. `planar-agent`,
# `planar-watch` and `planar-execute` (task 6107) land beside it.
#
# Four sibling directories, no shared library between them, and that is D18
# enforced rather than a missed refactor: cmake/architecture.cmake FATALs on
# a `cmd_* -> cmd_*` edge. The vocabulary that looks shared — each binary's
# `context`, `exit`, `handler`, `dispatch`, `tree` — is precisely what the
# capability boundary makes DIFFERENT per binary (four distinct database
# policies; two divergent exit-code policies). What is genuinely shared
# already lives one layer down in `planar.cli`. The full argument is in
# src/cmd/planar-agent/CMakeLists.txt.
#
# `parity_harness.hpp` beside these directories is the one deduplicated
# piece, and it is a plain header — included, never linked — so it creates
# no target edge. See its own header for why a library there would be both
# illegal and actively harmful (it would drag planar_db into
# planar-execute's link closure).
#
# Every binary here MUST be declared with planar_binary() (cmake/module.cmake),
# never a bare add_executable(): the helper is what registers the target into
# PLANAR_MODULE_TARGETS so cmake/architecture.cmake's D15 walk sees it at all.
# A raw add_executable() sits outside the guard entirely — the exact gap task
# 6050 found and cmake/tests/architecture-guard-fixture/cmd-binary-db/ still
# stands guard over.

add_subdirectory(planar)
add_subdirectory(planar-agent)
add_subdirectory(planar-watch)
add_subdirectory(planar-execute)
