# Research log

An **append-only lab notebook** for the research program: dated entries recording
what actually happened — the struggles, dead ends, instrument bugs, invalidated
measurements, and course corrections — not just the polished outcomes.

## Why this exists

The polished docs (`../results.md`, `../closure-measurement-report.md`) tell the
story after the fact. This log preserves the *process*: what broke, how it was
found, what it cost, and what we'd do differently. Three uses:

1. **Honesty infrastructure.** A pre-registered experiment earns its credibility
   from what it admits. The log is where "the instrument was wrong and here's how
   we found out" lives in full detail.
2. **The paper's threats-to-validity section**, pre-written. Most entries here map
   directly to limitations that must be disclosed.
3. **Institutional memory.** The same classes of failure (silent measurement
   no-ops, structural artifacts masquerading as findings, circularity introduced
   by our own fixes) will recur in future experiments. Look here first.

## Conventions

- One file per episode: `YYYY-MM-DD-<slug>.md` (date = when the entry is written,
  which may postdate the events it describes).
- **Append-only**: entries are historical record; don't rewrite them when
  understanding improves — write a new entry and cross-link.
- Candid register. Internal IDs (plans, tasks, PRs) welcome; explain on first use
  when practical.

## Entries

| date | entry | one-liner |
|---|---|---|
| 2026-07-03 | [`2026-06-closure-campaign.md`](2026-06-closure-campaign.md) | The confirmatory-run saga: a $165 silent measurement failure, six layered instrument bugs, API-limit perturbation, and the self-healing arc. |
| 2026-07-03 | [`2026-07-03-instrument-review.md`](2026-07-03-instrument-review.md) | Post-hoc critical review: M-WALL is a structural artifact; live RQ1 is contaminated in both directions; the pooled H1 verdict was wrong; what survives. |
