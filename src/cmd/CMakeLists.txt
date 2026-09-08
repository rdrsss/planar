# src/cmd/ — layer-3 command binaries (tech-spec § Target layout; D15's
# `cmd -> engine.* -> {cli, db} -> vendored`).
#
# `planar` (plan 996, task 6105) is the first. `planar-agent`,
# `planar-watch` and `planar-execute` (task 6107) land beside it.
# `planar-ext` (task 6418; task 6412, decisions 995-1000) is the fifth —
# the external-plane extraction's skeleton step, landing no `ext`/`sync`
# verb of its own yet.
#
# Five sibling directories, no shared library between them, and that is D18
# enforced rather than a missed refactor: cmake/architecture.cmake FATALs on
# a `cmd_* -> cmd_*` edge. The vocabulary that looks shared — each binary's
# `context`, `exit`, `handler`, `dispatch`, `tree` — is precisely what the
# capability boundary makes DIFFERENT per binary (five distinct database
# policies now that `planar-ext` adds a write-capability ALLOWLIST no other
# binary enforces at the connection level; two divergent exit-code
# policies). What is genuinely shared already lives one layer down in
# `planar.cli`. The full argument is in src/cmd/planar-agent/CMakeLists.txt.
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
add_subdirectory(planar-ext)

# --- cross-binary black-box coverage (plan 996, task 6547, decision 1035) ---
#
# `planar/cross_process.t.cpp` ports the part of the Zig integration suite
# that no in-process test can reach: several REAL processes contending on one
# database while a fourth watches, and every built binary's ability to START
# at all (task 6536 — a solver-ON build produced a `planar` that died at
# dyld with exit 134 while all 3,422 in-process tests passed).
#
# It therefore needs the paths of its four SIBLING binaries, which is why the
# block lives here rather than in `planar/CMakeLists.txt`: at that point in
# the configure the sibling targets do not exist yet, so `add_dependencies`
# below would FATAL.
#
# THIS CREATES NO `cmd_* -> cmd_*` LINK EDGE, and that distinction is what
# keeps D18 satisfied. cmake/architecture.cmake's walk reads exactly two
# target properties — LINK_LIBRARIES and INTERFACE_LINK_LIBRARIES. A compile
# definition carrying a `$<TARGET_FILE:...>` path is a string, and
# add_dependencies() is an order-only build edge; neither appears in either
# property. The test target still only ever LINKS its own binary's modules.
if(TARGET planar_cmd_planar_tests)
  target_compile_definitions(planar_cmd_planar_tests PRIVATE
    "PLANAR_AGENT_CPP_BIN=\"$<TARGET_FILE:planar_cmd_planar_agent>\""
    "PLANAR_WATCH_CPP_BIN=\"$<TARGET_FILE:planar_cmd_planar_watch>\""
    "PLANAR_EXECUTE_CPP_BIN=\"$<TARGET_FILE:planar_cmd_planar_execute>\""
    "PLANAR_EXT_CPP_BIN=\"$<TARGET_FILE:planar_cmd_planar_ext>\"")
  add_dependencies(planar_cmd_planar_tests
    planar_cmd_planar_agent
    planar_cmd_planar_watch
    planar_cmd_planar_execute
    planar_cmd_planar_ext)
endif()
