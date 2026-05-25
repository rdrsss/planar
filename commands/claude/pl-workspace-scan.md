---
description: Scan a workspace, refresh its routing table and AGENTS.md, optionally with LLM-enriched summaries.
argument-hint: [--enrich] [--workspace <slug>] [--dry-run]
source: docs/cli-reference.md#domain-workspace
---

# Planar Workspace Scan (Claude)

Orchestrates the scan → routing-build → optional LLM enrichment → AGENTS.md regenerate pipeline for a workspace.

## What It Does

Refreshes a workspace's static routing table and regenerates the canonical `AGENTS.md` from it. With `--enrich`, the skill invokes the LLM per project (vendor primitives) to produce richer one-line summaries and capability tags, writes the results to the workspace-enrichment cache, and re-builds the routing table so the cached results are merged in.

## CLI Commands

Wraps [`workspace`](../../docs/cli-reference.md#domain-workspace):

> **Scope.** `workspace routing build` and `workspace regenerate` are write verbs. The resolver order is: `--workspace <slug>` flag on the skill (translated to the positional `<workspace>` argument on the CLI), then the current working directory's org association. Run this skill from inside the workspace root or pass `--workspace <slug>` explicitly. The skill refuses if no workspace can be resolved. The active scope stack was removed in plan 153 M5; there is no `scope use` to push.

```
planar workspace routing build [<workspace>] [--enrich]
planar workspace routing show [<workspace>] [--json]
planar workspace regenerate [<workspace>]
planar workspace doctor
```

## When To Invoke

Run this skill when the workspace shape has changed — a new project added, a README rewritten, dependencies shifted, or after `planar workspace init` lays down the state directory. Routine use is occasional, not per-session; both the static and `--enrich` passes are deliberate, somewhat-expensive operations. Use `--dry-run` to preview what would change without writing.

## How the Skill Composes

1. Resolve workspace. If `--workspace` was passed, parse it (`org:<slug>` or numeric id) and pass the slug as the positional argument to the CLI verbs below. Otherwise call `planar scope show --json` and look for a `kind=org` (or `kind_label=org`) entry in the cwd-derived resolved scope. Fail with a clear message if no workspace can be resolved.

2. Run `planar workspace routing build [<workspace>]` to refresh the static routing table. If `--dry-run` was passed, run with `--json` and inspect the output without writing follow-ups.

3. If `--enrich` was passed:
   a. Read the routing table from `~/.planar/workspaces/<org_id>/routing-table.json` to get the per-project metadata (slug, root_path, summary, summary_source, fingerprint inputs).
   b. For each project where `summary_source != "manual"` AND no cached enrichment exists for the current fingerprint, run the LLM enrichment loop (see "LLM Enrichment Contract" below). Skip projects whose README is missing or whose static summary already looks human-authored.
   c. Re-run `planar workspace routing build --enrich [<workspace>]` so the Go builder merges the freshly-cached results.

4. Run `planar workspace regenerate [<workspace>]` to rebuild `AGENTS.md` from the updated routing table. The symlinks (`AGENTS.md`, `CLAUDE.md` at the workspace root) continue to point at the canonical state-dir target — no symlink work needed here.

5. Print a one-line summary describing what changed.

> **Writability guards.** `routing-table.json` and `AGENTS.md` are generated artifacts under `~/.planar/workspaces/<org_id>/`. Manual operator overrides live in a separate `routing-table-overrides.json` file in the same directory and are merged on every build. This skill does not edit either generated file directly, and it does not touch the workbench (see Rule 6 in "Authoring Conventions" below).

## LLM Enrichment Contract (when --enrich)

For each project that passes the cache-miss filter in step 3.b, the skill itself invokes the LLM at `temperature=0` with these inputs:

- Up to 2 KiB of the project's `README.md` (or `README` / `readme.md`).
- The depth-2 directory listing of the project root, file and directory names only, sorted lexicographically. Exclude `.git`, `node_modules`, `vendor`, `target`, `dist`, `build`.

The prompt asks the model to produce a single JSON object matching this schema (one paragraph per project, no extra commentary):

```json
{
  "project_slug": "repo-a",
  "summary": "Customer-facing API service",
  "capabilities": ["go-service", "grpc"],
  "depends_on": ["repo-b"],
  "fingerprint_hash": "<sha256 from Request>",
  "provenance": "claude-opus-4-7 temperature=0",
  "generated_at": "<RFC3339 UTC>",
  "template_version": 1
}
```

Rules:

- `summary` is one short sentence, no trailing period required.
- `capabilities` are lowercased, kebab-cased tags (e.g., `go-service`, `react-app`, `protobuf`).
- `depends_on` lists sibling project slugs only — never external dependencies.
- `fingerprint_hash` must equal the sha256 the Go builder computes from the same Request inputs (README first 64 KiB + the sorted depth-2 listing). The cache lookup keys on this exact hash, so a mismatch invalidates the cache entry on the next build.
- `provenance` records model id and decoding settings.
- `generated_at` is RFC3339 UTC at the moment the LLM call completed.
- `template_version` is `1` for the current schema.

Validate the LLM output as well-formed JSON matching this shape before writing. On parse/validation failure, log a warning and skip the project — never write a malformed cache file. Write valid results to `~/.planar/cache/workspace-enrichment/<org_id>/<slug>-<fingerprint>.json` atomically (temp + rename).

<!--
Note for maintainers: the fingerprint_hash field is load-bearing. The
Go routing builder recomputes the same sha256 over (README excerpt +
sorted depth-2 listing) and uses it to key the cache lookup. If the
skill writes a Result whose fingerprint_hash disagrees with the Go
side's computation, the cache file is silently ignored on the next
--enrich build (cache miss). Keep the inputs and hashing algorithm in
sync with the Go implementation in src/internal/workspace/routing/.
-->

## Output

A single line on stdout summarising the run:

```
workspace org:work scanned (3 projects, 2 enriched, 5 capabilities updated)
```

When `--dry-run` is set, the prefix is `would scan:` and no files are written.

## Authoring Conventions

This skill body adheres to the six rules established by task 602 (see
the M6 audit notes for context):

1. Quoted titles ("Title") not bare. The frontmatter `description` value is a double-quoted string.
2. Literal headings (`## What It Does`, `## When To Invoke`, `## How the Skill Composes`, `## LLM Enrichment Contract (when --enrich)`, `## Output`, `## Vendor Notes`, `## Invocation`).
3. No nested bullets. Step lists use only top-level numbered items with prose; sub-letters (a/b/c) are flat under the parent number.
4. No `## Out of this plan` H2. Deferred items belong in the tech spec's `## Out of scope` section, not in skill bodies.
5. Always double-quote `title:`-style values when this skill emits frontmatter elsewhere. This skill itself never writes workbench artifacts — it operates on the workspace state directory only.
6. Workbench discipline: this skill MUST NOT read or write the workbench filesystem. It only touches `~/.planar/workspaces/<org_id>/` (canonical state) and `~/.planar/cache/workspace-enrichment/<org_id>/` (enrichment cache). Active scope and workspace state come from the CLI; the skill must not invent direct DB writes or repo-local context scaffolding.

## Vendor Notes

- Installed to `~/.claude/commands/pl-workspace-scan.md`.
- Invoked as `/pl-workspace-scan <subcommand> [args]`.
- Active scope and workspace state come from the CLI; the skill must not read or write workspace context outside it.

## Invocation

```
/pl-workspace-scan                          # scan active workspace, static only
/pl-workspace-scan --enrich                 # static scan + LLM enrichment pass
/pl-workspace-scan --workspace org:work     # explicit workspace target
/pl-workspace-scan --dry-run                # report what would change, no writes
```
