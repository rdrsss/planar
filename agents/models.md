---
name: models
description: Tier-to-model mapping for every supported vendor.
---

# Models And Vendors

Agent specs in `agents/` reference abstract tiers (`medium`, `large`). The source of truth for concrete vendor model IDs is `src/configs/vendors.yaml`; this document is the published view.

`planar skills render` regenerates the `## Tier Table` section from `src/configs/vendors.yaml` while keeping the surrounding prose human-authored.

## Tier Table

| Tier | Claude | Codex | Copilot |
| ------ | ------ | ----- | ------- |
| medium | claude-sonnet-4-6 | gpt-5-codex | gpt-5 |
| large | claude-opus-4-7 | gpt-5 | claude-opus-4 |
## Agent Assignments

| Agent          | Tier   |
|----------------|--------|
| `orchestrator` | large  |
| `coder`        | medium |
| `test-coder`   | large  |
| `reviewer`     | large  |

## Conventions

- Tiers are coarse on purpose. Add a new tier (e.g. `small` for routine status updates, `xl` for adversarial review) only when an agent spec demonstrably needs it.
- Removing or renaming a tier requires updating every agent file under `agents/` and every vendor surface in the same change.
- The agent spec owns the tier; this file owns the tier-to-model resolution. Agents must not name concrete model identifiers directly.

## Notes On Identifiers

- `claude-sonnet-4-6` and `claude-opus-4-7` are the current Anthropic identifiers as of 2026-05.
- Codex and Copilot identifiers must be verified against each vendor's current model list periodically. Treat the values above as defaults, not guarantees.
- Vendors that expose Anthropic models (e.g. Copilot routing to `claude-opus-4`) should resolve to the closest available identifier on that vendor, not the Anthropic-native one.
