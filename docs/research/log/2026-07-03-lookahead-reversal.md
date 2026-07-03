# Log: the derived-closure payoff reversed under look-ahead control (2026-07-03, same day)

*Written hours after [`2026-07-03-instrument-review.md`](2026-07-03-instrument-review.md),
whose meta-lesson — "suspiciously clean numbers are the strongest bug signal
there is" — struck again before the day was out.*

## What happened

Study 2 of [`../retrospective-studies.md`](../retrospective-studies.md)
initially reported the result the whole program hoped for: the derived closure
recovered **6 of the 8** files that author declarations missed (0.25→0.75,
0.58→1.00 on the under-declared tasks). It shipped with a disclosed caveat:
the closures had been computed against the corpus at **post-implementation**
state, so the number was an upper bound pending recomputation at the
pre-feature base SHAs.

The recomputation took an hour and **erased the entire effect**:

| | head-state (2a) | base-state (2b, honest) |
|---|---:|---:|
| derived-effective macro recall | 0.880 | **0.574** |
| declared misses recovered | 6/8 | **0/8** |
| walk's contribution beyond seeds | — | **+0 files, on all 9 tasks** |

Every one of the six "recoveries" was an edge that **the implementation itself
had created** — the migration referenced by the new code, the sibling verb that
gained a call, the registration line that was added. Compute the closure after
the work exists and static reachability "predicts" them; compute it at the
state a real prediction would be made from, and they are unreachable,
definitionally. (A secondary extractor gap surfaced too: seeds that don't
exist yet at base are silently dropped rather than retained as
planned-new-file members — fixable, but even with the fix the walk adds
nothing.)

A knock-on: the confirmatory campaign's **grouped arm consumed the head-state
closures** as its slicing input. With honest closures, slice composition
changes on 2 of 3 plans (mixed direction). Disclosed as an input-sensitivity
caveat on RQ2; the token win held across both compositions that ran.

## Why this is a better paper, not a worse one

The comfortable narrative ("static analysis fixes under-declaration") is dead,
and what replaces it is sharper:

1. **Under-declaration is structural, not sloppiness.** The missing footprint
   is *future-edge coupling* — files the change will newly connect to. Authors
   can't see it (Study 1: recall 0.56 on multi-file tasks) and pre-state
   static reachability can't either (+0 files, n=9). That is a genuine
   negative result with a mechanism.
2. **The look-ahead trap is itself a contribution.** Anyone who evaluates a
   footprint predictor against the repo state that already contains the
   implementation will "validate" it. We demonstrated the trap with the same
   extractor, same tasks, same ground truth: 0.88 vs 0.57. Quantified,
   reproducible, cautionary.
3. **The forward direction is now evidence-backed:** predictors that encode
   history and convention (co-change mining, registration/migration/CI
   convention detectors) rather than current-state reachability — scored
   against the 8-file miss table this study leaves behind.

## Lessons (appending to the campaign list)

8. **Any predictor evaluated retrospectively must be computed at the
   information state of a real prediction.** "What data existed when the
   forecast would have been made?" is the first question, not a caveat to
   append later. We wrote the caveat honestly — but nearly shipped the number.
9. **When a result is exactly what you hoped, re-derive it from the least
   favorable valid setup before believing it.** The 6/8 recovery survived one
   full write-up cycle because it was the desired answer.
10. **Disclosed caveats are promissory notes — pay them before the paper.**
    The gap between "flagged as upper bound" and "measured: the bound was the
    whole effect" was one hour of free compute. It could have been a
    camera-ready retraction instead.
