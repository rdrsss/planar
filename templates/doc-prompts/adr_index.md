---
doc_kind: adr_index
template_version: 1
synthesis_voice: aggregator
required_sources: [adr]
optional_sources: []
output_shape: A date-sorted table of ADRs, one row per record, each with id, title, status, date, and a one-line summary derived from the ADR's first paragraph.
---

# Synthesis prompt — ADR index

You are producing the auto-aggregated index of all architecture
decision records. The body is largely mechanical aggregation; your job
is shaping the table and condensing each ADR's first paragraph into a
one-line summary.

## Inputs

- `{{title}}` — the doc title (typically "Architecture decision records").
- `{{slug}}` — file slug.
- `{{kind}}` — always `adr_index`.
- `{{template_version}}` — always `1`.
- `{{adrs}}` — every `kind=adr` artifact, pre-sorted by date
  descending. Each entry is rendered as a YAML-ish block with `id`,
  `title`, `status`, `date`, and a `body` field containing the ADR
  text. Blocks are separated by `\n\n---\n\n`.

## Voice and shape

- Pure reference. No commentary, no analysis.
- One sentence per ADR for the summary. Lift the verb and the object
  from the ADR's first paragraph; do not paraphrase further.

## Required sections

1. **`# {{title}}`** — title heading.
2. **Index** — a markdown table with columns `ID | Title | Status |
   Date | Summary`. One row per ADR, newest first. Render the ID as
   `[ADR-0007](#adr-0007)` style cross-links if the entries section
   below uses those anchors; otherwise plain text.
3. **Entries** (optional) — if the input includes full ADR bodies,
   render each as an `## ADR-XXXX` section with its full text. If
   only summaries are supplied, omit this section.

## Output

Emit only the markdown body. The caller computes the provenance front
matter. No preamble.
