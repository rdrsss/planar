---
title: Architecture decision records
doc_kind: adr_index
template_version: 1
source_artifacts:
  - artifact:35
  - artifact:36
  - artifact:37
  - artifact:38
  - artifact:39
  - artifact:40
  - artifact:41
  - artifact:75
regenerated_at: 2026-05-18T00:00:00Z
regenerated_by: hand
---

# Architecture decision records

The table below indexes every accepted architecture decision record.
The full text of each ADR lives as a Planar artifact under
`assoc:project:planar`; this page is the curated reading order plus
a one-paragraph summary of each decision.

| ADR  | Title                                                           | Status                  |
|------|-----------------------------------------------------------------|-------------------------|
| 0001 | Plugin extensibility as a future concern                        | Active                  |
| 0002 | sqlx-cli for migrations                                         | Superseded by ADR-0005  |
| 0003 | Scope reset — removing retrieval substrate                      | Active                  |
| 0004 | Rust as runtime                                                 | Superseded by ADR-0005  |
| 0005 | Go as runtime                                                   | Active                  |
| 0006 | GitHub operational-plane feature mapping                        | Active                  |
| 0007 | Internal package bucket grouping                                | Active                  |
| 0008 | `cmd/planar` subpackage buckets                                 | Active                  |

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

*Superseded in full by ADR-0005, 2026-05-11.* The original decision
adopted Rust's `sqlx-cli` as the migration tool, with `sqlx::query!`
and `sqlx::migrate!()` as the runtime contract. That decision lapsed
when Go replaced Rust; migrations are now embedded via Go's
`embed.FS` and applied at runtime by a goose-library migrator. The
migration file format (no in-file BEGIN/COMMIT, PRAGMAs handled
per-connection) is preserved as a coincidental practice. The ADR
remains in the index as historical record.

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
in `docs/lessons-learned-rust-to-go.md` (Planar artifact 34).

## ADR-0005 — Go as runtime

*Active, 2026-05-11. Supersedes ADR-0004 in full.* For a CLI of this
shape — local-first SQLite + HTTP + JSON + Markdown, a vendor surface
that lives in Markdown rather than the compiler — Rust's
compile-time-checking benefits are real but small, while the
cognitive load (`async fn` + `Pin<Box<dyn Future>>` + lifetimes +
`async_trait` + `sqlx::query!`'s offline-metadata story) is large.
Go is adopted as the runtime end to end: binary, test suite, build,
install path. `schema_migrations` is reinstated as the public
schema-version tracker (sqlx's `_sqlx_migrations` is no longer
relevant). Pure-Go SQLite via `modernc.org/sqlite` avoids cgo.

## ADR-0006 — GitHub operational-plane feature mapping

*Active, 2026-05-12.* Jira maps cleanly: feature → epic, child plans
→ stories, tasks → sub-tasks. GitHub Issues has no epic concept and
is repo-scoped, so the adapter chooses a strategy per feature at
first propagation time based on the number of distinct repos the
feature's tasks touch: single-repo features use parent-issue +
sub-issues; cross-repo features use a GitHub Projects v2 project as
the anchor; degraded modes use a tracking issue with a Markdown
task list. The strategy decision is recorded in `external_links`
and never silently changes thereafter.

## ADR-0007 — Internal package bucket grouping

*Active, 2026-05-16.* After M7, `src/internal/` contained 32 flat
entries with no grouping signal. The ADR introduces five bucket
directories — `identity/`, `planning/`, `external/`, `runtime/`,
`health/` — each holding the original per-entity packages as
sub-directories. Package names and exported symbols are unchanged;
only import paths shift from `internal/X` to `internal/<bucket>/X`.
Migration is mechanical and reviewable one bucket at a time. The
internal-package navigability problem this fixed has a counterpart
in `cmd/planar/`, addressed by ADR-0008.

## ADR-0008 — `cmd/planar` subpackage buckets

*Active, 2026-05-18.* Applies the bucket-as-subpackage pattern from
ADR-0007 to `cmd/planar/`. The flat top-level directory (26 Go
files in `package main`) becomes seven sub-packages under
`cmd/planar/internal/`: `identity/`, `planning/`, `external/`,
`runtime/`, `workbench/`, `system/`, plus shared helpers in
`cli/`. Each bucket exports a single `AddCommands(*cobra.Command)`
constructor; `main.go` calls one constructor per bucket. Go's
`internal/` path restriction prevents accidental import by any
other binary.
