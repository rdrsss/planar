# planar_engine_models — engine/models bucket (plan 996, task 6096).
#
# Layer-2 `engine_*` module (cmake/architecture.cmake): may DEPEND on layer-1
# modules only, never sideways onto another `engine_*` bucket (D15/D18's
# same-layer prohibition for layers 2 and 3). This module needs only `db`, so
# no D19 extraction was required and none was invented.
#
# Behavior-preserving port (D2) of the registry, ranking, and views halves of
# zig/src/engine/routing/{store,ranking,views}.zig plus the rendering half of
# zig/src/cmd/planar/handlers/models.zig -- ALL FOURTEEN `models` schema
# leaves:
#
#   models evals (cohort branch), models experiments, models outcomes,
#   models resolve,
#   models registry {list,add,update,remove,bind,unbind,observe,eligibility,
#                    verify-identity,export}
#
# The first thirteen were WIRED as of plan 996 task 6149 (src/cmd/planar/
# handlers/models.cpp). Wiring them surfaced TWO defects in this bucket that
# no engine test could reach, because until then no caller could produce the
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
# `models resolve`, the fourteenth leaf, WIRED at plan 996 task 6343. Its two
# PURE halves -- `profile` (the rule compiler) and `roles` (the resolution
# table) -- landed HERE at plan 996 task 6111; see profile.cppm and
# roles.cppm. Both are PURE -- no database, no packet dependency -- because
# each names its own input view rather than consuming `engine_ingest`'s
# `evidence`/`task_packet`. See profile.cppm's header for why that seam was
# chosen over a D19 taxonomy extraction: `tier`, `work_type`, and
# `complexity` already live in THIS bucket, so hosting the classifier here
# removes the cross-bucket edge outright instead of relocating it.
#
# The leaf rested on zig/src/engine/routing/{roles,profile,packet}.zig. The
# three file sizes (325 + 726 + 1674) OVERSTATED the remaining work by
# including colocated Zig `test` blocks a C++ port replaces with its own
# fixtures rather than transcribing (roles.zig is 202 implementation + 123
# test; profile.zig is 507 + 219) -- but task 6111 had ALREADY landed both of
# those. What task 6343 measured on the actual remainder -- the PLANNING half
# of packet.zig, packet.zig:581-880, ~300 Zig lines including its own
# struct/enum declarations -- UNDER-stated the C++ side by a wide margin, and
# the ~300 estimate's own note explains the biggest reason why:
#
#   - `assemble_planning` + `compile_planning` landed in `engine_ingest`
#     (packet.cppm/.cpp), beside the task half's digest/canonical/
#     `less_evidence` helpers they share, NOT here -- exactly as flagged.
#   - That module CANNOT call `engine_planning`'s `test_spec_status::compute`
#     (already ported, and exactly the summary it needs) -- layer-2-to-
#     layer-2, which FATALs. The estimate correctly named this and correctly
#     pointed at the `engine_grouping`/`closures` precedent, but did not size
#     it: reproducing `total_tasks`, `tasks_covered`, and `total_scenarios`
#     by hand -- three standalone SQL queries the oracle gets for free in
#     three lines by calling a same-layer sibling -- runs to ~90 lines on its
#     own. Measured total: ~450 implementation lines in packet.cpp, not ~300.
#   - The cmd layer (`src/cmd/planar/handlers/models.cpp`) is where the
#     packet and the classifier meet, and where the adapter from
#     `packet::evidence` to `profile::fact` lives (`to_profile_facts`) --
#     exactly where this note said it would.
#
# `profile::profile`'s `cohort` field is DELIBERATELY LEFT UNPORTED, a
# decision made explicit rather than filled in with an invented value -- see
# profile.cppm's own note beside `compile`. The oracle's `Cohort`
# (zig/src/engine/routing/store.zig:644) has SEVEN fields; this tree's
# `ranking::cohort` has EIGHT (task 6096 folded in `routing_policy_version`).
# Nothing on the `models resolve` path constructs one.
#
# Oracle-captured refusal surface, re-derived by RUNNING the parity oracle
# against a scratch DB (all four HOME variables redirected), NOT copied from
# the prior row -- kept here as the CANONICAL capture even though the leaf is
# now wired, because `models_leaves.t.cpp` and `dispatch.t.cpp` pin these
# bytes and this is where a future reader finds out WHY:
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
#     TWO oracle behaviors this port must NOT normalize away, both
#     reproduced:
#
#       * `--role spec_reviewer` (UNDERSCORED) is ACCEPTED, exit 0, and
#         reports `"role":"spec_reviewer"`. `--help` documents only the
#         hyphenated `spec-reviewer`. The hyphen-to-underscore map is a
#         no-op on an already-underscored name, so the second spelling is an
#         undocumented alias. roles.cppm reproduces it.
#       * `models resolve --json` renders complexity with `@tagName`, which
#         emits the UNDERSCORED `high_risk` -- NOT the hyphenated `high-risk`
#         that `ranking::complexity_to_text` returns and that every SQL CHECK
#         requires. `handlers/models.cpp`'s `complexity_tag` reproduces the
#         underscored form and does NOT reuse `complexity_to_text`.
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
# `src/engine/runs/runs.cppm`.
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
