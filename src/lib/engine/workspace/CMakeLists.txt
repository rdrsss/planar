# planar_engine_workspace — engine/workspace bucket (plan 996, task 6110).
#
# Layer-2 `engine_*` module (cmake/architecture.cmake).
#
# DEPENDS db + json_text (both layer 1). No sideways `engine_*` edge: the
# workspace leaves read `associations` / `projects` directly rather than
# reaching into `engine_identity`, the same call `engine_grouping` already makes
# to read `closures` without depending on `engine_closure`.
#
# Behavior-preserving port (D2) of zig/src/engine/identity/workspace.zig and
# zig/src/cmd/planar/handlers/workspace/doctor.zig -- ONE of the five
# `workspace` schema leaves (`workspace doctor`), plus the org-selection and
# state-directory layout every other leaf is built on.
#
# ---------------------------------------------------------------------------
# HOME SAFETY, and a correction to the obvious assumption.
#
# `identity::planar_home` takes an explicit env-lookup callable rather than
# reading `std::getenv`, so this module cannot find a real `~/.planar` on its
# own -- the same structural mechanism `engine_workflows` and `engine_local`
# use.
#
# But for THIS bucket that is not sufficient, and the difference was established
# by probing rather than assumed: `workspace doctor` writes to the association's
# recorded `config_json.root_path`, NOT to the current working directory. Run
# from an unrelated directory, doctor still installed `AGENTS.md` / `CLAUDE.md`
# into the root recorded at `workspace init` time. Isolating the cwd protects
# nothing; the destination comes out of the DATABASE, so the protection is a
# scratch `PLANAR_DB`. Every test here builds its own scratch DB and points
# `root_path` inside its own scratch tree.
#
# Doctor is also NOT read-only despite its name: asking it to diagnose makes it
# create the state directory and reinstall root symlinks. There is no
# `--dry-run`.
# ---------------------------------------------------------------------------
#
# NOT ported, named explicitly rather than silently dropped:
#
#   - `workspace routing build`. It is the routing-table BUILDER: capability
#     rule matching over a hand-rolled glob matcher, a `go.mod` / `package.json`
#     cross-repo dependency walk, entry-point discovery, a recursive language
#     census, README first-paragraph extraction, and an operator-override merge
#     -- 1410 lines in zig/src/engine/workspace/routing.zig. It is NOT
#     architecturally blocked (SQLite + filesystem only; `--enrich`, the one
#     arm that would have shelled git, is an unimplemented no-op whose
#     `enrich_enabled` / `enrich_misses` fields are hardcoded `false` / `0`).
#     Deferred on SIZE, with its own dependency surface intact.
#
#     `routing.cppm` DOES ship the routing-table data model and its decoder,
#     because `routing show` needs them; the builder's own surface (the glob
#     matcher, the dependency walk, the language census, the capability-rule
#     TOML loader, the overrides merge, and the JSON WRITE path) is what
#     remains. See routing.cppm's header for the `languages` key-order trap
#     any builder port will hit.
#
#   - ~~`workspace routing show`.~~ PORTED (task 6110, this cycle), against
#     the previous cycle's note that it should be deferred WITH `routing
#     build`. That note reasoned the text arm "renders every field of the
#     routing table, so the whole data model has to exist first" -- true, but
#     the model is produced by DECODING routing-table.json, not by building
#     one. `show` never calls the builder: it reads the file off disk and
#     decodes it. Decoder plus renderer is a COMPLETE leaf, so this is one
#     whole leaf whose sibling stays deferred rather than half of one. The
#     honest consequence: until `routing build` lands, this binary can only
#     show a table some other binary wrote, so `build` then `show` is not yet
#     a round trip here. Full argument in routing.cppm's header, including the
#     two decode failures that carry two DIFFERENT exit codes (`SyntaxError`
#     exit 1, `InvalidInput` exit 2) behind one message.
#
#   - `workspace regenerate`. It renders AGENTS.md through a hand-rolled
#     template engine (`{{range}}` / `{{if}}` / `{{else}}` / `{{end}}` /
#     `commaJoin`) whose `-` whitespace-trim markers are PARSED AND IGNORED --
#     `parseDirective` trims `" \t\r\n-"` off the directive NAME, so `{{if
#     .Projects -}}` leaves its blank lines in the output. It then writes a
#     `.manifest-docs` merkle over the state dir keyed by xxh64, and no xxhash
#     implementation is vendored in this tree. Deferred WITH both dependencies.
#
#   - `workspace init`. Architecturally blocked, not merely deferred: it
#     composes scan + project registration + routing build + regenerate +
#     symlink install, which decision 947 places at layer 3, and no `cmd_*`
#     layer exists yet.
#
#   - ~~`policy.audit.record` calls.~~ NOT AN OMISSION, and never was
#     (task 6100). The layer-1 `planar.policy` module now exists, and this
#     bucket was audited against it: the Zig originals behind the `workspace` leaves contain
#     ZERO `policy.audit.record` call sites, so there is nothing here to
#     wire. The paragraph this replaces claimed a gap it inherited by
#     copy-paste from a bucket that has one. Verified with
#     `grep -rln policy.audit.record zig/src/engine`, whose whole output is
#     five buckets: planning, identity, entitylink, promotion, runtime.
#
# `workspace.cppm` is the umbrella re-export.

planar_module(engine_workspace
  INTERFACE
    identity.cppm
    doctor.cppm
    routing.cppm
    workspace.cppm
  SOURCES
    identity.cpp
    doctor.cpp
    routing.cpp
  DEPENDS
    db
    json_dom
    json_text)

# Glaze (D8) parses `associations.config_json`, which is operator-written JSON
# of no fixed shape. PRIVATE: the exported interface speaks only in this
# bucket's own types. The JSON this bucket EMITS is hand-rolled, because the
# target is not "valid JSON" but the exact bytes zig's std.json.Stringify
# produces.
target_link_libraries(planar_engine_workspace PRIVATE glaze::glaze)

if(TARGET planar_engine_workspace_tests)
  # The test binary opens real on-disk scratch databases through
  # planar.db.migrate, exactly as engine/models' and engine/runs' suites do.
  target_link_libraries(planar_engine_workspace_tests PRIVATE SQLite::SQLite3 glaze::glaze)
endif()
