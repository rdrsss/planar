# Task list

The single definition of the task list file, `tasks.md`: the entry format, who
may change it, and how the next task is chosen. Implements PRD-R14 and TS-6
(TS-6.1 to TS-6.4) and the traceability rules of TS-5.5 that concern the task
list. Follow it whenever you write, edit, check or read a task list.

## What the file is

`tasks.md` declares what each task is. The only state it holds is one
checkbox per task, showing whether the task is complete. It is a declaration
of the work, not a log of it: progress, attempts and review results belong in
the task's own record, never here.

## Entry format

Write one entry per task, in the order the work should be done:

```markdown
- [ ] **T-003** Write the settings reference
  - cites: PRD-R22, TS-4
  - touches: references/settings.md
  - checks: TEST-C6, TEST-S6
  - after: T-001
```

- The first line is `- [ ] **T-<nnn>** <title>`. The ID is `T-` plus three
  digits, unique in the project and never reused. The title is one line
  saying what the task produces.
- The four fields below it are indented two spaces, appear in the order shown,
  and each holds a comma-separated list on its line.
- `cites`: the spec section IDs the worker and the reviewers are given for
  this task. Every task has at least one.
- `touches`: the paths the worker may create or change. A trailing `/` means
  everything under that directory (`skills/plan/`). A worker changes nothing
  else.
- `checks`: the IDs of testing-spec checks that must pass for this task.
- `after`: the IDs of tasks that must be ticked before this one, or `none`.
  Never put a request for the operator here: anything the task needs from the
  operator is an open question in the spec, which is answered there.

## The checkbox

- `- [ ]` is not complete; `- [x]` is complete.
- Only the actor driving the loop (the orchestrator, or the session under the
  minimal profile) ticks a task, and only when the task is approved. A worker
  never ticks.
- Ticking changes the box and nothing else in the entry.

## Next task

A task is ready when it is unticked and every task in its `after` list is
ticked (a task with `after: none` is ready whenever it is unticked). The next
task is the first ready task in file order.

## Lists the checker reads

`tasks.md` may end with two sections, in exactly this form, after the last
task:

```markdown
## Checks common to all tasks

- TEST-C7: all tasks
- TEST-C6: tasks touching references/*.md, agents/*.md

## Requirements no task builds

- PRD-R9: the operator supplies this at run time, so no file implements it
```

- `## Checks common to all tasks`: one line per check, `- <check ID>: all
  tasks` or `- <check ID>: tasks touching <path patterns>`, with several
  patterns separated by commas. These checks run for the matching tasks
  without being repeated in each task's `checks`.
- `## Requirements no task builds`: one line per requirement,
  `- <requirement ID>: <reason>`. The reason is required and says why no task
  builds the requirement.

## Traceability rules for the task list

The spec checker applies these mechanically, and any failure fails the check:

- Every requirement (`PRD-R<n>`) appears in the `cites` of some task, or is
  listed under "Requirements no task builds" with a reason.
- Every task has at least one entry in `cites`.
- Every check defined in the testing spec appears in some task's `checks` or
  in the list of checks common to all tasks.

The rule that every requirement has a row in the testing spec's coverage
section is checked against the testing spec; the spec review reference
defines it.
