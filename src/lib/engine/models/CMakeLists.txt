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
# All thirteen are WIRED as of plan 996 task 6149 (src/cmd/planar/handlers/
# models.cpp). Wiring them surfaced TWO defects in this bucket that no
# engine test could reach, because until then no caller could produce the
# inputs:
#
#   - `render.cpp`'s `json_number` emitted a BARE `nan` where the oracle
#     emits the QUOTED string `"nan"` (while both emit `inf` bare -- an
#     asymmetry nobody would guess). Reachable through
#     `models evals --quality-floor nan`, which Zig's `parseFloat` accepts
#     and the renderer echoes back in the `gates` object.
#   - `verify-identity` needs the registration ALONE and declares no
#     `--host`, but the only reader was `get_for_host`, whose
#     `valid_opaque_value` check REFUSES an empty host. `get_registration`
#     was added for it; without that, every verify-identity against a real
#     candidate reported `candidate <id> not found` at exit 1.
#
# NOT ported, named explicitly rather than silently dropped:
#
#   - `models resolve`, the fourteenth leaf. STILL NOT WIRED, but its two
#     PURE halves landed here in plan 996 task 6111 -- see profile.cppm and
#     roles.cppm, and the paragraph below for what is left.
#
#     The leaf rests on zig/src/engine/routing/{roles,profile,packet}.zig.
#     The three file sizes (325 + 726 + 1674) OVERSTATE the remaining work in
#     two ways, both measured rather than assumed:
#
#       * Those totals include colocated Zig `test` blocks, which a C++ port
#         replaces with its own fixtures rather than transcribing. roles.zig
#         is 202 implementation + 123 test; profile.zig is 507 + 219.
#       * Only the PLANNING half of packet.zig is in play at all; the TASK
#         half already landed as engine/ingest/packet (task 6324), and
#         `assemble_task` is what the task-bound branch of the handler calls.
#
#     Landed here: `profile` (the rule compiler) and `roles` (the resolution
#     table). Both are PURE -- no database, no packet dependency -- because
#     each names its own input view rather than consuming `engine_ingest`'s
#     `evidence`/`task_packet`. See profile.cppm's header for why that seam
#     was chosen over a D19 taxonomy extraction: `tier`, `work_type`, and
#     `complexity` already live in THIS bucket, so hosting the classifier
#     here removes the cross-bucket edge outright instead of relocating it.
#
#     Left for the cycle that finishes the leaf:
#       * `assemble_planning` + `compile_planning` (~300 implementation lines,
#         packet.zig:581-880). It reads the DB and shares the task half's
#         digest/canonical/`less_evidence` helpers, so it belongs in
#         engine/ingest beside them, NOT here.
#       * That module CANNOT call `engine_planning`'s `test_spec_status::
#         compute` (already ported, and exactly the summary it needs) --
#         layer-2-to-layer-2, which FATALs. Duplicate the three counting
#         queries the way engine_grouping duplicates its `closures` read;
#         only `total_tasks`, `tasks_covered`, and `total_scenarios` are
#         consumed.
#       * The cmd layer (`src/cmd/planar/handlers/models.cpp`), which is
#         where the packet and the classifier meet and where the adapter
#         from `packet::evidence` to `profile::fact` belongs.
#
#     Oracle-captured refusal surface, re-derived by RUNNING the parity
#     oracle against a scratch DB for this task (all four HOME variables
#     redirected), NOT copied from the prior row:
#
#       models resolve                      exit 2, `error: required flag
#                                           missing: --role` on STDOUT and
#                                           `error: MissingRequired` on STDERR
#       models resolve --role coder         exit 2, `error: --task is required
#                                           for task-bound role 'coder'`
#       models resolve --role nosuch        exit 2, `error: unknown role
#                                           'nosuch'`
#       --fallback-tier huge                exit 2, `error: invalid
#                                           --fallback-tier 'huge'`
#       --task abc / --plan abc             exit 2, `error: invalid --task
#                                           'abc'` / `... --plan 'abc'`
#       --role coder --task 999999          exit 1, `error: no task with id
#                                           999999`
#       --role planner --plan 999999        exit 1, `error: assembling
#                                           planning packet: PlanNotFound`
#
#     TWO oracle behaviors the finishing cycle must NOT normalize away:
#
#       * `--role spec_reviewer` (UNDERSCORED) is ACCEPTED, exit 0, and
#         reports `"role":"spec_reviewer"`. `--help` documents only the
#         hyphenated `spec-reviewer`. The hyphen-to-underscore map is a
#         no-op on an already-underscored name, so the second spelling is an
#         undocumented alias. roles.cppm reproduces it.
#       * `models resolve --json` renders complexity with `@tagName`, which
#         emits the UNDERSCORED `high_risk` -- NOT the hyphenated `high-risk`
#         that `ranking::complexity_to_text` returns and that every SQL CHECK
#         requires. The handler must not reuse `complexity_to_text` here.
#
#   (The LEGACY no-cohort-flags branch of `models evals` WAS deferred here and
#   has since landed -- see legacy.cppm, plan 996 task 6111. It reads the
#   `dispatch_shape` / `model_choice` note convention out of `session_entries`
#   and joins it against `agent_work_claims` and `agent_actions`. It needed no
#   new DEPENDS beyond `db` and the layer-1 `json_text`; the config read the Zig
#   handler performs on that path is NOT reproduced here -- see legacy.cppm's
#   "A config read that feeds nothing" note and engine/models/legacy.cpp.)
#
#   - ~~`policy.audit.record` calls.~~ NOT AN OMISSION, and never was
#     (task 6100). The layer-1 `planar.policy` module now exists, and this
#     bucket was audited against it: the Zig originals behind the `models` leaves contain
#     ZERO `policy.audit.record` call sites, so there is nothing here to
#     wire. The paragraph this replaces claimed a gap it inherited by
#     copy-paste from a bucket that has one. Verified with
#     `grep -rln policy.audit.record zig/src/engine`, whose whole output is
#     five buckets: planning, identity, entitylink, promotion, runtime.
#
# `models.cppm` is the umbrella re-export, matching
# `lib/engine/runs/runs.cppm`.
#
# STDOUT TERMINATOR CONTRACT (task 6114, resolving the M4 boundary review's
# F1). A renderer in this tree that returns a COMPLETE stdout payload returns
# the oracle's exact bytes INCLUDING the trailing newline; the layer-3 caller
# writes the string verbatim and appends nothing. A renderer that returns a
# FRAGMENT the caller composes into a larger line -- an error-message body
# that becomes `error: <body>` on stderr -- returns no terminator and its
# @return says so. The alternative shape ("renderer returns the body, caller
# appends the terminator") cannot express two payloads this tree actually has:
# a leaf whose empty output is ZERO BYTES rather than a bare newline
# (`workflow list --json`, `local list --json`), and NDJSON, where the newline
# is per-RECORD rather than per-document. Both are pinned. See
# `json_text/CMakeLists.txt` for the escaping half of the same contract.

planar_module(engine_models
  INTERFACE
    registry.cppm
    ranking.cppm
    views.cppm
    render.cppm
    legacy.cppm
    profile.cppm
    roles.cppm
    models.cppm
  SOURCES
    registry.cpp
    ranking.cpp
    views.cpp
    render.cpp
    legacy.cpp
    profile.cpp
    roles.cpp
  DEPENDS
    db
    json_text)

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

# Glaze (D8) parses the legacy branch's `model_choice:` value, which is
# arbitrary operator-authored JSON rather than a fixed struct (it is a map keyed
# by task id). PRIVATE: the exported interface speaks only in this bucket's own
# types. The JSON this bucket EMITS is still hand-rolled, because the target is
# not "valid JSON" but the exact bytes zig's std.json.Stringify produces.
target_link_libraries(planar_engine_models PRIVATE glaze::glaze)

if(TARGET planar_engine_models_tests)
  # The test binary opens real on-disk scratch databases through
  # planar.db.migrate, exactly as engine/runs' and engine/runtime's suites do.
  target_link_libraries(planar_engine_models_tests PRIVATE SQLite::SQLite3 glaze::glaze)
endif()
