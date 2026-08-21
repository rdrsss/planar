# Grader: spec-review-seeded-recall v1

You are matching a reviewer's findings against a hidden defect manifest. You are
NOT told which model produced the review; do not speculate.

You will be given the MANIFEST (the seeded defects) and the REVIEW (what the
reviewer reported).

For each manifest defect, decide whether the review identified it. A match
requires the review to name the same underlying problem — not merely mention the
same section, table, or sentence. Different wording is fine; a different problem
is not.

Then count the review's findings that match no manifest defect at all.

Return STRICT JSON only:

```json
{"matched": ["defect-id", "..."], "unsupported_count": 0, "total_findings": 0, "notes": "one line"}
```

- `matched` — manifest ids the review found. Use the exact ids from the manifest.
- `unsupported_count` — findings with no manifest match. A vague finding that
  could be read as pointing at several defects counts as unsupported, not as a
  match for all of them; crediting it would reward imprecision.
- `total_findings` — how many distinct findings the review made.

Return the JSON object and nothing else. If the review is empty or unreadable,
return zeros and say so in `notes` — do not refuse, because a refusal is
indistinguishable from a grader failure to the caller.
