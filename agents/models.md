---
description: Tier-to-model mapping for every supported vendor.
kind: doc
slug: models
---

# Models And Vendors

Agent specs in `agents/` reference abstract tiers (`small`, `medium`, `large`). The source of truth for concrete vendor model IDs is the Planar config (`[models.<vendor>]` tier maps in `~/.planar/config.toml`, with embedded defaults in `src/engine/config/defaults.toml`), resolved through the shared model resolver (plan 540). This document is the published view.

`planar models sync-doc` regenerates the `## Tier Table` section by resolving each vendor's tier→model through that resolver (config defaults), keeping the surrounding prose human-authored. Run `planar config show --effective` to see the live values + provenance, or `planar models` to see installed providers.

## Tier Table

| Tier | Claude | Codex | Copilot | Gemini |
| ------ | ------ | ----- | ------- | ------ |
| small | claude-haiku-4-5 | gpt-5.6-luna | gpt-5-mini | gemini-3.1-flash |
| medium | claude-sonnet-5 | gpt-5.6-terra | gpt-5 | gemini-3.1-pro |
| large | claude-opus-4-8 | gpt-5.6-sol | claude-opus-4 | gemini-3.1-pro |
## Candidate lists and work-type routing

Each `[models.<vendor>.<tier>]` value in `~/.planar/config.toml` (embedded
defaults in `src/engine/config/defaults.toml`) accepts either a **scalar**
(one model id — the shape shown in the Tier Table above) or an **ordered
list** of candidate model ids, e.g. `[models.codex] large = ["gpt-5.6-sol",
"gpt-5.5"]`. A scalar resolves to a one-element list internally,
so every existing scalar config is unaffected; `list[0]` is always the
**tier default** — the model a caller gets when it resolves a bare
`(vendor, tier)` or `(vendor, role)` pair with no work type in hand
(`resolveTier`, `resolveRole`, `resolveRoleAuto`, and `buildTierTableLines`
in `src/engine/models.zig` all read `list[0]`).

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

## Host capability boundary

Configuration expresses desired routing; the active host's subagent surface
defines what can actually be dispatched. Host-native orchestration never
crosses providers: a Codex host selects Codex candidates and a Claude host
selects Claude candidates even when `defaults.vendor` names another provider.
Cross-provider execution requires a separate external executor and is not the
host subagent path described by the orchestrator contract.

The preview records both the desired candidate and its concrete host binding.
If the host cannot represent that candidate, the row is `unsupported` and the
orchestrator stops for operator action. It must not silently substitute another
provider, tier, candidate, or agent type. Claude supports an invocation-level
model parameter, subject to the higher-precedence
`CLAUDE_CODE_SUBAGENT_MODEL` environment override. Codex agent roles may be
fixed to the model in their installed TOML projection; only agent types visible
in the current dispatch surface are valid bindings.

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

The coder defaults to `medium` (sonnet), and the default is load-bearing, not a starting bid. Planar's premise is that the hard reasoning happens upfront — in the spec, the decomposition, and the locked decisions — so execution is deliberately cheap and fast: a well-decomposed task carries its own context and a `medium` coder is expected to close it. The orchestrator proposes `large` (opus) per task, as an exception it can name, never as a batch default. Tier is Axis C of the dispatch model and is independent of isolation (Axis A) — even a `large`-tier coder must run as a separately spawned subagent with blank context. Routine implementation, CLI-surface additions, doc changes, and mechanical sweeps do not warrant escalation.

Task-title vocabulary is never sufficient escalation evidence. `config`,
`module`, `composition root`, `engine`, `refactor`, and a large file count remain
`medium` when the spec has already made the design decisions. Every `large`
proposal cites a concrete unresolved schema, allocator, error-set, transaction,
status-transition, or architecture judgment from the acceptance criteria or
spec. If it cannot, the task remains `medium`.

The escalation is keyed to the **type of work** in each task — not to the task
count, and not to the task's batch-mates. The orchestrator classifies each
task's dominant work type and proposes the corresponding tier in the Phase 3
dispatch preview:

| Work type | Tier | Signals (any one triggers the row) |
|-----------|------|------------------------------------|
| Schema / migration | large | new or edited `migrations/*.sql`; a change to the `schema_migrations` contract; a CHECK-constraint or index redesign |
| Engine judgment | large | allocator-ownership or error-set design; a transaction/atomicity boundary change; a status-transition, scope-resolution, or locked-capability-boundary rule change. Routine engine wiring that follows an existing pattern is `feature`, not `engine` — "it touches `src/engine/`" is not by itself an escalation signal. |
| Architectural | large | a new subsystem or binary; a cross-module diff touching many packages |
| CLI-surface change | medium | a new top-level verb, subcommand, or flag whose contract must be pinned by an integration test. The contract is pinned by the integration suite and the decomposition carries the design; a surface change that genuinely requires cross-cutting parser or design judgment classifies as `architectural` instead. |
| Feature (default) | medium | single-verb handler wiring, a bounded feature addition within an existing surface |
| Mechanical / docs | medium | renames, formatting sweeps, comment/doc-only edits, prose under `docs/`, workflow-surface text |

The one-word reason the orchestrator prints on every non-default (`large`) row
in the dispatch preview is the matching work-type name from this table
(`schema`, `engine`, `architectural`). The default (`medium`) rows carry
no reason. The operator may override any task's tier at the gate; the confirmed
`model_tiers` map is binding for dispatch (Axis C, below).

**Tiers are per-task; a cycle never inherits its highest task's tier.** The
former highest-row-wins rule (a mixed cycle escalates wholesale to the highest
tier present) is retired. When a proposed `grouped`/`single` cycle mixes
confirmed tiers, the orchestrator partitions the group by tier — one coder per
tier partition, dependency edges still ordering the dispatches — or falls back
to per-task dispatch (`strict` / `barrel-deferred`) for that cycle. Inflating a
`medium` task to `large` because of its batch-mates is prohibited; so is
silently folding a `large` task into a `medium` batch.

**Ambiguity escalates to the operator, not to opus.** When the classifier
genuinely cannot decide a task's dominant work type (competing signals, an
under-specified task body), the orchestrator does NOT round up to `large`. It
renders the row as `tier: ?` with the competing signals named — e.g.
`(engine? feature? — touches src/engine/ but follows the extant handler
pattern)` — and the gate requires an explicit operator answer for that row
before any cycle containing it dispatches. Uncertainty is a routing question
for the operator, not a budget decision the orchestrator resolves by rounding
up.

**Opportunistic escalation on reviewer bounce.** When a `medium` cycle fails
two consecutive reviewer iterations and the failures read as capability gaps
(the coder misunderstands the design, not the spec being ambiguous), the
orchestrator may propose re-dispatching the remaining iterations at `large` —
surfaced at the next preview render with the reviewer evidence, never applied
silently. Spec ambiguity escalates to the user as an open question instead; a
bigger model does not fix an under-specified task.

Axis C is **operator-confirmed, never silent**. The orchestrator surfaces the proposed tier per task in the Phase 3 dispatch preview (the task-breakdown table's tier column, with a one-word reason on every non-default row), and the operator may override any task's tier before confirming. The confirmed assignment is binding: the orchestrator spawns each coder at the confirmed tier and never silently escalates or downgrades — a mid-plan re-proposal is surfaced at the next preview render. Confirmed assignments persist in the dispatch entry's metadata as a `model_tiers` map for cross-cycle stickiness. See [`skills/src/pl-orchestrator.md` §Dispatch preview and model tiers](../skills/src/pl-orchestrator.md) for the preview format.

## Candidate use-cases within a tier

When a tier carries more than one candidate, the routing map is where "which
one, for what" gets encoded — over the existing work-type vocabulary, never new
ad-hoc labels (the orchestrator's classifier and `planar models evals`'
scorecard both key on `schema | engine | architectural | cli | feature |
mechanical`). The shipped priors:

- **`claude-fable-5` vs `claude-opus-4-8`** (Claude `large`): opus is the tier
  default — reviewers, escalated coders, and every bare large resolution get
  opus. Fable is Mythos-class (above opus) and is routed only where a wrong
  early judgment cascades hardest: the shipped seed routes `architectural` →
  fable. Widen (e.g. `schema` → fable) or retract via `[routing.claude.large]`
  in `~/.planar/config.toml`.
- **`gpt-5.6-sol` vs `gpt-5.5`** (Codex `large`): sol is the current frontier
  default; gpt-5.5 stays listed as a routable fallback candidate.
- These are **priors, not conclusions**. `planar models evals` aggregates
  completed dispatches into a per-(work-type, candidate) scorecard and emits
  preview-only routing recommendations — let accumulated dispatch history,
  not intuition, decide whether a routing entry earns its cost.

## Notes On Identifiers

- `claude-sonnet-5`, `claude-opus-4-8`, and `claude-fable-5` are the current Anthropic identifiers as of 2026-07. `claude-fable-5` is the Mythos-class tier above opus — kept as a routable `large` candidate, deliberately not the tier default.
- Codex and Copilot identifiers must be verified against each vendor's current model list periodically. Treat the values above as defaults, not guarantees.
- Vendors that expose Anthropic models (e.g. Copilot routing to `claude-opus-4`) should resolve to the closest available identifier on that vendor, not the Anthropic-native one.
