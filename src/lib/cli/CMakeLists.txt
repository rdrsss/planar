# planar.cli — the etcli command-tree/parser/help/completion port (plan
# 996, task cpp-cli-tree-parity). Layer-1 base module: no DEPENDS on
# engine/cmd (D15). `validate` and `schema` (task 6029) are not part of
# this CMake target yet — cli.cppm's facade grows an
# `export import planar.cli.schema;` line when that lands, at which point
# this file gains a schema.cppm/.cpp/.t.cpp trio the same way the others
# are declared below.
#
# Test binaries shell the reference Zig binaries (parser.t.cpp,
# help.t.cpp) to assert byte-for-byte exit-code and help-text parity for
# the modeled verbs (task add/done under `./zig/zig-out/bin/planar`, agent
# fail under `./zig/zig-out/bin/planar-agent`) — same SKIP-when-absent
# pattern as src/lib/db/migrate.t.cpp (D6, the zig/ tree is the parity
# oracle until M10 cutover).

planar_module(cli
  INTERFACE
    flag.cppm
    cmd.cppm
    error.cppm
    parser.cppm
    help.cppm
    completion.cppm
    cli.cppm
  SOURCES
    parser.cpp
    help.cpp
    completion.cpp)

if(TARGET planar_cli_tests)
  target_compile_definitions(planar_cli_tests PRIVATE
    PLANAR_ZIG_PLANAR_BIN="${CMAKE_SOURCE_DIR}/zig/zig-out/bin/planar"
    PLANAR_ZIG_PLANAR_AGENT_BIN="${CMAKE_SOURCE_DIR}/zig/zig-out/bin/planar-agent")
endif()
