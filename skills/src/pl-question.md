---
slug: pl-question
description: "Capture open questions during a session, answer them, and link to tasks and specs."
source: docs/cli-reference.md#domain-question
vendor:
  claude:
    argument_hint: "<add|answer|list|show|wontfix|link> [args]"
    invocation_examples: |
      /pl-question add "What is the Stripe API rate limit?"
      /pl-question answer 3 "100 requests per second per endpoint"
      /pl-question list --status open
shared_notes:
  - "Resolved scope and question state come from the CLI; the skill must not read or write workspace context outside it."
---

# Planar Question ({{.VendorTitle}})

Manages questions — open uncertainties surfaced during work.

## What It Does

Records open questions with their body text, answers them when resolved, marks them as won't-fix when no longer relevant, and links them to tasks, plans, and artifacts via `entity_links`. Questions flow through `open → answered / wontfix`.

## CLI Commands

Wraps [`question`](../../docs/cli-reference.md#domain-question):

> **Scope.** Reads use the cwd-derived scope; writes refuse on cross-scope mismatch (see [`docs/concepts.md#cross-scope-guard`](../../docs/concepts.md#cross-scope-guard)). Pass `--scope <slug>` explicitly when working from outside the target repo's cwd. The active scope stack was removed in plan 153 M5; there is no `scope use` to push.

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

## Vendor Notes

{{.VendorNotes}}
{{- if .InvocationBlock}}

## Invocation

```
{{.InvocationBlock -}}
```
{{- end}}
