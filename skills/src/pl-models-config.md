---
description: Inspect the opaque candidate registry, routing evidence, and role resolution; guide operator-gated preset edits.
origin: docs/architecture.md#host-aware-agent-model-binding
shared_notes:
    - All routing state comes from the CLI (`planar models`); the skill must not read or write config files directly.
    - Planar stores candidate ids opaquely and never validates them against a supported list — the tier-to-model presets live in the orchestration layer's `agents/models.md`, not in the planar binary.
slug: pl-models-config
vendor:
    claude:
        argument_hint: '[]'
        invocation_examples: |
            /pl-models-config
---

# Pl-Models-Config ({{ VendorTitle }})

Inspect what Planar knows about routing: which opaque candidates are
registered, what evidence exists, and what tier a role resolves to and why.

## When To Invoke

- An operator asks "what tier will the coder actually run at, and why?"
- You want to see which candidates are registered and whether they are eligible.
- You want to review routing evidence before trusting a recommendation.
- An operator wants to change which model a tier maps to.

## Mental Model

Ownership is split, and the split is the point:

- **Planar** stores candidate ids as opaque bytes, records coordination state
  and evidence, and *resolves* a role to a tier from the task's own packet. It
  never parses an id and never decides which models are supported.
- **The orchestration layer** owns the tier→model presets (`agents/models.md` §Candidate
  Presets), hand-maintained. There is no `[models]` or `[roles]` config block
  and no `planar models apply`; those were removed with the curated catalog.
- **Host adapters** own spawn verification. Planar records requested and actual
  identity separately and names a mismatch; it never asserts which model
  answered.

Evidence is cohort-local. Every sample is scoped to `(project, validation
policy version, vendor, role, tier, work type, complexity)` and is never pooled
across any of them, so evidence from one project says nothing about another.

## What It Does

1. **Registry** — `planar models registry list --json` reports each opaque
   candidate, its role/tier bindings, and the latest host observation. A
   candidate whose id has not been verified spawn-safe on this host is
   ineligible for routing, not merely unproven.
2. **Resolve** — `planar models resolve --role <role> [--task <id>] [--plan <id>]`
   answers what tier a role gets and whether the answer is backed by anything.
   `source: packet` means it was derived from the task's compiled profile;
   `static_fallback` means it was not, and the reason is named
   (`no_packet`, `packet_not_ready`, `policy_not_ready`). On a fallback the
   work type and complexity are null — report that, never a derived-looking
   tier without its provenance.
3. **Evidence** — `planar models experiments --json` lists declared experiments
   with recorded vs counted sample totals; `planar models outcomes --json`
   lists terminal outcomes INCLUDING excluded ones with the reason each was
   excluded. Report the excluded rows: hiding them makes the evidence look
   thinner than it is.
4. **Ranking** — `planar models evals --vendor … --role … --tier … --work-type …
   --complexity … --project … --validation-policy … --routing-policy …`
   ranks candidates in one exact cohort by the 95% Wilson lower bound over
   declared-experiment evidence. It is **read-only**. Under-sampled candidates
   report `insufficient_data`; candidates below the quality floor are excluded
   before any iteration or cost ordering; and **no recommendation is a valid
   outcome** meaning keep the configured default.
5. **Change a preset (operator-gated)** — presets live in the orchestration
   layer's `agents/models.md`, so guide the operator there. A `Use when` cell must rest
   on a product fact or recorded dispatch evidence; vendor capability claims,
   benchmark scores, and release ordering are not admissible.

## What It Must Not Do

- Do not ask Planar which model to use. It records the vendor and model an
  agent reports and validates neither.
- Do not present a `models evals` recommendation as applied. It writes nothing.
- Do not report a fallback tier without its reason, or infer a work type when
  the resolution says none was derived.
- Do not treat an unverified candidate as available, or an absent latency/cost
  metric as zero — unmeasured is not fast and free.

## Context

Report the resolved scope, which inspection mode was selected, and the cohort
dimensions in play when ranking.

## Intent

State in one sentence whether the operator wants to inspect registry state,
resolve a role, review evidence, or change a preset.

## Actions

Report `attempted`, `applied`, `skipped`, and `failed`. Every verb here is
read-only, so `applied` is zero unless the operator explicitly confirmed a
preset edit in `agents/models.md` — which this skill guides but does not perform.

## Result

Set `outcome=ok|partial|error`. For a resolution, return the tier AND its
source, with the fallback reason when applicable. For a ranking, return the
per-candidate rows (samples, successes, raw rate, Wilson lower bound,
gate-failure rate, excess iterations) plus which gate blocked any candidate,
and name the recommendation as a preview. For evidence, return recorded and
counted totals separately.

## Warnings

Name unverified/ineligible candidates, cohorts too thin to compare, excluded
samples with their reasons, and any metric reported as unmeasured. State
explicitly when a tier came from a static fallback rather than a packet.

## Next actions

Give zero to three executable commands, led by the exact inspection the
operator asked for. When a ranking yields a recommendation, the next action is
to ask whether to edit the `agents/models.md` preset — never to apply it unprompted.

## Recovery

Provide `planar models registry list --json`, `planar models resolve --json`,
or `planar models outcomes --json` to re-read current state. These verbs write
nothing, so there is no rollback to claim; if a ranking was blocked by a gate,
say which gate and that more evidence is the remedy.

## Vendor Notes

- Installed to `~/.claude/commands/pl-models-config.md`.
- Invoked as `/pl-models-config`.
- Presets live in `agents/models.md`; this skill reads Planar state and guides preset edits, never writing them.
