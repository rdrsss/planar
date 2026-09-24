# In-tree Scriptorium technical specification

**Status:** implemented for Planar's current source corpus; see the tool README
and the adjacent CTest parity suite for the shipped contract.

**Location:** `src/tools/scriptorium/`

**Purpose:** remove Planar's dependency on the deprecated external Scriptorium binary while preserving its authored skill and agent projections.

## Decision and scope

Build a C++26 `scriptorium` executable with first-party C++ modules and no
first-party headers. It reads the existing `scriptorium.yaml`, `skills/src/`
and `agents/` sources, and stages the same vendor files that `install.sh`
currently consumes. Glaze handles YAML input, TOML validation, and JSON
reports; explicit TOML quoting preserves the existing Codex agent bytes.
Inja renders source bodies. Its nlohmann dependency is confined to this
tool's private template adapter; other Planar targets continue to use Glaze.

The in-tree tool implements `render`, `check`, `status` and `version`. Those
are the rendering and inspection operations Planar uses. `install.sh`
continues to wire staged projections into vendor directories and to record
them in `$PLANAR_HOME/install-manifest.json`. The old Scriptorium workbench,
`install`, `uninstall`, `update`, `sync` and `init` commands are not part of
this Planar-specific tool. Their absence must be explicit in help and the
updated documentation.

### Template engine assessment

Glaze parses source/config YAML and handles JSON reports. Inja's
private rendering context uses nlohmann, which is an intentional second
data representation within this tool. Inja's
[`INJA_DATA_TYPE` hook](https://github.com/pantor/inja/blob/main/include/inja/json.hpp)
changes a type alias, but the same header includes `nlohmann/json.hpp` and
the renderer uses nlohmann-style `get`, `get_ref`, `json_pointer`, `dump` and
type aliases. Treating Glaze as a drop-in replacement would require a large
compatibility facade.

| Option | Structured-data fit | Template compatibility and assessment |
| --- | --- | --- |
| [Inja](https://github.com/pantor/inja) | Requires `nlohmann/json` for practical use, privately within this tool. | Chosen for its full expression, branch, loop and include support. The current `{{.VendorTitle}}` actions migrate to `{{ VendorTitle }}`. |
| [Bustache](https://github.com/jamboree/bustache) | Accepts STL containers or custom model traits; can use `<format>` instead of `fmt`. No JSON library is required. | Strongest external candidate if full sections/partials become necessary: configurable escaping and unresolved-variable handling. It uses Mustache syntax, so source migration and byte-parity review remain necessary. Its GitHub repository currently lists no releases, which must be resolved against Planar's pinned release-archive vendoring rule before adoption. |
| [kainjow/Mustache](https://github.com/kainjow/Mustache) | Header-only, zero dependencies and has a tagged release. | Simpler external candidate, but defaults to Mustache escaping/missing-value semantics. It still needs a syntax migration and a strict validation wrapper to preserve current errors. |
| Small in-tree renderer | Uses plain context strings/maps; Glaze remains the only structured-data library. | Narrowest solution for current actions, but would make Planar own the template language and its future expansion. Deferred. |

Mustache's leading-dot syntax is not a Go `text/template` field lookup;
its special `.` means the current context. Thus neither Mustache library is
a drop-in renderer for `{{.VendorTitle}}`. Planar's current 59 configured
source files contain 52 template actions, all `{{.VendorTitle}}`. Migrating
those actions to Inja syntax is part of this cutover. Keep the rendered
projection baseline unchanged even as authored source syntax changes.

## Inputs and output contract

| Input or output | Contract |
| --- | --- |
| `scriptorium.yaml` | Keep `sources.skills`, `sources.agents`, `vendors` and `vendor_overrides` as authored. Source roots resolve relative to this file, not the process cwd. The four selected vendors are Claude, Codex, Copilot and Gemini. |
| Unified source | Recursively discover Markdown files under both roots in sorted order. Parse `---` YAML frontmatter and body. Kinds are `skill` (default), `agent` and `doc`. Validate required `slug` and `description`, permitted keys by kind, slug syntax `^[a-z0-9-]+$`, and global slug uniqueness. Preserve source-path diagnostics. |
| Vendor profiles | Reproduce the four current built-in profiles, then apply config overrides. Keep output directory, layout, ordered frontmatter fields, invocation block, notes, agent format and doc placement data-driven. Reject unknown selected vendors or invalid profile shapes before writing. |
| Skill projection | Emit profile-selected YAML frontmatter in its declared order, optional invocation block, shared notes and rendered Markdown body. Codex uses `<slug>/SKILL.md`; the other Planar staging profiles use flat `<slug>.md` files. |
| Agent projection | Emit YAML-frontmatter Markdown for Claude, Copilot and Gemini; emit TOML for Codex. Preserve field order, quoting, trailing newlines and the rendered body. |
| Companion doc projection | Emit body only, adjacent to agent files for file-layout vendors. Skip the Codex dir-layout pairing as the current renderer does. |
| Staging root | `render` writes under an explicit `--output-root <dir>`, defaulting to the process cwd for CLI compatibility. It never writes through an output path that escapes this root. |

The initial oracle is a scratch render of the current external Scriptorium:
**232 files** (40 Claude command files, 120 skill files and 72 agent/doc
files). This count is a corpus snapshot, not a hard-coded rule. Compare the
relative path and every byte for every file; compare the set of skipped
source/vendor pairs separately.

### Template language

The authored body language becomes Inja syntax. Convert the current
`{{.VendorTitle}}` actions to `{{ VendorTitle }}` in `skills/src/` and
`agents/` in the implementation change. Reject leftover Go-style actions
at source validation; do not silently reinterpret them. Inja receives the
documented context (`Slug`, `VendorName`, `VendorTitle`, `Invoke`,
`InstallPath`, `Description`, `Model`, `Vendor` and profile-added context
fields). A `doc` receives only `Slug` and `Description`; vendor fields in
a doc are errors. Map Inja parse/render errors to diagnostics containing
the source path and vendor. Missing variables fail the render.
Build the nlohmann context directly from validated C++ fields; do not use
it to parse config/source files or serialize CLI reports.

Disable HTML autoescaping and filesystem template search. Keep Inja's
default whitespace settings unless a byte comparison requires an explicit
setting. Includes, if later authored, must use a controlled loader rooted
under the source tree; an include may not read an arbitrary filesystem path.
Preserve the authored body's internal whitespace and Scriptorium's
single-blank-line join rule. Test missing keys, malformed expressions,
literal delimiters, whitespace and a rejected leftover Go action. The
current corpus has no control-flow actions; future Inja constructs need
focused tests when introduced.

### Structured formats

Use Glaze's YAML reader for both the repo config and source frontmatter,
its TOML writer for Codex agents, and its JSON reader/writer for reports and
any manifest data. The old implementation uses `yaml.v3`, so Glaze acceptance
of the *real corpus* is a prerequisite, not an assumption. Test quoted and
plain scalars, apostrophes, colons, inline comments, flow lists, nested
vendor maps, block strings, empty maps and duplicate/unknown keys. Keep
validation separate from parsing so a permissive parser cannot silently
admit an invalid source. Pin output bytes where Glaze's default serializer
differs from Scriptorium's emitted YAML or TOML; use explicit serialization
helpers for those projection bytes if needed, while Glaze remains the parser
and structured-data layer.

## C++ and build layout

```text
src/tools/scriptorium/
  CMakeLists.txt
  main.cpp                 # process entry
  core.cppm core.cpp       # parser, profiles, Inja rendering and inspection
  core.test.py             # adjacent process tests
  planar-golden.sha256     # complete projection byte pins
  README.md                # usage and authoring syntax
```

Use a private module and a small executable entry point, registered from
`src/tools/CMakeLists.txt`.
The tool uses Glaze for YAML and JSON. Only the Scriptorium target includes
Inja and nlohmann; no exported module
interface exposes their types. It must not link a `src/cmd/*` binary or
acquire a Planar SQLite handle. Keep third-party headers in the global
module fragment or implementation units. Add Inja and nlohmann as separate
pinned release-archive CPM entries, cached under committed `vendor/`.
Disable their upstream tests and install rules. Avoid an upstream
`find_package(nlohmann_json)` dependency by wiring the two pinned source
trees explicitly if Inja's CMake options require it. Do not enable
nlohmann on any `src/engine/`, `src/lib/` or `src/cmd/` target.

`cmake --build` must build the tool, and `cmake --install` must place it at
`$PLANAR_HOME/bin/scriptorium` beside the five Planar binaries. It is a
project tool, not a sixth planning-state binary; it owns no database write
surface.

## CLI behavior

| Command | Behavior |
| --- | --- |
| `render --config <path> [--source <dir>] [--vendor <name>]... [--output-root <dir>] [--dry-run] [--json]` | Discover, validate, render and stage selected projections. `--source` overrides configured roots. Dry run computes the same result without writing. JSON reports `written`, `skipped` and `failed` arrays. |
| `check --config <path> [--vendor <name>]... [--output-root <dir>] [--json]` | Read-only comparison of expected relative paths and bytes against staged output; report missing, changed and unexpected projections within the selected profiles' staging directories. Never traverse unrelated destination content. |
| `status --config <path> [--vendor <name>]... [--output-root <dir>] [--install-manifest <path>] [--json]` | Read-only per-source view. Preserve the `.artifacts[]` JSON keys consumed by `scripts/check-self-installed.sh` (`slug`, `kind`, `defined`, `rendered`, `installed`, `drifted`, `orphaned`); define `rendered` as all applicable staged outputs present and byte-equal to the current projection. `installed` is false unless an explicit Planar install manifest is supplied and its selected rows match; no new registry is created. |
| `version` | Print the built tool version; replace the old `render --help` identity probe. |

Accept `--config`/`-config` and `--json`/`-json` during cutover because
existing scripts use both spellings. Structural errors and invalid flags
exit 2; render or filesystem failures and `check` findings exit 1; a clean
operation exits 0. `status --json` must emit its report even when it exits 1
for drift. Sort paths, vendors and diagnostics to make output deterministic.

Unlike the old renderer's per-source partial write on a template failure,
render first validates and computes the complete selected projection set.
Validation or template failure performs no writes. Write each output through
a temporary file in its destination directory and rename it into place. An
I/O failure partway through commit may leave a subset of complete outputs;
it must never leave a truncated file, and a repeat render must repair it.
The installer may still reset owned staging directories before rendering;
that operation remains in `install.sh` and is covered by its tests.

`check` and `status` are intentionally based on actual staged bytes. The
external Scriptorium's `status.rendered` only meant that at least one vendor
could render a source, and its `check` relied on its separate install
registry. This is a deliberate semantic correction for Planar's use of
`scriptorium render`: Planar never used Scriptorium's install registry as
its installer authority. Existing callers and documentation must be updated
to pass the staging root they intend to inspect.

## Ownership and filesystem safety

The source roots and staging root are explicit inputs; tests use temporary
directories and never resolve the operator's actual vendor home. Validate
output directories and slug-derived paths before writes. Refuse absolute
profile output paths, `..` components, duplicate projected paths and
symlinked parent paths that lead outside the staging root. `check` and
`status` have no write path. Do not delete unmanaged files in these verbs.
Vendor installation remains under `install.sh` and its existing ownership
manifest; local personal skills remain under `planar local`.

## Cutover sequence and gates

1. **Capture baseline:** render the configured corpus with external
   Scriptorium into a temporary directory; record its 232 relative paths,
   bytes and expected skip set. Keep focused malformed-source fixtures from
   the local Scriptorium tests as independent negative cases.
2. **Pin dependencies and migrate syntax:** vendor release archives for
   Inja and nlohmann, keep their include paths private to the tool, and
   convert the 52 current Go-style actions to Inja syntax. Keep the source
   edit limited to template actions and document the new authoring syntax.
3. **Implement the compiler:** source/config parsing, profile merge, Inja
   rendering and all three projections. Require byte-for-byte corpus parity
   against the pre-migration baseline, plus stable diagnostics for validation
   classes, before installer changes.
4. **Implement inspection and CLI:** verify `render`, dry run, `check`,
   `status`, JSON shape, aliases, exit codes, output-root safety and atomic
   writes in process tests. Exercise missing, stale, malformed and foreign
   output cases.
5. **Wire build and installer:** make `install.sh` build and use its own
   `$PLANAR_HOME/bin/scriptorium` after CMake installation; remove the
   external discovery preflight and `scripts/discover-scriptorium.sh`.
   Update `scripts/check-self-installed.sh` and installer tests to use the
   installed tool and the intended staging root. Keep full vendor link/copy
   and install-manifest verification.
6. **Update contracts together:** revise `README.md` prerequisites and
   `install.sh` dependency manifests if external tool requirements change;
   update `docs/architecture.md`, `docs/skill-reference.md`, workflow docs
   and authored skills that mention external Scriptorium or its manifest.
   Preserve `AGENTS.md`/`CLAUDE.md` equivalence and the repo's five-binary
   capability description.

The cutover gate is a clean debug build and CTest run, byte parity for all
current projections, the installer dependency/manifest tests, a sandboxed
full install and `scripts/check-self-installed.sh` against that install.
Inspect the CMake link and include graph to confirm nlohmann is confined
to `src/tools/scriptorium/`.
Remove the external prerequisite only after those checks pass. The source
template syntax changes to Inja; the generated vendor layout and bytes do
not change.
