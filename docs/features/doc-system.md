---
title: Outward-facing documentation system
doc_kind: feature
template_version: 1
source_artifacts: [artifact:66, artifact:67, artifact:68]
source_plans: [plan:96]
regenerated_at: 2026-05-18T00:00:00Z
regenerated_by: hand
references:
  doc_product_spec:
    kind: planar
    entity: artifact:66
  doc_tech_spec:
    kind: planar
    entity: artifact:67
  doc_roadmap:
    kind: planar
    entity: artifact:68
---

# Outward-facing documentation system

Planar's documentation system turns internal planning artifacts —
tech specs, decision records, ADRs, glossary entries — into the
published prose under `docs/`. Every published doc carries
provenance that names its sources, citations that pass a structural
linter, and a content hash registered in a per-repo manifest so
source drift is visible as a deterministic four-signal
classifier[^doc_product_spec].

## What it does

- Extends `artifacts.kind` with four doc-supporting kinds: `research`,
  `getting_started`, `changelog_entry`, `glossary_term`. Schema
  change ships as migration `0008_doc_artifact_kinds.sql`.
- Establishes a provenance front-matter contract every published
  doc obeys: `doc_kind`, `template_version`, the list of source
  artifacts/decisions/plans, per-source content hashes, and
  `regenerated_at` / `regenerated_by` for audit[^doc_tech_spec].
- Registers a citation system: GFM footnote syntax (e.g. a bracketed
  caret plus id) for body citations, a structured `references:` block
  in front matter,
  and a linter that walks both halves and reports
  `undeclared_citation`, `unused_declaration`,
  `unresolvable_external`, and `unresolvable_planar` issues.
- Maintains `.manifest-docs` — an xxh3-keyed two-level Merkle index
  across `docs/`. The root hash is O(1) to verify; the per-doc
  classifier distinguishes regenerate-candidates (sources changed),
  hand-edits (body changed without source drift), new authoring
  (file appeared without a manifest entry), and deletions.
- Ships LLM-assisted authoring as two verbs: `planar doc promote`
  (synthesise a new doc from named source entities) and
  `planar doc regenerate` (re-synthesise an existing doc when its
  sources have drifted). The LLM call lives in the vendor skills;
  the Go CLI handles resolution, hashing, atomic write, and
  manifest update.
- Adds query verbs for coverage analysis: `planar doc backlinks`,
  `planar doc orphans`, `planar doc coverage`[^doc_roadmap].

## The provenance schema

Every doc under `docs/` opens with YAML front matter of this shape:

```yaml
---
title: <human-readable title>
doc_kind: <feature | getting_started | adr_index | changelog | glossary | research | agents>
template_version: 1
source_artifacts: [artifact:47, artifact:74]
source_decisions: [decision:12]
source_plans: [plan:47]
source_versions:
  artifact:47: <xxh64 hash of body at regenerate time>
  decision:12: <xxh64 hash>
regenerated_at: 2026-05-18T00:00:00Z
regenerated_by: pl-doc-promote
references:
  smith2024:
    kind: external
    title: "Foundations of WAL concurrency"
    author: "Smith, J."
    year: 2024
    url: "https://example.org/smith2024"
  scope_adr:
    kind: planar
    entity: decision:7
---
```

The `source_versions` block holds the content hash of each source at
synthesis time. When `planar doc manifest diff` runs, it re-resolves
those sources from the DB, recomputes their hashes, and flags any
that have drifted as regenerate-candidates.

## The four-signal classifier

`planar doc manifest diff` walks every entry in the stored manifest
and emits one of four signals per path:

| Signal | Doc body changed? | Sources changed? | Meaning |
|--------|-------------------|------------------|---------|
| (unchanged) | no | no | nothing to do |
| `regenerate-candidate` | maybe | yes | re-run `doc regenerate` |
| `hand-edit` | yes | no | operator changed prose directly |
| `new-authoring` | n/a (no stored entry) | n/a | file appeared on disk |
| `deletion` | n/a (file gone) | n/a | manifest entry stale |

The classifier is pure. The regenerator acts on
`regenerate-candidate` paths and refuses to overwrite hand-edits
without `--force` (discard) or `--merge` (write a
`<path>.regenerated.md` sibling for the operator to reconcile).

## CLI surface

```sh
# Citation + reference validation.
planar doc lint [--path <dir>] [--no-refs] [--refs-only]

# Manifest operations.
planar doc manifest verify        # O(1) root compare
planar doc manifest diff          # four-signal breakdown
planar doc manifest update        # rebuild and write
planar doc manifest info <path>   # one entry's hashes + sources

# Synthesis.
planar doc promote --kind <k> --source <ref> [--source <ref>...] \
                   [--slug <slug>] [--body-file <path>]
planar doc regenerate (--slug <s> | --path <p> | --all) \
                      [--body-file <path>] [--force | --merge]

# Coverage queries.
planar doc backlinks <entity-ref>
planar doc orphans --kind <artifact-kind>
planar doc coverage
```

## Where docs live vs where authoring lives

Internal planning artifacts (tech specs, roadmaps, product specs,
ADRs, design notes) live as Planar artifacts under the active scope
and surface via the workbench filesystem. Published outward-facing
docs live under `docs/` in the repo. The doc system is the
boundary: `promote` reads from the artifact plane and writes to the
docs plane; `regenerate` keeps the two in sync; `lint` and the
manifest enforce the contract.

## Related

- [Concepts: artifacts and the workbench](../concepts.md)
- [Workflows: synthesising and refreshing docs](../workflows.md)
- [Architecture: provenance and the manifest](../architecture.md)
- [Features: scope resolution](scope-resolution.md) — `doc promote`
  routes through the same resolver as every other write verb.
