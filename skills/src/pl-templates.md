---
description: Inspect, validate, and render Planar JSON templates for external-system propagation.
origin: docs/cli-reference.md
shared_notes:
    - Template state is read through the CLI; rendered examples are validation artifacts, not direct adapter writes.
slug: pl-templates
vendor:
    claude:
        argument_hint: '[set] [system] [kind]'
        invocation_examples: |
            /pl-templates
            /pl-templates list --system jira
            /pl-templates show default jira epic
            /pl-templates render default github-issues issue --entity task:<task-id>
            /pl-templates validate
            /pl-templates validate ~/.planar/templates/acme-internal/github-issues/issue.json
            /pl-templates init
            /pl-templates path default jira epic
---

# Templates ({{ VendorTitle }})

{{ VendorTitle }} skill surface for the `planar templates` CLI domain. See
[`docs/cli-reference.md`](../../docs/cli-reference.md) for the full specification of
every verb and its flags.

## When to use

Use this skill to:

- Inspect which templates are currently available and where they resolve from.
- Show the raw JSON of a specific template to understand its structure.
- Validate a template file for JSON syntax and `text/template` expression errors
  before deploying a custom set.
- Render a template against a real DB entity in dry-run mode to preview the
  payload that the ext-sync agent will send to the external system.
- Initialise the default template set on disk from the embedded baseline.

Do **not** use this skill to push changes to Jira or GitHub Issues. That is
handled by `pl-ext-propagate`.

## Resolution chain

Templates resolve via three fallback levels (highest priority first):

1. `<root>/<set>/<system>/<kind>.json` — the user-chosen template set on disk.
2. `<root>/default/<system>/<kind>.json` — the baseline set on disk.
3. Embedded defaults shipped in the binary — always present.

The root is `Config.Templates.Dir` (default: `~/.planar/templates/`). The
active set is determined by `[templates] default_set` in `config.toml`, or
overridden per-association via `[associations."<slug>"] default_template_set`.

## Main verbs

| Verb | Purpose |
|------|---------|
| `planar templates list` | List all available templates (set, system, kind, source). |
| `planar templates show <set> <system> <kind>` | Print raw JSON of a resolved template. |
| `planar templates render <set> <system> <kind> --entity <kind>:<id>` | Dry-run render against a DB entity. |
| `planar templates validate [<path>]` | Validate all templates or a single file. |
| `planar templates init` | Extract embedded defaults to disk (idempotent). |
| `planar templates path [<set> <system> <kind>]` | Print the templates root or a resolved template path. |

## Underlying CLI verbs

```
planar templates list [--system <system>] [--set <name>]
planar templates show <set> <system> <kind>
planar templates render <set> <system> <kind> --entity task:<task-id>
planar templates render <set> <system> <kind> --entity plan:<plan-id>
planar templates render <set> <system> <kind> --entity scenario:<scenario-id>
planar templates validate
planar templates validate /path/to/custom/issue.json
planar templates init
planar templates path
planar templates path default github-issues issue
```

## Customising templates

1. Run `planar templates init` to extract the baseline to `~/.planar/templates/default/`.
2. Copy the directory (or individual files) to a new set directory, e.g.
   `~/.planar/templates/acme-internal/`.
3. Edit the JSON files. String values support `text/template` directives that
   reference the rendering context (`.Task.Title`, `.Feature.Title`,
   `.Plan.Body`, `.Touches`, `.Assoc.Slug`, `.ExternalKey`, `.Children`, etc.).
4. Update `config.toml` to point to the new set:
   ```toml
   [templates]
   default_set = "acme-internal"
   ```
   Or per-association:
   ```toml
   [associations."org:acme"]
   default_template_set = "acme-internal"
   ```
5. Run `planar templates validate` to confirm there are no errors.
6. Run `planar templates render acme-internal github-issues issue --entity task:<task-id>`
   to preview the output before propagation.

## Context

Report the resolved templates root and set, system, kind, entity or file target,
resolution source, and read, validation, render-preview, or init mode.

## Intent

State in one sentence which template targets will be inspected, validated,
preview-rendered, or initialized.

## Actions

Report `attempted`, `applied` (the succeeded count), `skipped`, and `failed` per template file or
render target. List, show, path, validate, and render are read-only with zero
applied; `init` counts only files actually extracted and treats identical
existing defaults as skips. Name every failed set/system/kind/path target and
its validation evidence.

## Result

Always report `outcome=ok|partial|error`. Return resolved paths, resolution
source, validation findings, or rendered preview identities. After `init`,
confirm the extracted paths through `planar templates path` and validation.
An empty filter or fully initialized default set is an informative no-op.

## Warnings

Name fallback resolution, malformed JSON or expressions, missing entity
context, unavailable path verification, and partial initialization. Rendering
is always a preview and must never be described as an external write.

## Next actions

Give zero to three executable recommendations. A successful edit workflow
leads with `planar templates validate <path>` and an exact `planar templates
render <set> <system> <kind> --entity <kind:id>` preview before propagation.

## Recovery

For each failed template, give its exact `planar templates validate <path>` or
`planar templates render <set> <system> <kind> --entity <kind:id>` retry and
`planar templates path <set> <system> <kind>` inspection. Successfully
initialized independent files remain on disk; never imply cross-file rollback.

## Vendor Notes

