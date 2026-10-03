# Architecture decision records

The table below indexes every accepted architecture decision record.
This page gives a reading order for the ADRs plus a one-paragraph
summary of each decision.

| ADR  | Title                                                           | Status                  |
|------|-----------------------------------------------------------------|-------------------------|
| 0001 | Plugin extensibility as a future concern                        | Active                  |
| 0002 | sqlx-cli for migrations                                         | Superseded by ADR-0009  |
| 0003 | Scope reset — removing retrieval substrate                      | Active                  |
| 0004 | Rust as runtime                                                 | Superseded by ADR-0005  |
| 0005 | Go as runtime                                                   | Superseded by ADR-0009  |
| 0006 | GitHub operational-plane feature mapping                        | Active                  |
| 0007 | Internal package bucket grouping                                | Active (Go → Zig → C++26) |
| 0008 | `cmd/planar` subpackage buckets                                 | Active (Go → Zig → C++26) |
| 0009 | Zig as runtime                                                  | Superseded by plan 996  |

## ADR-0001 — Plugin extensibility as a future concern

*Active, 2026-05-09.* Planar is already plugin-shaped: the schema is
the contract for read-side binaries; the five-method adapter
interface is the contract for operational systems; vendor surfaces
are file-drop pluggable; agent role specs are vendor-neutral
Markdown. Rather than formalise "plugins" as a published manifest
format, registry, or signed loader, the project preserves these
extension points as design constraints during M1–M6 and defers
formalisation until after M6. No central registry, no signing, no
sandboxing — these are explicit non-goals.

## ADR-0002 — sqlx-cli for migrations *(superseded)*

*Superseded in full by ADR-0009, latest revision 2026-05-28.* The original decision adopted Rust's `sqlx-cli` as the migration tool, with `sqlx::query!` and `sqlx::migrate!()` as the runtime contract. That decision lapsed when Go replaced Rust (ADR-0005) and again when the Zig rewrite landed (ADR-0009). The sqlx-cli file format (`-r` reversible pairs, no in-file BEGIN/COMMIT, PRAGMAs handled per-connection) survived all three runtimes and is now the durable authoring contract — `sqlx migrate add -r <name> --source migrations` is still the way to scaffold a migration, but the runtime migrator is Planar's own (today `cmake/generate_migrations.cmake` + `src/lib/db/migrate.cpp`), not the sqlx-cli tool itself. The ADR remains in the index as historical record.

## ADR-0003 — Scope reset: removing the retrieval substrate

*Active, 2026-05-10.* The first cut of `0001_initial.sql` carried 36
tables, including a retrieval/RAG substrate inherited from a prior
project (`fragments`, `fragment_metadata`, coverage, routes,
invalidation events, per-artifact feedback). Planar's product
surface — local-first task tracker, project/association scopes,
operational-plane sync, cross-vendor handoff — does not need it.
The ADR removes 14 retrieval tables and locks the schema at 22
tables (21 application + `schema_migrations`).

## ADR-0004 — Rust as runtime *(superseded)*

*Superseded in full by ADR-0005, 2026-05-11.* Adopted Rust as the
runtime in place of the founding spec's C11 recommendation, on the
grounds that the post-scope-reset workload (SQLite, HTTP, JSON,
Markdown) played to Rust's ecosystem (`sqlx`, `reqwest`, `serde`,
`clap`, `tokio`). The Rust implementation reached M5b cleanly (355
tests, clippy-clean, wiremock-backed adapter tests). The decision
was later overturned after hands-on adapter review surfaced an
ergonomics-vs-workload mismatch this assessment did not anticipate.
The implementation lives at commit `3923743`; the retrospective is
not part of this repository.

## ADR-0005 — Go as runtime *(superseded)*

*Superseded in full by ADR-0009.* For a CLI of this shape — local-first SQLite + HTTP + JSON + Markdown, a vendor surface that lives in Markdown rather than the compiler — Rust's compile-time-checking benefits were real but small, while the cognitive load (`async fn` + `Pin<Box<dyn Future>>` + lifetimes + `async_trait` + `sqlx::query!`'s offline-metadata story) was large. Go was adopted as the runtime end to end: binary, test suite, build, install path. `schema_migrations` was reinstated as the public schema-version tracker. Pure-Go SQLite via `modernc.org/sqlite` avoided cgo. The Go implementation reached M1–M19 feature-complete (608+ unit tests, 64+ integration tests, two operational-plane adapters, full propagation + workbench + spec-loop) and was archived at github.com:rdrsss/planar-go-archive.git. The decision was overturned in favor of Zig (ADR-0009) for reasons documented there; the bucket layouts from ADR-0007 and ADR-0008 carried forward into the Zig tree mechanically.

## ADR-0006 — GitHub operational-plane feature mapping

*Active, 2026-05-12; the multi-repo arm below is cut by decision 1001
(2026-09-03) — see that decision for rationale. the single-repo parent-issue,
zero-repo, and tracking-issue arms survive in the C++ rewrite.* Jira maps cleanly: feature
→ epic, child plans → stories, tasks → sub-tasks. GitHub Issues has no epic
concept and is repo-scoped, so the adapter chooses a strategy per feature at
first propagation time based on the number of distinct repos the
feature's tasks touch: single-repo features use parent-issue +
sub-issues; cross-repo features use a GitHub Projects v2 project as
the anchor; degraded modes use a tracking issue with a Markdown
task list. The strategy decision is recorded in `external_links`
and never silently changes thereafter.

## ADR-0007 — Internal package bucket grouping

*Historical (Go era), 2026-05-16.* After M7, `src/internal/`
contained 32 flat entries with no grouping signal. The ADR introduces
five bucket directories — `identity/`, `planning/`, `external/`,
`runtime/`, `health/` — each holding the original per-entity packages
as sub-directories. Package names and exported symbols are unchanged;
only import paths shift from `internal/X` to `internal/<bucket>/X`.
Migration is mechanical and reviewable one bucket at a time. The
internal-package navigability problem this fixed has a counterpart
in `cmd/planar/`, addressed by ADR-0008. *Carried forward into the
Zig rewrite, and from there into the C++26 tree, as the bucket layout
under `src/engine/`.*

## ADR-0008 — `cmd/planar` subpackage buckets

*Historical (Go era), 2026-05-18.* Applies the bucket-as-subpackage
pattern from ADR-0007 to `cmd/planar/`. The flat top-level directory
(26 Go files in `package main`) becomes seven sub-packages under
`cmd/planar/internal/`: `identity/`, `planning/`, `external/`,
`runtime/`, `workbench/`, `system/`, plus shared helpers in
`cli/`. Each bucket exports a single `AddCommands(*cobra.Command)`
constructor; `main.go` calls one constructor per bucket. Go's
`internal/` path restriction prevents accidental import by any
other binary. *The Zig rewrite preserved the per-bucket grouping
under `src/cmd/planar/handlers/` and `src/engine/`, replacing the
cobra-specific `AddCommands` constructor with hand-rolled `cli.Cmd`
registration; the C++26 tree keeps one directory per command family
under `src/cmd/<binary>/handlers/<family>/`, assembled in each binary's
`main.cppm`.*

## ADR-0009 — Zig as runtime

*Superseded by plan 996 (the C++26 rewrite), 2026-09. Supersedes
ADR-0005 in full; supersedes ADR-0002's runtime choice of sqlx-cli
library mode.*

> **SUPERSEDED.** Zig was the runtime from M1–M19 of the Go→Zig port
> through the C++26 port's M9 parity gate. Planar is C++26 now: the
> CMake tree at the repo root is the only build, and the Zig tree under
> `zig/` — kept buildable through the port as its parity ORACLE, never
> as a shipped toolchain — was deleted at the M10 cutover (task 6045,
> decisions 963/982) once its state-differential evidence came back
> clean. Everything below is the historical record of why Zig was
> chosen and what it delivered; none of its file paths, build commands,
> or test counts describe the current tree. The C++26 decision itself
> is recorded as plan 996's decision set (D1–D18) rather than as a
> numbered ADR here. The Go implementation (ADR-0005)
reached M1–M19 feature-complete and exposed two structural costs that
did not surface at adoption time: (1) cgo-free SQLite via
`modernc.org/sqlite` paid a measurable per-query overhead and made
cross-platform single-binary distribution awkward as the surface area
grew, and (2) the Go runtime's GC + scheduler + reflection were
overhead for what is fundamentally a synchronous local-first CLI with a
small fixed concurrency surface (the `--follow` wake loop and
parallel sync). Zig is adopted as the runtime end to end: binary, test
suite, build, install path. The decision is load-bearing on three
properties.

**Vendored SQLite, no external C dependency.** The official SQLite
amalgamation (`sqlite3.c` + `sqlite3.h`) is vendored under
`vendor/sqlite/` and compiled by `build.zig` as a static library with
`SQLITE_THREADSAFE=1`, `SQLITE_ENABLE_FTS5`, `SQLITE_ENABLE_JSON1`,
`SQLITE_DQS=0`, `SQLITE_DEFAULT_FOREIGN_KEYS=1`, `SQLITE_USE_URI=1`.
No system SQLite. No `cgo`. No wrapper crate. Cross-compilation works
out of the box because Zig itself compiles the C source — this is the
distribution property that drove the rewrite.

**Build-time codegen replaces runtime embedding.** Migrations live as
plain SQL files under `migrations/` (sqlx-cli `-r` reversible format,
preserved from ADR-0002). `tools/gen_migrations.zig` scans the
directory at build time and emits a `migrations` Zig module exposing
`pub const all: []const Migration`. The runtime `src/db/migrate.zig`
applies them on startup. Propagation templates under
`templates/defaults/` go through a parallel codegen pass via
`tools/gen_templates.zig`. There is no `embed.FS`, no goose library,
no on-disk migration discovery at runtime.

**Purpose-built CLI parser, extracted upstream.** Argv parsing, help
rendering, shell completion, and validation live in
[etcli-zig](https://github.com/rdrsss/etcli-zig) (~1500 LOC, no
third-party dependency beyond Zig stdlib). Originally hand-rolled
in-tree under `src/cli/`; extracted into a standalone repo and
vendored back under `vendor/etcli-zig/` so other Zig CLI projects can
reuse it. The deliberate choice over cobra (Go) or clap (Rust)
stands: the parser surface is small, stable, and integrates with
the four-binary capability boundary (each binary's verb set is
its capability surface, enforced at compile time).

**Bucket layouts carry forward.** ADR-0007 (engine bucket grouping)
and ADR-0008 (`cmd/planar` subpackage buckets) survive the runtime
swap mechanically. The Go path `src/internal/<bucket>/<entity>/` maps
to the Zig path `src/engine/<bucket>/<entity>.zig`; `cmd/planar/internal/`
maps to `src/cmd/<binary>/handlers/`. The Go-era `AddCommands`
constructor is replaced by hand-rolled `cli.Cmd` registration in
each binary's `main.zig`.

**Operational properties.** 1,700+ unit tests via `zig build test`, 570+
integration tests via `zig build test-integration` (which exec the
compiled `./bin/planar` through `integration_tests/harness.zig`), plus
a `make parity-check` gate against the archived Go reference binary (since RETIRED — see docs/architecture.md; the reference froze at schema 00030 and the port moved past it)
covering Bucket-3 (intentional Zig divergence) and Bucket-4 (cosmetic)
rows per the plan-351 parity-triage taxonomy. The archived Go
implementation remains at github.com:rdrsss/planar-go-archive.git as
historical reference.
