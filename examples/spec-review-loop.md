# Example: Flesh Out Draft Specs Before Ingestion

Use this when specs already exist but feel thin, ambiguous, or mismatched with
the user's actual goal.

## 1. Pull Current Workbench State

If the operator has edited files on disk, pull those edits into the database
before review:

```text
/pl-workbench pull <plan-id>
```

## 2. Run The Review

```text
/pl-spec-review <plan-id>
```

Read the verdict first:

- `ready-for-ingest`: proceed to ingest preview.
- `needs-answers`: ask the operator the listed questions.
- `needs-spec-work`: the answers are known, but artifacts need edits.
- `abort-replan`: the drafts describe the wrong feature; restart planning.

## 3. Answer Blocking Questions

For direct CLI updates:

```text
planar question list --plan <plan-id> --status open --json
planar question answer <question-id> --answer "<operator-approved answer>"
```

For spec-aware updates, prefer the skill write pass:

```text
/pl-spec-review <plan-id> --write
```

Use write mode when the answer must also be reflected in `product-spec.md`,
`tech-spec.md`, `roadmap.md`, or `test-spec.md`.

## 4. Close Feature Gaps

Typical edits after review:

- Add a missing acceptance signal to `product-spec.md`.
- Add an architecture decision to `tech-spec.md`.
- Split a vague roadmap bullet into implementable work.
- Add `[slug: ...]` to each testable roadmap bullet.
- Add scenarios for happy, empty/null, error, and edge paths.
- Mark a non-goal explicitly instead of leaving it implied.

After edits:

```text
/pl-workbench push <plan-id>
/pl-spec-review <plan-id>
```

Repeat until the verdict is `ready-for-ingest` or until remaining risk is an
explicit operator-approved assumption.

## 5. Confirm Mechanical Readiness

```text
/pl-spec-ingest <plan-id> --strict
planar test-spec status <plan-id> --json
```

Both commands are read-only in this form. Use their output as the final check
before apply.

