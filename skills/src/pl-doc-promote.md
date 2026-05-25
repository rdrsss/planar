---
slug: pl-doc-promote
description: "Promote internal Planar artifacts into a published doc by synthesizing through a doc-kind template."
source: docs/cli-reference.md#domain-doc
vendor:
  claude:
    argument_hint: "--kind <kind> --source <ref> [--source <ref>...] [--slug <slug>] [--out <path>]"
    invocation_examples: |
      /pl-doc-promote --kind feature --source artifact:47 --slug auth-flow
      /pl-doc-promote --kind getting_started --source artifact:35 --source artifact:36
      /pl-doc-promote --kind adr_index --source artifact:75
      /pl-doc-promote --kind research --source artifact:64 --slug scope-resolution
      /pl-doc-promote --kind feature --source artifact:47 --out docs/features/auth.md
shared_notes:
  - "Source entities and template state come from the CLI; the skill must not read or write planar context outside it."
---

# Planar Doc Promote ({{.VendorTitle}})

Synthesises an outward-facing doc from one or more internal Planar
entities (artifact, decision, plan) using a doc-kind synthesis template.

## What It Does

Reads each `--source` entity's body from the database, fills the
doc-kind synthesis template, calls the LLM at temperature 0 to produce
the synthesised doc body, then hands that body to `planar doc promote`
which writes the doc with provenance front matter and updates the
docs manifest.

## CLI Commands

Wraps [`doc`](../../docs/cli-reference.md#domain-doc):

```
planar doc promote --kind <kind> --source <ref> [--source <ref>...]
                   [--slug <slug>] [--out <path>] [--body-file <path>]
                   [--title <title>]
planar artifact show <id> --json
planar decision show <id> --json
planar plan show <id> --json
```

## When To Invoke

Run this skill once a body of work has shipped and an outward-facing
doc would be valuable — for example after a feature lands you want a
feature-catalog entry for, after an ADR set has stabilised and an
adr-index page is overdue, or when a research artifact is ready to be
published as a research doc. The skill is deliberate; it consumes LLM
budget and writes a new file under `docs/`, so it should not be a
per-session reflex.

## How the Skill Composes

1. Validate `--kind` against the supported doc kinds: `feature`,
   `getting_started`, `adr_index`, `changelog`, `glossary`, `research`.
   Fail fast with a clear message if the operator passed an unknown
   kind — do not invoke the LLM on an invalid request.

2. Resolve every `--source` entity-ref to its body. Parse each ref as
   `kind:id` (artifact:N, decision:N, plan:N), then call the matching
   show verb with `--json` (`planar artifact show <id> --json`,
   `planar decision show <id> --json`, or `planar plan show <id> --json`)
   to fetch the body. If any source fails to resolve, abort and surface
   the error — the synthesis prompt depends on every declared source
   being present.

3. Load the doc-prompt template at
   `~/.planar/templates/doc-prompts/<kind>.md`. The template's own
   front matter declares `template_version`, `synthesis_voice`,
   `required_sources`, `optional_sources`, and `output_shape`; the body
   below the fence is the prompt the LLM receives.

4. Invoke the LLM at `temperature=0` with the template prompt plus the
   concatenated source bodies. The expected output is the synthesised
   doc body — markdown without a front-matter block, since the Go CLI
   prepends provenance separately.

5. Compute the provenance front matter inputs. The skill does not need
   to format the YAML — `planar doc promote` does that — but it does
   need to know the values: `source_artifacts` / `source_decisions` /
   `source_plans` partition the resolved refs by kind, `source_versions`
   maps each ref to the xxh64 of its body bytes (the Go CLI recomputes
   this), `regenerated_at` is `now()` in RFC3339 UTC, `regenerated_by`
   is `pl-doc-promote`, `template_version` is the integer from the
   template front matter, and `title` is derived from `--slug` or the
   `--out` filename when both are absent.

6. Write the synthesised body to a tmpfile (e.g.
   `$TMPDIR/pl-doc-promote-<kind>-<slug>.md`), then invoke
   `planar doc promote --kind <kind> --source <ref>... --slug <slug>
   --body-file <tmpfile>` (or `--out <path>` for explicit overrides).
   The Go verb composes the provenance front matter, atomic-writes the
   doc to `--out` (default: `docs/<kind-derived-subdir>/<slug>.md`),
   and updates the manifest.

7. The Go verb also runs `planar doc manifest update` semantics
   inline — re-walking `docs/` and re-writing `.manifest-docs` so the
   new doc is registered with the correct per-source hashes. The skill
   does not need a separate manifest call.

## LLM Synthesis Contract

The synthesis call is deterministic and constrained:

- `temperature=0`. No nucleus or top-p sampling; reproducibility is the
  contract.
- The template's `synthesis_voice` field governs phrasing. `imperative`
  means "tell the user what the feature does for them"; `aggregator`
  means "compile entries with minimal commentary".
- The template's `output_shape` line is the one-sentence summary of
  what the LLM is producing. The body must respect it — do not invent
  sections beyond the shape's scope.
- Do NOT invent sources. The synthesis prompt's `## Inputs` section is
  exhaustive; if a fact does not appear in one of the declared source
  bodies, omit it rather than confabulating.
- Produce footnote-style `CITATION` markers only for references that
  are explicitly declared in the source bodies or in a `references:`
  block the operator passed in via the source artifact's front matter.
  The doc linter (`planar doc lint`) rejects undeclared citations.

## Output

A single line on stdout describing the write:

```
wrote docs/features/auth-flow.md (kind feature, 2 source(s), manifest root a3f1c2d4e5b6f708)
```

In `--json` mode the verb emits:

```json
{
  "ok": true,
  "path": "docs/features/auth-flow.md",
  "kind": "feature",
  "sources": [
    {"ref": "artifact:47", "hash": "a3f1c2d4e5b6f708"},
    {"ref": "decision:7",  "hash": "9b2e8d6c4f1a3027"}
  ],
  "manifest_root": "1a2b3c4d5e6f7080"
}
```

## Authoring Conventions

This skill body adheres to the six rules established by task 602:

1. Quoted titles ("Title") not bare. The frontmatter `description`
   value is a double-quoted string.
2. Literal headings (`## What It Does`, `## CLI Commands`,
   `## When To Invoke`, `## How the Skill Composes`,
   `## LLM Synthesis Contract`, `## Output`, `## Vendor Notes`,
   `## Invocation`).
3. No nested bullets. Step lists use only top-level numbered items with
   prose; sub-letters are flat under the parent number where used.
4. No `## Out of this plan` H2. Deferred items belong in the tech spec's
   `## Out of scope` section.
5. Always double-quote `title:`-style values when this skill emits
   front matter elsewhere. The provenance front matter that `planar doc
   promote` writes follows this convention via the provenance.Format
   YAML emitter; the skill itself only writes a synthesised body to a
   tmpfile, never directly to a doc.
6. Workbench discipline: this skill MUST NOT read or write the
   workbench filesystem. It reads entity bodies through `planar
   artifact|decision|plan show --json`, writes a tmpfile, and invokes
   `planar doc promote` to commit the result.

## Vendor Notes

{{.VendorNotes}}
{{- if .InvocationBlock}}

## Invocation

```
{{.InvocationBlock -}}
```
{{- end}}
