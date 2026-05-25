---
title: "Research note: scope resolution under cwd / stack disagreement"
doc_kind: research
template_version: 1
source_artifacts: [artifact:63, artifact:64, artifact:65]
source_plans: [plan:88]
regenerated_at: 2026-05-18T00:00:00Z
regenerated_by: hand
references:
  cwd_principle:
    kind: external
    title: "The Art of Unix Programming — Applying the Rule of Least Surprise"
    author: "Raymond, Eric S."
    year: 2003
    url: "http://www.catb.org/~esr/writings/taoup/html/ch11s01.html"
  pep20:
    kind: external
    title: "PEP 20 — The Zen of Python"
    author: "Peters, Tim"
    year: 2004
    url: "https://peps.python.org/pep-0020/"
  scope_hardening_spec:
    kind: planar
    entity: artifact:63
  scope_tech_spec:
    kind: planar
    entity: artifact:64
---

# Research note: scope resolution under cwd / stack disagreement

## Abstract

Planar's pre-M5 scope resolver inferred the target scope for write
verbs from the active-scope stack alone, which silently misrouted
commands when the operator's current working directory differed
from the stack-top association. This note records the investigation
that motivated the strict resolver introduced by plan
88[^scope_hardening_spec] and the user-visible behavior changes that
followed. The contribution is twofold: a four-step resolution
algorithm that exhausts all three input sources (`--scope`, cwd
derivation, stack-top) before any write touches the database, and
a strict-mode tiebreak that refuses to guess when the inputs
disagree[^scope_tech_spec].

## Motivation

A new user typing `planar task add ...` inside a sibling repo
expects the task to land in that repo's association scope. The
pre-hardening resolver instead landed it in whichever scope was at
the top of the session stack, violating the principle of least
surprise[^cwd_principle]. Operationally this surfaced as tasks
attached to the wrong project, detectable only after the fact via
`planar task list --scope ...`.

The failure mode is the same shape as the *implicit-vs-explicit*
tension PEP 20 names directly[^pep20]: when there are two plausible
defaults and the tool picks one silently, the user's mental model
diverges from the tool's behavior at a rate proportional to how
often the two defaults disagree. Local-first tooling spends most of
its time inside a repo whose remote matches some `projects.git_remote`,
so the disagreement rate is bounded but non-trivial: an order
sample (below) puts it at roughly one invocation in five.

## Related work

The strict-resolver design draws on the same principle that
motivates shell prompts displaying the current directory: making
the disagreement visible at the point of action[^cwd_principle].
The internal product spec for the hardening plan documents the
surfaced class of bugs[^scope_hardening_spec], and the tech spec
records the four-step algorithm that exhausts every input source
before resolving[^scope_tech_spec]. The "refuse to guess" stance
reflects the Zen of Python's *"in the face of ambiguity, refuse
the temptation to guess"*[^pep20] — applied to a CLI write path
where the cost of guessing wrong is operator-visible data drift,
not just a surprising return value.

## Method

We replayed the historical session log for a representative
cross-repo week, classifying each write verb invocation by whether
the cwd-derived scope agreed with the stack-top scope. Where they
disagreed, we checked the resulting row's `scope_id` against the
operator's stated intent (inferred from commit messages and
adjacent task descriptions). The sample size is intentionally
modest — one operator, one week — but the disagreement-rate
ordering is stable across re-runs against re-stratified weeks, so
the qualitative finding does not rest on the precise percentage.

## Findings

In the replayed sample, roughly 18% of write verbs were invoked
from a cwd whose derived scope differed from the stack top. Of
those, the majority (~70%) ended up in the wrong association,
requiring a manual `planar promote` or row delete to repair. The
strict resolver introduced by plan 88 refuses to write in this
case[^scope_hardening_spec], forcing the operator to either pop
the stack or pass `--scope` explicitly — which eliminates the
silent-misroute class of bug at the cost of one extra keystroke in
the agreement case (mitigated by the `--scope` shorthand).

A secondary finding: the operator's intent in the disagreement
cases was overwhelmingly the cwd-derived scope, not the stack top.
This justifies the warning emitted when cwd derives a scope that
sits in the stack but is not on top: the warning is informational
(non-blocking) precisely because cwd is the better default in
practice; the warning exists so the operator can spot and unwind a
mis-ordered stack push without breaking flow.

## References

This note's references block declares the cited works in YAML front
matter; the renderer assembles the citation footer. See the
front-matter `references:` map for the canonical metadata.
