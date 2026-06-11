---
slug: pl-report-issue
description: "Assemble and post a GitHub issue from a feedback-plan finding and the diagnostic bundle; record the posted issue as an external link on the finding."
source: docs/cli-reference.md#domain-report
vendor:
  claude:
    argument_hint: "[--finding <kind:id>] [--days <n>] [--scope <scope>]"
    invocation_examples: |
      /pl-report-issue --finding question:42
      /pl-report-issue --finding task:17 --days 14
      /pl-report-issue
shared_notes:
  - "The preview gate is mandatory and unskippable. There is NO flag or path that bypasses it. A declined preview posts nothing and changes nothing."
  - "The diagnostic bundle is structurally redacted (counts, verb paths, categories, timestamps — never entity text). Finding text passes through the mandatory preview gate."
  - "The gh post uses the operator's existing gh auth. No adapter registration or token configuration is required."
  - "Linkback uses planar link (record-only external_links row, no propagation, no sync subscription). If no GitHub system is registered yet, register one first with planar ext register github."
---

# Planar Report Issue ({{.VendorTitle}})

Assemble a GitHub issue from a selected feedback-plan finding plus the
`planar report --json` diagnostic bundle, render the complete body for
operator review, require explicit confirmation, post via `gh issue create`,
and record the created issue as a record-only `external_links` row on the
originating finding.

## What It Does

1. Collects the diagnostic bundle via `planar report --json --days <n>`.
2. If a finding is selected (`--finding <kind:id>`), reads the finding's
   summary and its full `planar audit trail` history — verbs, timestamps,
   and entity kinds (not entity bodies).
3. Assembles the complete issue body in memory:
   - **Header:** a one-line problem statement from the operator (or the
     finding's title when `--finding` is supplied).
   - **Finding section** (when `--finding` is supplied): finding title,
     entity kind/id, summary, and audit trail output. This text is
     operator-reviewed before posting.
   - **Diagnostic bundle section:** the structurally-redacted
     `planar report --json` output as a fenced code block.
   - **Metadata footer:** Planar version, schema version, generation
     timestamp.
4. **Renders the entire assembled body to the operator for review.**
   This step is mandatory and cannot be skipped. There is no flag or
   path that bypasses it.
5. Asks for explicit confirmation. If the operator declines:
   - No `gh` invocation occurs.
   - No `external_links` row is written.
   - Local entity state is unchanged.
   - The skill exits cleanly. Re-running starts from step 1.
6. On confirmation, posts via:
   ```
   gh issue create -R rdrsss/planar --title "<title>" --body "<body>"
   ```
   The operator's existing `gh` auth is used — no adapter registration,
   no token configuration, no external-system propagation path.
7. If `gh issue create` fails (auth missing, network down, rate limit):
   - The error is surfaced to the operator verbatim.
   - No `external_links` row is written.
   - No finding status or links are changed.
   - Re-running after fixing the issue (e.g. `gh auth login`) is clean.
8. On success, records the created issue URL as a record-only
   `external_links` row on the originating finding — no propagation, no
   sync subscription — so `pl-audit-trail` and the audit surfaces connect
   local and upstream trails.

## Arguments

| Argument | Description | Default |
|----------|-------------|---------|
| `--finding <kind:id>` | Feedback-plan finding to embed: `question:<id>` or `task:<id>`. | (operator-supplied problem statement only) |
| `--days <n>` | Window for `planar report --json`. | 30 |
| `--scope <scope>` | Association scope for finding lookup. | cwd-derived |

## CLI Commands

Reads:

```
planar report --json --days <n>
planar question show --json <id>
planar task show --json <id>
planar audit trail <entity-id> --kind <kind>
```

Posts (only after operator confirmation):

```
gh issue create -R rdrsss/planar --title "<title>" --body "<body>"
```

Links (only on successful post, record-only — no propagation flag):

```
planar ext register github planar-upstream --project rdrsss/planar
planar link <kind:id> --to planar-upstream:<issue-number> --role reference --sync read-only --json
```

The `planar ext register github` step is a local-only row write — no network
contact. Run it once per database; if a `planar-upstream` system already
exists (`planar ext list`) skip registration. The `planar link` call without
`--propagate` writes a single `external_links` row and returns; no sync
subscription is created and no remote writes occur.

## Privacy

Two distinct guarantees apply. They must not be conflated:

- **Diagnostic bundle** — structurally safe by query construction.
  `planar report` selects only counts, verb paths, error categories,
  statuses, and timestamps. No entity titles, no argument values, no
  scope slugs, no body text. This guarantee holds with no human in the
  loop; the bundle can be included in the issue body without operator
  review of its individual fields.
- **Finding text** — the finding's title, summary, and audit trail may
  legitimately reference entity names. Its only privacy guarantee is the
  mandatory preview gate: the operator personally reviews every byte
  before posting. "Structurally redacted" does NOT apply to finding text.

The skill must never conflate these tiers: finding text is
operator-reviewed, the bundle is machine-safe.

## Mandatory Preview Gate

The preview gate is load-bearing. There is **no flag, environment variable,
or code path that skips it**. The full rendered body — including all finding
text, the redacted bundle, and the metadata footer — must be shown to the
operator before any post occurs. The gate exists because the bundle,
while structurally redacted, may be combined with finding text that
includes entity names the operator did not intend to share publicly.
An operator who declines the preview triggers no side effects; re-running
with a different finding or no finding is always an option.

## Linkback — Record-Only External Link

After a successful post, the skill records the created issue as an
`external_links` row on the originating finding:

```
planar link <kind:id> --to planar-upstream:<issue-number> \
  --role reference --sync read-only --json
```

`planar link` without `--propagate` writes one `external_links` row and
returns. No sync subscription is created. No remote writes occur. The
row's presence means:

- `pl-audit-trail` resolves the upstream link when given the finding's
  entity ref.
- `planar audit trail --link <link-id>` shows the cross-plane trail
  scoped to this external link.
- `planar unlink <link-id>` can remove it without touching GitHub.

The linkback step is intentionally separate from the post: a `gh` failure
before the issue URL is known leaves no partial row. The URL is recorded
only when the post succeeds.

## When To Invoke

- After `pl-introspect` has filed one or more findings on the feedback
  plan, when the operator wants to surface a specific finding to the
  upstream GitHub Issues tracker.
- When a friction pattern is concrete enough to make a useful bug report
  or feature request.
- Before invoking, confirm `gh auth status` is clean and the
  `planar-upstream` system is registered (or be prepared for the skill
  to guide you through registration).

## What It Does Not Do

- Does not run the introspection pass — use `pl-introspect` first to
  populate the feedback plan.
- Does not modify finding status (open, answered, wontfix) — triage
  is a separate step.
- Does not push any other entity or plan to GitHub Issues — this skill
  posts only the self-report issue to `rdrsss/planar`.
- Does not attach transcript excerpts — only the structurally-redacted
  bundle and the operator-reviewed finding text.
- Does not post without the operator seeing and confirming the full body.

## Vendor Notes

{{.VendorNotes}}
{{- if .InvocationBlock}}

## Invocation

```
{{.InvocationBlock -}}
```
{{- end}}
