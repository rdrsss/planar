---
doc_kind: getting_started
template_version: 1
synthesis_voice: imperative
required_sources: [tech_spec, product_spec]
optional_sources: [decision]
output_shape: A from-zero onboarding tutorial that takes a new user from install to first useful action, structured as numbered steps with command examples.
---

# Synthesis prompt — getting started

You are producing the onboarding tutorial for Planar. Read it as the
first thing a brand-new user sees after deciding to try the tool.

## Inputs

- `{{title}}` — the doc title (typically "Getting started").
- `{{slug}}` — file slug.
- `{{kind}}` — always `getting_started`.
- `{{template_version}}` — version pin (always `1` today).
- `{{sources}}` — internal tech-spec and product-spec bodies, joined
  by `\n\n---\n\n`. The tech spec contributes install / setup steps;
  the product spec contributes the first-task narrative.

## Voice and shape

- Imperative, second-person ("Run `planar init`", "Create your first
  plan with ...").
- Sequential and concrete. Each step is one action with one verified
  outcome (a CLI output line, a created file).
- No conceptual digressions. Concepts get their own doc; this one
  drives the user to a working first session.
- Prefer short paragraphs. Long preambles lose the new user.

## Required sections

1. **`# {{title}}`** — title heading.
2. **Install** — one-paragraph install instruction with the canonical
   command. Confirm-with-`planar version`.
3. **Initialize** — `planar init` step, including what it creates on
   disk (`~/.planar/planar.db`, default config).
4. **First plan** — walkthrough creating a plan, adding a task,
   listing the result. Use real command output as the verified
   outcome.
5. **What's next** — short list of links to the feature catalog,
   workflows, and concepts docs.

## Output

Emit only the markdown body. The provenance front matter is computed
and prepended by the caller. Do not emit preamble or trailing
commentary.
