---
entity_kind: artifact
anchor_plan_id: {{.PlanID}}
title: "{{.Title}}"
artifact_kind: test_spec
status: draft
verifies: []
---

# {{.Title}}

Test strategy for plan {{.PlanID}} ({{.PlanSlug}}).

## Strategy

One-paragraph statement of what kind of testing this feature warrants. Why
this surface needs deliberate scenarios beyond what the coder would write
unprompted.

## Scenarios

Scenarios are grouped into four return-path buckets. Each bucket asks a
different question. A complete test surface answers all four where
applicable; gaps in any bucket are tracked in the Coverage gap checklist
below.

### Scenario: Happy path — <description>

The function returns a meaningful, expected result for valid input.

**Verifies:** TODO-REPLACE-ME  # required; cite task:<slug> from a [slug:] roadmap bullet
**Kind:** unit | integration
**Acceptance:** <observable result that proves "this works">

<prose>

### Scenario: Empty / null return — <description>

The function does its job *by* returning nothing meaningful: empty slice,
nil pointer, "not found" sentinel, zero count. This is a correctness path,
not an error path. Easy to skip; often hides the most subtle bugs
(conflating "no results" with "error").

**Verifies:** TODO-REPLACE-ME  # required; cite task:<slug> from a [slug:] roadmap bullet
**Kind:** unit | integration
**Acceptance:** the function returns the expected zero value with no error.

<prose>

### Scenario: Error return — <description>

The operation cannot proceed: invalid input, dependency unavailable,
constraint violated. The function returns a non-nil error (and a
zero-valued result, by Go convention).

**Verifies:** TODO-REPLACE-ME  # required; cite task:<slug> from a [slug:] roadmap bullet
**Kind:** unit | integration
**Acceptance:** the function returns an error of the expected class; the
result value is the zero value.

<prose>

### Scenario: Edge case — <description>

Boundary conditions: zero / one / max inputs, off-by-one, exact-equal,
concurrent access. Pick the boundaries that the implementation is most
likely to mishandle.

**Verifies:** TODO-REPLACE-ME  # required; cite task:<slug> from a [slug:] roadmap bullet
**Kind:** unit | integration
**Acceptance:** <observable result at the boundary>

<prose>

## Coverage gap checklist

For each public function, user-visible flow, or load-bearing invariant,
confirm coverage in each applicable bucket. Use this as the reviewer's
checklist for the test-spec.

- [ ] Happy path covered
- [ ] Empty / null return covered (or: N/A — function cannot legitimately return empty)
- [ ] Error return covered (or: N/A — function is infallible)
- [ ] Edge cases enumerated and covered

Gaps marked N/A must include a one-line justification in this section so
the reviewer can confirm the absence is deliberate.

## Test surface allocation

- **Unit tests** (`src/internal/...`): which packages and which invariants.
- **Integration tests** (`src/integration_tests/`): which user-visible flows.
- **Scenario rows** (`test_scenarios` table): which entities track verification status.

## Open questions

### <H3 title>

<prose>
