# Profiles

The single definition of the profile format: what a profile file holds, how a
profile name is resolved to a file, how the file is validated, and what each
key means. Implements PRD-R27, PRD-R31 and PRD-R32, TS-17 (TS-17.1 to
TS-17.8), TS-4.6 and TS-23 (TS-23.8 and TS-23.13), and the reference layout
entry LAY-1.5.9. Follow it
whenever you read, write or check a profile.

## What a profile is

A profile is a YAML file that says how the execution loop is staged: which
stages run, which agent does each, on which model and reasoning effort, how
many reviewers judge a task, what limits stop the loop, and whether tasks are
coded one at a time or in parallel. It also says how the spec set is reviewed
before execution. Policies say what is allowed (see
`${CLAUDE_PLUGIN_ROOT}/references/settings.md`); a profile says how the work is
done. The drivers read the profile; nothing is generated from it.

## Resolving a profile to a file

The settings reference says which profile name is in force and what to do when
no profile has that name. This section defines how a name becomes a file:

1. Look for `<home>/profiles/<name>.yaml`. If it exists, use it.
2. Otherwise look for `<name>.yaml` in the plugin's own `profiles/`
   directory. If it exists, use it.
3. Otherwise no profile has that name.

A profile in `<home>/profiles/` therefore hides a shipped profile of the same
name. The operator adds a profile by putting a file in `<home>/profiles/`;
never create or edit one unasked. Use lowercase letters and hyphens in a
profile name. A name with any other character, such as a path separator, is
not a profile name.

## Validating a profile

`run` reads the profile before it does anything else and checks it. Refuse the
profile, and do not start the run, when any of these holds:

- It is not valid YAML, or its `version` is not `1`.
- It has a key not listed in "Keys" below, at any level.
- It has a stage not listed in "Stages" below.
- A key has a value of the wrong type or outside its allowed values.
- A required key or stage is missing.
- An `agent` names no agent. `agent: <name>` means the plugin agent
  `planar:<name>` when the plugin has one, and otherwise an agent of exactly
  that name available to the session (this is how a test agent stands in for a
  real one). If neither exists, the profile is refused.

When you refuse a profile, tell the operator which key, stage or agent is
wrong and where, and stop. Do not repair the file, do not substitute another
profile, and do not run with part of it.

## Example

```yaml
version: 1
name: basic
description: Orchestrator, planner, symbol maps, tiered workers, two reviewers.
driver: orchestrator
driver_model: sonnet
parallel: false
stages:
  plan:
    agent: planner
    model: opus
    effort: high
  map:
    agent: mapper
    model: sonnet
  implement:
    agent: worker
    ladder:
      - { model: haiku, effort: high }
      - { model: sonnet, effort: medium }
  check: {}
  review:
    reviewers:
      - { agent: reviewer, focus: spec-fit, model: opus }
      - { agent: reviewer, focus: correctness, model: opus }
limits:
  revisions: none
  stall: 2
decider: { agent: decider, model: opus, effort: high }
spec_review:
  reviewers:
    - { agent: spec-reviewer, focus: intent, model: opus }
    - { agent: spec-reviewer, focus: consistency, model: opus }
    - { agent: spec-reviewer, focus: feasibility, model: opus }
```

## Keys

### Top level

| Key | Type | Allowed values | Default | Meaning |
| --- | --- | --- | --- | --- |
| `version` | integer | `1` | required | Profile format version. |
| `name` | string | a profile name | required | The profile's name; use the file's name without `.yaml`. A file is found by its file name, so a `name` that differs is not refused; it is still the name written to each task record. |
| `description` | string | any text | none | One line saying what the profile does. |
| `driver` | string | `session`, `orchestrator` | required | Who does the loop's bookkeeping; see "Driver". |
| `driver_model` | string | a model | the dispatcher's model | The model the `orchestrator` agent is dispatched on. Used only when `driver` is `orchestrator`. |
| `parallel` | boolean | `true`, `false` | `false` | `false`: one task at a time. `true`: every ready task that touches different files is coded at the same time, then checked and reviewed. The loop reference defines how the tasks are chosen and run. |
| `stages` | map | the stages below | required | The stages of the loop; see "Stages". |
| `limits` | map | `revisions`, `stall` | `revisions: none`, `stall: 2` | The caps that stop the loop; see "Limits". A missing key in it takes its default. |
| `decider` | map | agent, model, effort | none | The agent that takes the operator's decisions in an autonomous run; see "Decider". |
| `spec_review` | map | `reviewers` | none | How the spec set is reviewed before execution; see "Spec review". |

### Stages

`stages` is a map. The stage names are `plan`, `map`, `implement`, `check` and
`review`; no other name is allowed.

| Stage | Required | Keys | Meaning |
| --- | --- | --- | --- |
| `plan` | no | `agent`, `model`, `effort` | When present, a planner writes a plan for the tasks before any coding. Absent: no plan, and every task starts on the first rung. The plans reference defines what the stage produces. |
| `map` | no | `agent`, `model`, `effort` | When present, a mapper writes a symbol map before each task is coded. The plans reference defines what the stage produces. |
| `implement` | yes | `agent`, `model`, `effort`, `ladder` | The worker stage. See "Ladder". |
| `check` | yes | none | Always runs. Write `check: {}`. |
| `review` | yes | `reviewers` | The review stage. See "Reviewers". |

`agent`, `model` and `effort` mean the same in every stage and in every
reviewer, rung and the decider:

| Key | Type | Allowed values | Default | Meaning |
| --- | --- | --- | --- | --- |
| `agent` | string | an agent name (see "Validating a profile") | required in `plan`, `map`, `implement`, each reviewer and the decider | The agent dispatched for the stage. |
| `model` | string | a model name the dispatch accepts, for example `haiku`, `sonnet`, `opus` | the dispatcher's model | Passed on each dispatch. |
| `effort` | string | a reasoning-effort level the dispatch accepts, for example `medium`, `high` | the dispatcher's effort | Passed on each dispatch. |

Pass the resulting `model` and `effort` on every dispatch of the agent.

### Ladder

`implement.ladder` is a list of rungs ordered from the weakest to the
strongest. A rung has the keys `model` and `effort` only; the stage's `agent`
does every rung. Without a `ladder`, the stage's own `model` and `effort`
are the one rung.

Rungs are numbered from 1 in list order, and rung 1 is the weakest. A plan and
a task record refer to a rung by that number; without a `ladder` the one rung
is rung 1.

- A task starts on the rung its plan assigns, or on the first rung when there
  is no plan.
- Each `request-changes` round moves the task up one rung.
- On the top rung the task stays there.

### Reviewers

`review.reviewers` is a list with at least one reviewer. Each reviewer has the
keys `agent`, `focus`, `model` and `effort`, and `focus` is required.

Dispatch each reviewer separately, with its `focus`. The focus selects the
section of the reviewer's instructions that applies:

| Focus | The reviewer judges |
| --- | --- |
| `spec-fit` | Whether the change does what the cited spec sections say. |
| `correctness` | Whether the change is right, and within the symbol map. |
| `all` | Both of the above. |

The result of a round is the first of these that applies: `request-changes` if
any reviewer says so, else `open-question` if any reviewer says so, else
`approve`. A request for changes is not outvoted.

### Limits

| Key | Type | Allowed values | Default | Meaning |
| --- | --- | --- | --- | --- |
| `limits.revisions` | integer or string | a whole number, or `none` | `none` | The most `request-changes` rounds one task may have. `none` means no cap. |
| `limits.stall` | integer | a whole number | `2` | Stops an uncapped loop that keeps meeting the same finding. |

Stall works like this. Every reviewer is shown all earlier reviews of the
task and must mark any finding that restates an earlier one, whichever
reviewer raised it. When one finding has been raised on `stall` consecutive
attempts, dispatch no further attempt. With the default of 2, a finding
raised on attempt 1 and repeated on attempt 2 stops the loop before attempt
3. The packets reference defines how reviews are shown and the `repeats:`
mark (`${CLAUDE_PLUGIN_ROOT}/references/packets.md`, "Review"). A note-level
finding asks for no change, carries no `repeats:` mark and is ignored by the
stall guard (TS-23.13). An attempt
that ended in a map-wrong restart is ignored when counting consecutive
attempts: it neither adds to the chain nor breaks it (TS-23.8).

A task also has at most two map-wrong restarts. That number is fixed, not a
profile key; the loop reference defines a map-wrong restart.

Reaching `revisions`, `stall` or the restart cap sets the task to `blocked`
and escalates as the escalation reference
(`${CLAUDE_PLUGIN_ROOT}/references/escalation.md`) says.

### Driver

- `session`: the session the operator is talking to does the bookkeeping
  itself.
- `orchestrator`: the session dispatches the `orchestrator` agent, on
  `driver_model`, to do it.

Either way the bookkeeping rules are the ones in the loop reference.

### Decider

`decider` takes `agent`, `model` and `effort`. It names the agent that takes
the operator's decisions when autonomy is on. The escalation reference
(`${CLAUDE_PLUGIN_ROOT}/references/escalation.md`) defines what it may and may
not decide and how it records decisions.

### Spec review

`spec_review.reviewers` is a list of spec reviewers for the review before the
operator is asked to approve a spec set. Each has the keys `agent`, `focus`,
`model` and `effort`, and `focus` is required. The focus is one of:

| Focus | The reviewer looks for |
| --- | --- |
| `intent` | Each thing the operator asked for, matched to the sections that carry it out, or reported as a gap; anything specified that nobody asked for; questions that should have been asked. |
| `consistency` | Places where the specs disagree, requirements nothing implements, parts no task builds, checks that could not fail. |
| `feasibility` | Steps that cannot be done as written, and claims about the platform or codebase that need trying first. |
| `all` | All three. |

Dispatch each spec reviewer separately. The spec review reference defines the
rounds, the reports and how the verdicts combine.

## Shipped profiles

| Profile | Driver | Stages | Parallel |
| --- | --- | --- | --- |
| `minimal` | `session` | `implement` with one rung on the session's model, `check`, one reviewer with focus `all`; one spec reviewer with focus `all` | no |
| `basic` | `orchestrator` on `sonnet` | `plan`, `map`, `implement` with a ladder, `check`, two reviewers (`spec-fit`, `correctness`); three spec reviewers (`intent`, `consistency`, `feasibility`) | no |
| `basic-parallel` | `orchestrator` on `sonnet` | as `basic` | yes |

`minimal` is the shipped default; the settings reference says when it applies.

## Recording

Write the profile's `name` to each task record. Each log line carries the
model and effort the orchestrating actor dispatched with, so runs under
different profiles can be compared.
