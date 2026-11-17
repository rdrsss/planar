# Clean work/review case

This case uses the historical `revision-fault` fixture name for straight-through work and independent review without fault injection. It uses two fresh driver sessions, one worker child and one reviewer child. Expected actor counts are graded conditions, not runtime child caps.

The native worker step must pass its separate verifier before review can start. The host confirms the stopped worker boundary, compares its saved snapshot and current workspace, and records a `mode=none` receipt with equal product digests; it does not inject a fault. The trusted second instruction contains the factual prior-driver-and-children stop confirmation only after those host checks. The runtime collects initial, worker pre, no-fault post and final snapshots at actual quiescent boundaries.

There is no host snapshot immediately before reviewer dispatch. The check and linked reviewer observations are cooperative evidence; the full TS-8.4 packet, its prior existence and actual child delivery require independently bound raw review. The structural-positive review verifier returns incomplete/reward 0 pending that audit. Unknown native forms remain incomplete. A genuine request-changes is not suppressed to make the control pass.

The stronger pre-dispatch host-snapshot requirement in the original design is historical and is superseded by the separately reviewed native-boundary amendment. No test, case source or structural result itself approves a live run; a fresh frozen plan and independent one-use gate remain required. The opt-in native noauth control uses synthetic A data and deliberately omits B review/approval; it proves host integration and failure gating, not a model workflow pass.
