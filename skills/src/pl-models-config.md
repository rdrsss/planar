---
slug: pl-models-config
description: "Discover installed provider CLIs and review/configure per-role model routing (plan 540)."
source: docs/cli-reference.md#domain-models
vendor:
  claude:
    argument_hint: "[]"
    invocation_examples: |
      /pl-models-config
shared_notes:
  - "All provider/model state comes from the CLI (`planar models`, `planar config`); the skill must not read or write config files directly except by invoking `planar models apply` or guiding the operator to edit `~/.planar/config.toml`."
  - "The provider CLIs do not enumerate models; the per-vendor catalog is curated in the binary, so `models list` reports installed-state, not a live model fetch."
---

# Pl-Models-Config ({{.VendorTitle}})

Guided review and configuration of which model each role spawns, across every supported provider (Claude, Codex), using the shared model resolver (plan 540).

## When To Invoke

- You want to see which provider CLIs are installed and what models they expose.
- You want to check or change the effective role→model routing (e.g. route the coder to Codex, or bump the reviewer's tier).
- An operator asks "what model will the coder/reviewer actually run?"

## Mental Model

Routing is config-driven and unified in `~/.planar/config.toml`:

- `[models.<vendor>]` — per-vendor tier maps (`small` / `medium` / `large` → concrete model id).
- `[roles]` — role → tier (e.g. `coder = "medium"`, `reviewer = "large"`).
- `[role_vendors]` — optional role → vendor override (defaults to `[defaults].vendor`).

The shared resolver composes these. Skills render, `agents/models.md`, and external workflow harnesses all resolve through it — there is no separate per-tool model table.

## What It Does

1. **Discover** — `planar models list` reports each provider CLI's installed-state + version and its curated model catalog (with human labels), plus the default routing.
2. **Inspect routing** — `planar models routing` prints the effective role → `vendor model` mapping with provenance (`[embedded default]` vs `[config file]`). `--json` for machine consumption (this is what an external workflow harness shells).
3. **Cache** — `planar models refresh` writes the discovery result to `~/.planar/models/catalog.json`.
4. **Scaffold** — `planar models apply` writes the `[models]`/`[roles]` block into the config file as an editable starting point (idempotent; `--force` to append again).
5. **Override** — guide the operator to edit `~/.planar/config.toml`:
   - re-route a tier: set `[models.codex] medium = "gpt-5.4"`.
   - move a role's tier: set `[roles] coder = "large"`.
   - route a role to another vendor: set `[role_vendors] coder = "codex"`.
   Then re-run `planar models routing` to confirm the change took, with provenance now showing `[config file]`.

## What It Must Not Do

- Do not hand-edit `agents/models.md` or any rendered surface — those regenerate from config via the resolver.
- Do not invent per-call model overrides in Lua workflows; routing is per-role and config-driven.

## Context

Report the selected discovery, routing, refresh, scaffold, or guided-edit mode;
the resolved config and catalog paths; and the vendors or roles in scope.

## Intent

State in one sentence whether the operator wants to inspect or change effective
role-to-model routing.

## Actions

Report `attempted`, `applied`, `skipped`, and `failed` counts for provider
discovery, cache refresh, config scaffold, and routing checks. Read-only
inspection has zero applied; an already-present scaffold is an expected skip.

## Result

Always report `outcome=ok|partial|error` and the effective routing with its
provenance. After `planar models refresh`, verify with `planar models list
--json`; after `planar models apply`, verify with `planar models routing
--json`. Return the affected role/vendor mappings and durable path, not only a
successful exit code.

## Warnings

Name missing provider CLIs, curated rather than live catalog evidence, config
parse errors, unavailable post-state verification, and forced duplicate
scaffolding. An uninstalled optional provider is not a failure unless requested.

## Next actions

Give zero to three executable recommendations, led by the exact routing check
or the specific config edit the operator requested.

## Recovery

Provide `planar models routing --json` to inspect the last effective state and
an idempotent `planar models refresh` or `planar models apply` retry when
applicable. Do not claim a config rollback that the CLI did not perform.

## Vendor Notes

{{.VendorNotes}}
