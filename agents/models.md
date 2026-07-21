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
| medium | claude-sonnet-5 | gpt-5.4 | gpt-5 |
| large | claude-opus-4-8 | gpt-5.5 | claude-opus-4 |

## Candidate lists and work-type routing

Each `[models.<vendor>.<tier>]` value in `~/.planar/config.toml` (embedded
defaults in `src/engine/config/defaults.toml`) accepts either a **scalar**
(one model id — the shape shown in the Tier Table above) or an **ordered
list** of candidate model ids, e.g. `[models.codex] large = ["gpt-5.5",
"gpt-5.3-codex-spark"]`. A scalar resolves to a one-element list internally,
so every existing scalar config is unaffected; `list[0]` is always the
**tier default** — the model a caller gets when it resolves a bare
`(vendor, tier)` or `(vendor, role)` pair with no work type in hand
(`resolveTier`, `resolveRole`, `resolveRoleAuto`, and the render path's
`resolveModel` in `src/engine/skillrender.zig` all read `list[0]`).

A separate `[routing.<vendor>.<tier>]` sub-table maps a **work type**
(`schema | engine | architectural | cli | feature | mechanical` — see
§Coder tier policy below) to one of that tier's candidate model ids. It
ships as an embedded default (the shipped default only routes `mechanical`
to the tier default; every other work type falls back to `list[0]` until an
operator adds an entry) and is fully operator-overridable. The resolver
entry point is `resolve(role, work_type)` — concretely
`resolveRoleAutoWorkType` in `src/engine/models.zig` — which resolves the
role to a tier exactly like the tier-only path, then looks up
`routing.<vendor>.<tier>.<work_type>`: a hit returns the named candidate, a
miss falls back to `list[0]`. **Validation:** a routing entry naming a model
id absent from that tier's candidate list is a configuration error rejected
by `planar config validate` — not a silent fall-through.

The orchestrator's Phase 3 dispatch preview shows the routed-model candidate
`resolve(role, work_type)` selects per task, alongside the tier column (see
[`skills/src/pl-orchestrator.md` §Dispatch preview and model tiers](../skills/src/pl-orchestrator.md)).
`planar models candidates` prints the effective candidate lists and routing map
with provenance for operator inspection; see `docs/cli-reference.md` §Domain
`models` and §Domain `config` (Model routing) for the full CLI surface.

## Agent Assignments

Every installable agent under `agents/`, its authored `tier:` (the source of
truth — see §Conventions), its `capability:` (which drives the Codex
`sandbox_mode` and the Claude tool grant), and its primary work. Rows marked †
are the six **runtime-resolvable roles** whose tier is *also* carried in
`[roles]` of `src/engine/config/defaults.toml` for the plan-540 model resolver;
those two copies MUST agree. Every other agent resolves its model straight from
this frontmatter via the render path.

| Agent               | Tier   | Capability  | Primary work |
|---------------------|--------|-------------|--------------|
| `orchestrator`      | large  | coordinate  | Full lifecycle dispatch; phase selection, escalation, iteration-cap judgment |
| `planner`           | large  | write       | Drafts product / tech / test specs and the roadmap |
| `spec-reviewer`     | large  | write       | Adversarial spec review before ingestion |
| `ingestor`          | large  | coordinate  | Decomposes workbench specs into the task graph |
| `coder` †           | medium | write       | Scoped implementation (escalates to `large` per §Coder tier policy) |
| `test-coder` †      | large  | write       | Adversarial test authoring against the coder diff |
| `reviewer` †        | large  | read-only   | approve / request-changes / open-question / abort |
| `janitor`           | medium | coordinate  | Merge, reconcile, cleanup, plan closeout |
| `documenter` †      | large  | read-only   | Proposes the doc worklist from repo drift |
| `doc-author` †      | large  | write       | Authors approved reference prose under `docs/` |
| `ext-sync`          | large  | coordinate  | Propagates a feature to Jira / GitHub Issues |
| `sync-reconciler` † | large  | coordinate  | Reconciles local/external sync conflicts |
| `importer`          | large  | write       | Translates an existing repo's planning content into Planar |
| `synthesizer`       | large  | write       | Synthesizes fresh planning artifacts via an LLM pass |
| `introspector`      | medium | coordinate  | Usage / friction introspection over redacted signal |
| `feedback-triager`  | large  | coordinate  | Triages redacted feedback findings |

## Conventions

- Tiers are coarse on purpose. Add a new tier (e.g. `small` for routine status updates, `xl` for adversarial review) only when an agent spec demonstrably needs it.
- Removing or renaming a tier requires updating every agent file under `agents/` and every vendor surface in the same change.
- The agent spec owns the tier; this file owns the tier-to-model resolution. Agents must not name concrete model identifiers directly.

## Coder tier policy

The coder defaults to `medium` (sonnet). The orchestrator may escalate the spawned coder subagent to `large` (opus) for cycles that involve schema changes, engine-judgment calls, or large architectural diffs where the higher model tier materially improves the output. Tier is Axis C of the dispatch model and is independent of isolation (Axis A) — even a `large`-tier coder must run as a separately spawned subagent with blank context. Routine implementation, doc changes, and mechanical sweeps do not warrant escalation.

The escalation is keyed to the **type of work** in the cycle, not to the task
count. The orchestrator classifies each task's dominant work type and proposes
the corresponding tier in the Phase 3 dispatch preview; when a cycle mixes work
types, the highest-tier row present wins:

| Work type | Tier | Signals (any one triggers the row) |
|-----------|------|------------------------------------|
| Schema / migration | large | new or edited `migrations/*.sql`; a change to the `schema_migrations` contract; a CHECK-constraint or index redesign |
| Engine judgment | large | non-trivial logic under `src/engine/` or `src/db/`; error-set / allocator-ownership design; a status-transition or scope-resolution rule change |
| Architectural | large | a new subsystem or binary; a cross-module diff touching many packages; a change to a locked capability boundary |
| CLI-surface change | large | a new top-level verb, subcommand, or flag whose contract must be pinned by an integration test |
| Feature (default) | medium | single-verb handler wiring, a bounded feature addition within an existing surface |
| Mechanical / docs | medium | renames, formatting sweeps, comment/doc-only edits, prose under `docs/`, workflow-surface text |

The one-word reason the orchestrator prints on every non-default (`large`) row
in the dispatch preview is the matching work-type name from this table
(`schema`, `engine`, `architectural`, `cli`). The default (`medium`) rows carry
no reason. The operator may override any task's tier at the gate; the confirmed
`model_tiers` map is binding for dispatch (Axis C, below).

Axis C is **operator-confirmed, never silent**. The orchestrator surfaces the proposed tier per task in the Phase 3 dispatch preview (the task-breakdown table's tier column, with a one-word reason on every non-default row), and the operator may override any task's tier before confirming. The confirmed assignment is binding: the orchestrator spawns each coder at the confirmed tier and never silently escalates or downgrades — a mid-plan re-proposal is surfaced at the next preview render. Confirmed assignments persist in the dispatch entry's metadata as a `model_tiers` map for cross-cycle stickiness. See [`skills/src/pl-orchestrator.md` §Dispatch preview and model tiers](../skills/src/pl-orchestrator.md) for the preview format.

## Notes On Identifiers

- `claude-sonnet-5` and `claude-opus-4-8` are the current Anthropic identifiers as of 2026-07.
- Codex and Copilot identifiers must be verified against each vendor's current model list periodically. Treat the values above as defaults, not guarantees.
- Vendors that expose Anthropic models (e.g. Copilot routing to `claude-opus-4`) should resolve to the closest available identifier on that vendor, not the Anthropic-native one.
