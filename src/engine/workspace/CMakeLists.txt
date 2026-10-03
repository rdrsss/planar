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
#   - ~~`workspace routing build`.~~ PORTED (task 6275, this cycle), as
#     `routing_build.cpp`. It was deferred on SIZE alone -- 1410 lines and no
#     architectural blocker -- and that sizing held: SQLite plus filesystem,
#     no new dependency, no spawn seam. `--enrich`, the one arm that would
#     have shelled git, is an unimplemented no-op in the oracle whose
#     `enrich_enabled` / `enrich_misses` fields are hardcoded `false` / `0`,
#     and is reproduced as exactly that, warning line included.
#
#     `build` then `show` is now a genuine round trip in this tree, which it
#     was not after task 6110.
#
#     THREE things the cycle established that were not knowable from the
#     source, each captured against the oracle in a pinned scratch arena and
#     each pinned by a test:
#
#       * The `languages` key order really is a Zig hash artifact and is the
#         port's ONE deliberate output divergence -- this builder sorts. Task
#         6274 had predicted this; the capture confirmed it across three
#         repositories in a single build (`go, markdown` / `typescript,
#         markdown, json` / `javascript, markdown, python, json`). Every
#         OTHER collection the builder emits is explicitly ordered by the
#         oracle and was checked one at a time rather than assumed to share
#         the problem.
#
#       * A capability pattern CONTAINING A SLASH never matches, because
#         `patternFinds` frees the joined subdirectory path before opening it
#         (`defer` inside the `if` block). So the default `protobuf` rule
#         fires only through its `*.proto` arm and the `proto/*.proto` arm
#         beside it is dead. Five consecutive builds, three fixtures,
#         identical every time. REPRODUCED as observed rather than repaired,
#         on the same grounds that keep `--enrich` a no-op; it is an oracle
#         defect and belongs in a task row against routing.zig.
#
#       * The capability-rules TOML has a two-arm error surface with TWO
#         EXIT CODES behind one message (`ParseFailed` 1, `InvalidInput` 2),
#         and which arm a malformed line takes is not derivable from its
#         shape -- `match_all = 1.5` is ParseFailed while `match_all = 7` is
#         InvalidInput. Captured one line at a time; the table is in
#         routing_build.cpp beside the parser.
#
#     `globExists` and `pathExistsJoin` are defined in routing.zig and called
#     from nowhere in the Zig tree. Task 6110 had already noticed them; they
#     are deliberately NOT transcribed.
#
#   - ~~`workspace routing show`.~~ PORTED (task 6110, this cycle), against
#     the previous cycle's note that it should be deferred WITH `routing
#     build`. That note reasoned the text arm "renders every field of the
#     routing table, so the whole data model has to exist first" -- true, but
#     the model is produced by DECODING routing-table.json, not by building
#     one. `show` never calls the builder: it reads the file off disk and
#     decodes it. Decoder plus renderer is a COMPLETE leaf, so this is one
#     whole leaf whose sibling stays deferred rather than half of one. The
#     honest consequence it recorded -- that until `routing build` landed this
#     binary could only show a table some other binary wrote -- was true then
#     and is no longer: task 6275 landed the sibling and closed the loop.
#     Full argument in routing.cppm's header, including the two decode
#     failures that carry two DIFFERENT exit codes (`SyntaxError` exit 1,
#     `InvalidInput` exit 2) behind one message.
#
#     Task 6275 also found ONE defect in that port and corrected it in the
#     same cycle: `routing show`'s AMBIGUOUS refusal ("multiple org
#     associations registered ...") exits 2 in the oracle and exited 1 here.
#     Both `show` and `build` die through the identical
#     `exit.die(ctx, error.InvalidInput, ...)`, and the message bytes matched
#     exactly, so nothing short of comparing EXIT CODES could have caught it
#     -- which is precisely the failure mode routing.cppm's header warns
#     about, arriving on the arm one layer up from the one it warns about.
#
#   - ~~`workspace regenerate`.~~ PORTED (task 6364), as
#     `regenerate.cppm`/`.cpp`, with a hand-rolled template engine this leaf
#     also carries.
#
#     The hand-rolled template engine's one deliberate quirk survived the
#     port unchanged: `parseDirective` trims `" \t\r\n-"` off the directive
#     NAME, so the Go-template-style `-}}` / `{{-` whitespace-trim markers
#     PARSE but trim nothing beyond the delimiters themselves -- `{{if
#     .Projects -}}` behaves exactly like `{{if .Projects}}`, and the oracle's
#     own rendered output carries the resulting blank lines. Captured
#     end-to-end against a live `workspace init` run (`regenerate.cppm`'s
#     header) and reproduced byte-for-byte, not fixed forward.
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
#
# `regenerate.cpp` imports the layer-1 `planar.queuerule` module (plan 1080,
# tasks hq-workspace-guide and 7110) so the generated guide carries the host
# queue rule. The bytes come through a real `queuerule` edge; there is no
# `engine_hostqueue` edge and no cross-directory `#embed`.

planar_module(engine_workspace
  INTERFACE
    identity.cppm
    doctor.cppm
    routing.cppm
    regenerate.cppm
    workspace.cppm
  SOURCES
    identity.cpp
    doctor.cpp
    routing.cpp
    routing_build.cpp
    regenerate.cpp
  DEPENDS
    db
    json_dom
    json_text
    queuerule
  EMBED)

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
