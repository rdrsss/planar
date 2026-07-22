---
description: Capture open questions during a session, answer them, and link to tasks and specs.
origin: docs/cli-reference.md#domain-question
shared_notes:
    - Resolved scope and question state come from the CLI; the skill must not read or write workspace context outside it.
slug: pl-question
vendor:
    claude:
        argument_hint: <add|answer|list|show|wontfix|link> [args]
        invocation_examples: |
            /pl-question add "What is the Stripe API rate limit?"
            /pl-question answer 3 "100 requests per second per endpoint"
            /pl-question list --status open
---

# Planar Question ({{.VendorTitle}})

Manages questions — open uncertainties surfaced during work.

## What It Does

Records open questions with their body text, answers them when resolved, marks them as won't-fix when no longer relevant, and links them to tasks, plans, and artifacts via `entity_links`. Questions flow through `open → answered / wontfix`.

## CLI Commands

Wraps [`question`](../../docs/cli-reference.md#domain-question):

> **Scope.** Reads use the cwd-derived scope; writes refuse on cross-scope mismatch (see [`docs/concepts.md#cross-scope-guard`](../../docs/concepts.md#cross-scope-guard)). Pass `--scope <slug>` explicitly when working from outside the target repo's cwd. There is no active scope stack and no `scope use` to push.

```
planar question add <title> [--body <text>] [--scope <scope>]
planar question answer <question-id> <answer>
planar question wontfix <question-id>
planar question list [--status <status>] [--scope <scope>]
planar question show <question-id>
planar question link <question-id> <to-kind:to-id> --relationship <kind>
```

## When To Invoke

During a session when an uncertainty blocks progress or a design decision needs a recorded rationale. Answered questions surface in resume packets to give resuming agents the current best-guess answers.

## Context

Report the resolved scope, question target or filter, requested operation, and
related entity when linking.

## Intent

State in one sentence whether the request records, inspects, answers, closes,
or links an uncertainty.

## Actions

Report `attempted`, `applied`, `skipped`, and `failed` counts for question rows
and links. Lists and shows have zero applied; an already-matching terminal
state or existing relationship is an expected skip when reported by the CLI.

## Result

Always report `outcome=ok|partial|error`. After add, answer, wontfix, or link,
read `planar question show <question-id> --json` and return the stable ID,
status, answer when present, and verified relationship. Empty lists and
idempotent operations remain explicit successful no-ops.

## Warnings

Name missing or ambiguous targets, invalid terminal transitions, scope
mismatch, unavailable post-state reads, and partial independent results. Do
not warn merely because a filtered list is empty.

## Next actions

Give zero to three executable recommendations tied to the verified question,
such as `planar question show <question-id> --json` or the exact answer/link
command that remains useful.

## Recovery

For each failure, provide `planar question show <question-id> --json` and the
exact idempotent retry with the original answer, scope, target, and
relationship arguments. Never claim a completed independent question write
was rolled back.

## Vendor Notes

See [cross-scope-writes.md](../../agents/cross-scope-writes.md) before any write outside the cwd-derived scope.
