# planar.process — layer-1 process-spawn seam (plan 996, task 6272).
#
# Layer 1 by name (cmake/architecture.cmake classifies anything not `cmd_*`
# / `engine_*` as layer 1), same as `db` / `git` / `json_text` / `policy`.
#
# Layer 1 is FORCED, not chosen, for the reason `scope_ref`, `json_text`,
# `policy` and `git` each record independently: the next consumer in line is
# `engine.runtime.sessioncommits` (layer 2, the git-walk half `capture
# commits` and `bench harvest` are waiting on), and a layer-2 -> layer-3
# edge FATALs at configure time. Both of this module's bodies came DOWN from
# layer 3 — `editor.cpp`'s `spawn_inherit`/`resolve_program` and
# `handlers/shared/ext_adapter_factory.cpp`'s `spawn_capture` — where they were
# reachable only from `cmd_planar`.
#
# WHAT THIS MODULE DID NOT DO, deliberately:
#
#   - `planar.git` is NOT folded in. Its `popen`-based `run()` deliberately
#     ERASES the spawn-failure/non-zero-exit distinction this module exists
#     to preserve ("no answer, never a wrong answer"), its arguments are
#     shell-quoted, and its behavior was measured against four consumers. It
#     is a git-probe vocabulary, not a spawn vocabulary. Merging them would
#     mean picking one of two incompatible error contracts.
#   - `engine/execute/host.cpp`'s `capture_process` is NOT folded in. It is
#     internal-linkage on purpose: `planar_engine_execute` is the
#     architecture guard's "execute carrier" and must not grow a reachable
#     surface into the rest of the tree.
#
# DEPENDS nothing: `fork`/`execv`/`posix_spawnp`/`waitpid` and `<filesystem>`
# only. No DB handle, no logging, no JSON. Keeping it dependency-free is
# deliberate — every layer above it can take the edge without dragging
# anything else along, which is the same property that makes `git` usable
# from both `engine_identity` and two `cmd_*` binaries.

#
# `planar.process.identity` (plan 1080, task 6998) is a second interface
# unit in the SAME target: process start time, host identity, process and
# group existence, group signalling, and the sleep-excluding clock the host
# queue engine compares with. It is a liveness vocabulary, not a spawn one,
# so it is a separate module name — but it depends on nothing either
# (`proc_pidinfo` / `sysctl` on macOS, `/proc` reads on Linux, `kill`,
# `clock_gettime`), so it shares the target the same way `src/lib/db/`
# holds `db`, `migrate` and `agentdb`.

#
# `planar.process.runner` (plan 1080, task 7006) is a third interface unit
# in the same target: the command runner the host queue starts a queued
# command with. It resolves a program telling missing from not executable,
# starts a child in a new process group with one added environment
# variable, polls it without blocking, reports an exit status or a
# terminating signal, and signals its group through
# `planar.process.identity`. `run_inherited` is deliberately left as it is:
# it folds signal death into exit 1, and its callers depend on that.

planar_module(process
  INTERFACE
    process.cppm
    identity.cppm
    runner.cppm
  SOURCES
    process.cpp
    identity.cpp
    runner.cpp)
