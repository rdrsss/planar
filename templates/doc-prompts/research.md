---
doc_kind: research
template_version: 1
synthesis_voice: academic
required_sources: [research]
optional_sources: [tech_spec, decision, adr]
output_shape: A research note with the academic five-section structure (abstract, motivation, related work, method, findings) plus a citations footer, suitable for inclusion in docs/research/.
---

# Synthesis prompt — research note

You are producing a research note in academic tone. The audience is a
fellow engineer evaluating whether the investigation's findings change
how Planar should behave.

## Inputs

- `{{title}}` — the note's title.
- `{{slug}}` — file slug.
- `{{kind}}` — always `research`.
- `{{template_version}}` — always `1`.
- `{{sources}}` — one or more internal research artifacts (`kind=
  research`), optionally accompanied by tech specs, decisions, or
  ADRs that contextualize the investigation. Bodies joined by
  `\n\n---\n\n`.

## Voice and shape

- Third-person, declarative, hedged where appropriate ("the data
  suggest", not "this proves").
- Citations are mandatory. Every empirical or external claim must
  carry a footnote reference `[^id]` whose declaration lives in the
  doc's `references:` front matter. The caller will append the
  reference declarations after synthesis; emit the `[^id]` body
  citations so they are present for the citation linter.
- Prefer concrete numbers and reproducible procedure over hand-wavy
  argument.

## Required sections

1. **`# {{title}}`** — title heading.
2. **`## Abstract`** — 3–6 sentence summary of the question,
   approach, and result.
3. **`## Motivation`** — why the question matters now; what decision
   downstream depends on the answer.
4. **`## Related work`** — prior investigations or external
   literature. Cite via `[^id]`.
5. **`## Method`** — what was measured, how, against what fixtures.
6. **`## Findings`** — observations grouped by question. Cite via
   `[^id]` where claims rest on a specific source.
7. **`## References`** — short prose paragraph pointing readers to
   the front-matter `references:` block. The renderer fills the
   actual footer; this section is the human breadcrumb.

## Output

Emit only the markdown body. The caller computes the provenance front
matter and appends the rendered reference footer. No preamble.
