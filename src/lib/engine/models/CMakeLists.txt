# planar_engine_models — engine/models bucket (plan 996, task 6096).
#
# Layer-2 `engine_*` module (cmake/architecture.cmake): may DEPEND on layer-1
# modules only, never sideways onto another `engine_*` bucket (D15/D18's
# same-layer prohibition for layers 2 and 3). This module needs only `db`, so
# no D19 extraction was required and none was invented.
#
# Behavior-preserving port (D2) of the registry, ranking, and views halves of
# zig/src/engine/routing/{store,ranking,views}.zig plus the rendering half of
# zig/src/cmd/planar/handlers/models.zig -- THIRTEEN of the fourteen `models`
# schema leaves:
#
#   models evals (cohort branch), models experiments, models outcomes,
#   models registry {list,add,update,remove,bind,unbind,observe,eligibility,
#                    verify-identity,export}
#
# NOT ported, named explicitly rather than silently dropped:
#
#   - `models resolve`, the fourteenth leaf. It rests on
#     zig/src/engine/routing/{roles,profile,packet}.zig (325 + 726 + 1674
#     lines): role classification, the profile rule compiler, and live
#     task/planning packet assembly that reads plans, tasks, questions,
#     decisions, and routing facts. That is a self-contained subsystem three
#     times this bucket's size, and porting it inside this task would have
#     replaced depth on the Wilson-bound acceptance criterion with breadth.
#     Deferred WITH its dependency -- it is NOT architecturally blocked and
#     needs no cmd layer. Its refusal surface is captured for the cycle that
#     takes it: `models resolve --role coder` (no `--task`) exits 2 with
#     `error: --task is required for task-bound role 'coder'`, and
#     `--role nosuch` exits 2 with `error: unknown role 'nosuch'`.
#
#   - The LEGACY (no-cohort-flags) branch of `models evals`. Supplying no
#     cohort flags falls back to a note-convention scorecard mined out of
#     `session_entries` / `agent_work_claims` / `agent_actions` -- a
#     pre-routing-evidence-plane surface the oracle itself labels "not
#     evidence-backed". Its EMPTY-input shape is pinned in render.t.cpp so
#     the cycle that ports it starts from a captured baseline:
#     `{"scorecard":[],"recommendations":[],"signals_sourced":
#     {"reviewer_disposition":true,"iteration_count":true,
#     "quality_gate_pass_fail":false,"test_coder_expansion":true},
#     "legacy_dispatch_notes_skipped":0}`.
#
#   - `policy.audit.record` calls. There is still no `policy.audit` module in
#     the C++ tree; `engine_planning/CMakeLists.txt`,
#     `engine_identity/association.cppm`, `engine_promotion`, `engine_runtime`
#     and `engine_runs` all document the same omission. No `models` leaf reads
#     or emits audit rows, so no observable CLI contract is affected.
#
# `models.cppm` is the umbrella re-export, matching
# `lib/engine/runs/runs.cppm`.

planar_module(engine_models
  INTERFACE
    registry.cppm
    ranking.cppm
    views.cppm
    render.cppm
    models.cppm
  SOURCES
    registry.cpp
    ranking.cpp
    views.cpp
    render.cpp
  DEPENDS
    db)

# Floating-point contraction OFF, for the module and its tests alike.
#
# `ranking.cpp`'s Wilson-bound expression `p * (1 - p) + z2 / (4 * n)` is the
# multiply-add shape clang fuses into a single `fma` under its DEFAULT
# `-ffp-contract=on`. The fused form skips an intermediate rounding and moves
# `wilson_lower_bound(12, 20)` by ONE ULP -- 0.38658150076225312 fused versus
# the oracle's 0.38658150076225317. Because `render.cpp` prints the shortest
# round-trippable form of that double, the last digit reaches the emitted JSON
# and the byte diff against the Zig binary fails. This was caught by
# ranking.t.cpp's exact-equality fixture, not predicted.
#
# Scoped to the two SOURCE FILES that carry the arithmetic, via
# set_source_files_properties() rather than target_compile_options().
#
# That distinction is not stylistic and was found the hard way: CMake groups
# targets by their compile settings when it synthesizes the `import std;`
# module build (CXX_MODULE_STD), and adding ANY target-level compile option
# here split that grouping such that the synthesized std-module target
# inherited planar_module()'s `-Werror` -- which libc++'s own std.cppm does
# not survive (`error: 'std' is a reserved name for a module
# [-Werror,-Wreserved-module-identifier]`). That is exactly the failure the
# top-level CMakeLists.txt's PLANAR_WARNINGS_AS_ERRORS comment warns about
# ("a global -Werror would make every configure fail on those"). A
# source-level property does not perturb the target's settings group, so the
# std-module build is untouched.
#
# Dropping this fails `models.ranking: the wilson bound reproduces the
# oracle's exact doubles` by name -- the flag is guarded by a standing test,
# not by this comment.
set(_planar_models_fp_strict
    $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>:-ffp-contract=off>)
set_source_files_properties(ranking.cpp PROPERTIES COMPILE_OPTIONS "${_planar_models_fp_strict}")
# The test file too: it evaluates the transcribed oracle constants and must do
# so under the same rounding rules as the code it compares against.
set_source_files_properties(ranking.t.cpp PROPERTIES COMPILE_OPTIONS "${_planar_models_fp_strict}")

if(TARGET planar_engine_models_tests)
  # The test binary opens real on-disk scratch databases through
  # planar.db.migrate, exactly as engine/runs' and engine/runtime's suites do.
  target_link_libraries(planar_engine_models_tests PRIVATE SQLite::SQLite3)
endif()
