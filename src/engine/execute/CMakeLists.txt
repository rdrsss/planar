# planar_engine_execute — the sandboxed Lua workflow host (plan 996, task
# 6042; plan 633 for the binary this serves).
#
# --- THIS TARGET IS THE ARCHITECTURE GUARD'S EXECUTE CARRIER ---------------
#
# cmake/architecture.cmake carries one semantic exception layered on top of
# the layer numbers: engine.execute — and anything that reaches it — must
# NEVER depend on planar_db, even though `db` sits in a strictly lower layer
# and every other engine bucket may take it freely. planar-execute holds no
# SQLite handle; it reaches Planar state only by shelling `planar` /
# `planar-agent` / `planar-watch`.
#
# Until this file existed, that exception fired on exactly one thing: the
# LITERAL target name `cmd_planar_execute`, matched by an arm the guard's
# header describes at length (and which is why the binary is declared
# `planar_binary(planar_execute ...)` rather than the natural
# `planar_binary(execute ...)` — the latter yields `planar_cmd_execute`,
# misses the literal, and leaves the exception guarding nothing). The guard
# ALSO has an edge-derived arm: any target whose link closure reaches
# `planar_engine_execute` is treated as a carrier regardless of its own name.
# That arm has been dead code since it was written, because no
# `engine_execute` target existed.
#
# It is live now. `cmd_planar_execute` links this module, so the no-SQLite
# rule now reaches it by BOTH routes, and the literal-name arm is finally
# belt-and-braces rather than the only guard — exactly the transition
# src/cmd/planar-execute/CMakeLists.txt predicted. Adding `db` to DEPENDS
# below fails CONFIGURE with a named diagnostic, and so does adding it to any
# future target that links this one.
#
# DEPENDS is therefore deliberately minimal and every entry should be read as
# a claim:
#
#   json_text  — the one JSON string-escaping primitive in the tree
#                (`append_json_string`). `flow.result` marshals a Lua table
#                to JSON and needs exactly that; writing an eleventh escaper
#                here was not an option (see json_text/CMakeLists.txt).
#   json_dom   — added at task 6125 for the `state`/`schema` parse helpers
#                behind `ctx.brief`: `planar plan show`/`task show`/`task
#                packet --json` and `planar-agent schema` all need typed
#                field extraction from a decoded document, and json_dom is
#                the one JSON DOM in the tree (D19) rather than a twelfth
#                bespoke recursive-descent reader. It is a layer-1 leaf with
#                no db import of its own, so this does not touch the
#                no-SQLite exception above.
#
# That is the whole list. No db. No cli — planar-execute parses argv by hand
# and has no `schema` catalog, which is why zig's capability_boundary_test
# and tools/cli_usage_lint both exclude it; wiring it onto planar.cliapp
# would manufacture a catalog the reference binary does not have.
#
# --- Lua ------------------------------------------------------------------
#
# lua::lua is the static library cmake/dependencies.cmake builds from the
# pinned 5.5.0 release archive (DOWNLOAD_ONLY + our own sources list, the
# same treatment SQLite gets, because Lua ships a Makefile and no CMake
# build). PRIVATE, not PUBLIC: `lua_State` appears NOWHERE in this module's
# exported interface. That is a boundary, not an accident — execute.cppm
# exports no way to obtain a state, register a function into one, or reach
# the fork/exec runner that `cli.*` and `git.*` shell through. See
# execute.cppm's header.
#
# --- what is ported and what is not ---------------------------------------
#
# All twenty-five host functions are complete as of task 6125. `ctx.brief`
# was the last: its Zig original renders a coder brief out of
# zig/src/cmd/planar-execute/{brief,schema,state}.zig. Only the SLICE of
# `state.zig`/`schema.zig` that `ctx.brief` actually consumes is ported
# (`planShow`, `taskShow`, the `TaskPacket` family, `loadSchema`/`BinSchema`)
# — see `execute.cppm`'s `state` namespace doc comment for why the oracle's
# remaining `state.zig` helpers (`recommendStrategy`'s typed struct,
# `contextList`, `claimStatus`, `testSpecStatus`, `questionAdd`,
# `taskTouchesList`'s typed struct) serve ONLY `main.zig`'s retired
# orchestration loop and have no consumer on this ported host surface;
# porting them would be dead code behind the very re-entrant-harness door
# plan 633 D5 exists to keep shut.
#
# STDOUT TERMINATOR CONTRACT (task 6114): `run_workflow` writes a COMPLETE
# stdout payload — the JSON document plus its trailing newline — directly to
# the stream it is handed. The layer-3 caller appends nothing.

planar_module(engine_execute
  INTERFACE
    execute.cppm
  SOURCES
    manifest.cpp
    host.cpp
    state.cpp
    schema.cpp
    brief.cpp
    config.cpp
  DEPENDS
    json_text
    json_dom)

# Glaze decodes `planar config show --json` in config.cpp (task 6485): the
# execute engine reads the config plane through the sibling `planar`, never
# through `engine_config`, which reaches `db`.
target_link_libraries(planar_engine_execute PRIVATE lua::lua glaze::glaze)

if(TARGET planar_engine_execute_tests)
  target_link_libraries(planar_engine_execute_tests PRIVATE lua::lua glaze::glaze)
endif()
