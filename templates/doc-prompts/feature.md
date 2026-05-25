---
doc_kind: feature
template_version: 1
synthesis_voice: imperative
required_sources: [product_spec]
optional_sources: [tech_spec, decision]
output_shape: A feature-catalog entry naming the capability, summarizing user-visible behavior, and listing concrete use cases without exposing implementation details or decision history.
---

# Synthesis prompt — feature catalog entry

You are producing one entry in Planar's outward-facing feature catalog.
The entry's job is to tell a user what the feature does for them, in
imperative voice, without leaking implementation choices or internal
debate.

## Inputs

- `{{title}}` — the human-readable title to use for the entry.
- `{{slug}}` — the file slug (used for the H1 anchor).
- `{{kind}}` — always `feature` in this template.
- `{{template_version}}` — the template version (always `1` today).
- `{{sources}}` — one or more internal product-spec bodies, separated
  by a `\n\n---\n\n` divider line. Each body retains its own H1/H2
  structure. Optional tech-spec or decision bodies follow the product
  specs under the same divider convention.

## Voice and shape

- Imperative voice ("Use the X verb to ...", "Pass `--scope` to ...").
- Capability-oriented: lead with what the user can now do, not how
  Planar implements it.
- Strip anything that smells like a decision section ("Why we chose X
  over Y", "Rejected alternatives"). The catalog is not the place.
- Strip open questions, TODOs, and risk-mitigation tables.
- Keep concrete CLI examples; remove pseudo-code and architecture
  diagrams.

## Required sections

1. **`# {{title}}`** — title heading.
2. **Summary** — one paragraph, 2–4 sentences, naming the capability
   and the user payoff.
3. **What it does** — bulleted list of user-visible behaviors. Each
   bullet starts with an imperative verb.
4. **CLI** — copy-pasteable command examples, one per use case.
5. **Related** — a short list of cross-links to other feature entries
   or concept docs, by slug. Optional.

## Output

Emit only the markdown body. Do not emit the provenance front matter;
the caller computes and prepends it. Do not include preamble like
"Here is the entry:".
