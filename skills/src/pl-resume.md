---
slug: pl-resume
description: "Resume an in-flight task from zero conversational context, validate readiness first."
source: docs/cli-reference.md#domain-resume
vendor:
  claude:
    argument_hint: "[<task-id> | <plan-id>] [--budget <tokens>]"
    invocation_examples: |
      /pl-resume 42
      /pl-resume validate 42
      /pl-resume
shared_notes:
  - "Active scope and task state come from the CLI; the skill must not read or write workspace context outside it."
  - "On return, the session id and vendor are recorded on the snapshot."
---

# Planar Resume ({{.VendorTitle}})

Implements the from-zero resumption contract: a new agent process with no prior conversation history can resume any captured task using a single command.

## What It Does

Produces a structured resume packet containing the task identity, current status, exact next action, plan position, linked Jira/GitHub Issues state, recent session entries, decisions, open questions, and linked artifacts. Validates whether a task meets resumability criteria before producing the packet, and pulls the operational plane if state is stale.

## CLI Commands

Wraps [`resume`](../../docs/cli-reference.md#domain-resume):

```
planar resume [<task-id> | <plan-id>] [--budget <tokens>] [--no-pull]
planar resume validate <task-id>
```

## When To Invoke

At the start of any new agent session when picking up work from a prior session. Run `resume validate` before `resume` to confirm the task is capture-complete; failures include concrete remediation steps.

## Vendor Notes

{{.VendorNotes}}
{{- if .InvocationBlock}}

## Invocation

```
{{.InvocationBlock -}}
```
{{- end}}
