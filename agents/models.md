---
name: models
description: Tier-to-model mapping for every supported vendor.
---

# Models And Vendors

Agent specs in `agents/` reference abstract tiers (`small`, `medium`, `large`). The source of truth for concrete vendor model IDs is the Planar config (`[models.<vendor>]` tier maps in `~/.planar/config.toml`, with embedded defaults in `src/engine/config/defaults.toml`), resolved through the shared model resolver (plan 540). This document is the published view.

`planar skills render` regenerates the `## Tier Table` section by resolving each vendor's tier→model through that resolver (config defaults), keeping the surrounding prose human-authored. Run `planar config show --effective` to see the live values + provenance, or `planar models` to see installed providers.

## Tier Table

| Tier | Claude | Codex | Copilot |
| ------ | ------ | ----- | ------- |
| small | claude-haiku-4-5 | gpt-5.4-mini | gpt-5-mini |
| medium | claude-sonnet-4-6 | gpt-5.4 | gpt-5 |
| large | claude-opus-4-8 | gpt-5.5 | claude-opus-4 |
## Agent Assignments

| Agent              | Tier   |
|--------------------|--------|
| `orchestrator`     | large  |
| `spec-reviewer`    | large  |
| `coder`            | medium |
| `test-coder`       | large  |
| `reviewer`         | large  |

## Conventions

- Tiers are coarse on purpose. Add a new tier (e.g. `small` for routine status updates, `xl` for adversarial review) only when an agent spec demonstrably needs it.
- Removing or renaming a tier requires updating every agent file under `agents/` and every vendor surface in the same change.
- The agent spec owns the tier; this file owns the tier-to-model resolution. Agents must not name concrete model identifiers directly.

## Coder tier policy

The coder defaults to `medium` (sonnet). The orchestrator may escalate the spawned coder subagent to `large` (opus) for cycles that involve schema changes, engine-judgment calls, or large architectural diffs where the higher model tier materially improves the output. Tier is Axis C of the dispatch model and is independent of isolation (Axis A) — even a `large`-tier coder must run as a separately spawned subagent with blank context. Routine implementation, doc changes, and mechanical sweeps do not warrant escalation.

Axis C is **operator-confirmed, never silent**. The orchestrator surfaces the proposed tier per task in the Phase 3 dispatch preview (the task-breakdown table's tier column, with a one-word reason on every non-default row), and the operator may override any task's tier before confirming. The confirmed assignment is binding: the orchestrator spawns each coder at the confirmed tier and never silently escalates or downgrades — a mid-plan re-proposal is surfaced at the next preview render. Confirmed assignments persist in the dispatch entry's metadata as a `model_tiers` map for cross-cycle stickiness. See [`skills/src/pl-orchestrator.md` §Dispatch preview and model tiers](../skills/src/pl-orchestrator.md) for the preview format.

## Notes On Identifiers

- `claude-sonnet-4-6` and `claude-opus-4-8` are the current Anthropic identifiers as of 2026-06.
- Codex and Copilot identifiers must be verified against each vendor's current model list periodically. Treat the values above as defaults, not guarantees.
- Vendors that expose Anthropic models (e.g. Copilot routing to `claude-opus-4`) should resolve to the closest available identifier on that vendor, not the Anthropic-native one.
