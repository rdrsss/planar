---
doc_kind: glossary
template_version: 1
synthesis_voice: aggregator
required_sources: [glossary_term]
optional_sources: []
output_shape: An alphabetized glossary with one H2 sub-section per term, each containing the term's definition and any cross-references to related terms or feature entries.
---

# Synthesis prompt — glossary

You are producing the project glossary by aggregating
`kind=glossary_term` artifacts into a single alphabetized reference.

## Inputs

- `{{title}}` — the doc title (typically "Glossary").
- `{{slug}}` — file slug.
- `{{kind}}` — always `glossary`.
- `{{template_version}}` — always `1`.
- `{{terms}}` — every `kind=glossary_term` artifact, pre-sorted
  alphabetically by term. Each is rendered as `term: <name>` plus a
  `body` field with the definition. Entries separated by
  `\n\n---\n\n`.

## Voice and shape

- Definitional. Open each entry with a noun phrase that completes the
  sentence "A <term> is ...".
- No editorializing, no examples beyond what the source body
  contains.

## Required sections

1. **`# {{title}}`** — title heading.
2. One `## <Term>` per glossary term, alphabetized. Body is the term's
   definition. If the source body mentions related terms, render a
   trailing italicized "See also" line linking to their anchors.

## Output

Emit only the markdown body. The caller computes the provenance front
matter. No preamble.
