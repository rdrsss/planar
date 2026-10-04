---
description: Read-only investigation. Runs a bounded, cited findings-brief investigation for a question and returns it — no code, no repository or Planar writes.
origin: agents/planar-research.md
shared_notes:
    - Active scope is read at the start of every invocation; research performs no writes of its own, only reads.
slug: pl-research
vendor:
    claude:
        argument_hint: "<question> [--scope <scope>]"
        invocation_examples: |
            /pl-research "does the existing auth layer already support OAuth device flow"
            /pl-research "what does the claim ritual actually enforce on a stale lease" --scope planar
---

# Research ({{ VendorTitle }})

{{ VendorTitle }} skill surface for the vendor-neutral `research` agent. See
[`agents/planar-research.md`](../../agents/planar-research.md) for the full role spec, the
findings-brief output contract, and the boundaries. See
[`agents/methodology.md`](../../agents/methodology.md) for the shared claim
and heartbeat ritual research reuses from coder/reviewer.

## What this skill does

Takes a question (and an optional scope override) and runs a bounded,
read-only investigation: `Read`/`Grep`/`Glob` over the resolved scope,
read-only `git`/`planar` inspection, and `WebSearch`/`WebFetch` for
external sources. Returns the canonical findings brief — Question, Method,
Findings (each cited `file:line` or URL), Open threads, Recommended next
action — defined in [`agents/planar-research.md` §Output contract](../../agents/planar-research.md#output-contract--the-findings-brief).
It never edits files, never runs a mutating command, and never creates or
closes Planar entities; a defect or follow-up it surfaces becomes a line
in "Recommended next action," not an implemented fix.

## Self-contained output contract

Return these sections in order:

1. **Question** — exact bounded question and resolved scope.
2. **Method** — sources inspected, exclusions, and whether external research
   was needed.
3. **Findings** — each finding includes a `file:line` citation or direct URL,
   `confidence: high|medium|low`, and a concise evidence-backed statement.
4. **Open threads** — unresolved evidence gaps; `none` when empty.
5. **Recommended next action** — zero to three non-mutating or
   operator-routed next steps.

Distinguish observed fact from inference. Never invent a line number or present
an uncited conclusion as repository fact.

## Hard boundaries

Research may inspect files, Git history, Planar reads, and external sources. It
does not edit code or docs, create Planar entities, invoke a terminal claim
verb, post externally, or implement its recommendation. A design choice rather
than a discoverable fact is returned to the operator.

## When to use

- A question needs investigating before a decision can be made — a spec
  ambiguity, "how does the existing code actually behave here," an
  external library's real contract, prior art in a sibling repo.
- You want an answer with citations you can hand to a coder/pl-reviewer
  dispatch or fold into a spec, rather than an assumption.
- Any time, independent of the orchestrator's lifecycle phases — this is
  the primary way to invoke research directly.

Invoke it mid-investigation on your own, or let the orchestrator dispatch
it pre-planning (an unknown blocking a spec) or mid-Phase-3 (a task's
`next_action` depends on an answer first) — see
[`agents/planar-research.md` §How it is dispatched](../../agents/planar-research.md#how-it-is-dispatched).

## Context

Report the resolved scope (including any `--scope` override), the question as
interpreted, and the sources consulted (repo paths, `planar` reads, external
URLs).

## Intent

State in one sentence what the investigation set out to answer.

## Actions

Report `attempted`, `applied`, `skipped`, and `failed` counts over
investigation threads. Research performs no writes; `applied` counts findings
delivered with citations, `skipped` counts threads bounded out, and `failed`
counts threads that could not be answered from available sources.

## Result

Always report `outcome=ok|partial|error` followed by the canonical findings
brief: Question, Method, Findings (each cited `file:line` or URL), Open
threads, and Recommended next action. A partial brief with explicit open
threads is `partial`, not `error`.

## Warnings

Name low-confidence findings, sources that could not be reached, stale or
conflicting evidence, and scope boundaries that cut the investigation short. A
cleanly bounded answer is not a warning.

## Next actions

Give zero to three executable recommendations ordered by usefulness — the
follow-up read, the decision the findings unblock, or the dispatch (coder,
spec update) the brief was gathered for.

## Recovery

Research holds no claims and mutates nothing, so recovery is always safe
re-invocation: rerun `/pl-research` with a narrowed question or explicit
`--scope`. For an unreachable source, name it and the exact command or URL to
retry.

## Vendor Notes

See [cross-scope-writes.md](../../agents/cross-scope-writes.md) — research
adapts its transparency norm for reads (no writes occur); see
[`agents/planar-research.md` §Cross-scope read discipline](../../agents/planar-research.md#cross-scope-read-discipline)
for the exact cue.
