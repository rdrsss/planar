# planar.git — layer-1 git-subprocess seam (plan 996, tasks 6128 and 6137).
#
# Layer 1 by name (cmake/architecture.cmake classifies anything not `cmd_*`
# / `engine_*` as layer 1), same as `db` / `log` / `scope_ref`. It has to be
# layer 1 rather than a shared helper inside one of the two `cmd_*` binaries
# that already had a private copy, because the fourth consumer —
# `detect_worktree`, called from `planar.engine.identity.scope` — is layer 2
# and a layer-2 -> layer-3 edge is exactly what D15 forbids.
#
# DEPENDS nothing: `popen`/`pclose`/`std::filesystem` only. No DB handle, no
# logging, no JSON. Keeping it dependency-free is deliberate — every layer
# above it can take the edge without dragging anything else along.

planar_module(git
  INTERFACE git.cppm
  SOURCES git.cpp)
