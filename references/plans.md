# Plans and symbol maps

The single definition of the two files that constrain a worker: the plan
(`plan.md`) and the symbol map (`map-<n>.md`). Implements PRD-R29 and PRD-R30,
TS-18 (TS-18.1 to TS-18.4) and the reference layout entry LAY-1.5.10. Follow it
whenever you write, refresh or read a plan or a map.

Both files live in the task's own directory, `tasks/<task-id>/`, in the project
directory. Both are constraints, not hints: the worker's packet carries each whole, copied
exactly, under `## Plan` and `## Map` (see
${CLAUDE_PLUGIN_ROOT}/references/packets.md), and the reviewer checks the change
against them.

## Plan

The planner writes `tasks/<task-id>/plan.md` for each task. It runs only when
the profile has a `plan` stage, once, after the spec set is approved and before
any worker. Plan every task you can, as far ahead as is useful. A task you
cannot plan yet gets a plan that says why (see the status line below), and you
plan it again when it becomes ready.

### Contents

A plan has these parts, in this order:

1. The heading `# Plan: <task-id> <task title>`.
2. A line `commit: <full hash>`: the commit of the project's worktree the plan
   was written against. Read the code as it is at that commit.
3. `## Steps`: a numbered list of the actions the worker takes, each small
   enough to check.
4. `## Files and symbols`: one bullet per file the task changes or reads, with
   the symbols involved. Name every file the steps depend on; the refresh rule
   below is driven by this list. Every file the plan changes or creates is
   within the task's `touches`; a file named only to be read, for example code
   to imitate, may be anywhere in the working tree and is read-only for the
   worker.
5. `## Starting rung`: one line, `Rung <n>: <reason>`. `<n>` is the number of
   the rung the worker starts on in the profile's `implement` ladder. Rungs are
   numbered from 1 in list order, rung 1 is the weakest, and a profile without
   a ladder has the one rung, 1 (${CLAUDE_PLUGIN_ROOT}/references/profiles.md).
   Choose the lowest rung you judge able to do the task. The reason is one
   sentence.
6. Any refresh sections (see below).
7. The status line, always last.

### The status line

The last line of `plan.md` is `status: <value>`. Nothing follows it. A step
file with no such last line is incomplete
(${CLAUDE_PLUGIN_ROOT}/references/task-record.md).

- `status: ready`: the task can be implemented as specified and the plan is
  complete.
- `status: not-ready`: the task cannot be planned yet, for example because it
  depends on code an earlier task has not written. Say why under `## Steps` in
  place of steps. Plan the task again when it becomes ready, and replace the
  status.
- `status: question`: you found the task cannot be implemented as specified,
  for example the specs contradict each other or the task asks for something
  the code makes impossible. Write the problem and what must be decided under
  `## Steps` in place of steps. The task is not dispatched, and the question
  goes to the operator or the decider as the escalation reference describes
  (${CLAUDE_PLUGIN_ROOT}/references/escalation.md). Do not answer it yourself
  and do not write code. Raise it before any coding starts.

### Refresh

When a task first starts, check whether the plan is still current. Run
`git diff --name-only <commit>` in the worktree, with the commit from the plan's
`commit:` line. If none of the files named under `## Files and symbols` appears
in the output, leave the plan as it is. If any does, refresh it:

- Read the changed files, then add a section `## Refresh <YYYY-MM-DD>` before
  the status line. It says which named files changed and gives the corrected
  steps, files and symbols, and starting rung (as `Rung <n>: <reason>`), only
  for what is now different.
- Update the `commit:` line to the current commit.
- Keep all earlier text. Never edit or delete it.
- If the refresh shows the task can no longer be implemented as specified, set
  the status line to `status: question` as above. Otherwise leave it as it was,
  or set it to `status: ready`.

Refresh only at the first start of the task, not on each attempt.

### Example

```markdown
# Plan: T-003 Write the settings reference

commit: c41e7b09d2a85f36e0b1c7d94a2f8e65b3d01a7c

## Steps

1. Create `references/settings.md` with sections for each settings key.
2. State each key's default and allowed values in the same section.
3. Add the example file to the list of examples at the end.

## Files and symbols

- `references/settings.md`: new file; imitate the layout of `references/tasks.md`.
- `references/conventions.md`: read only, for the ID and citation forms.

## Starting rung

Rung 1: the task is one prose file with a quoted source.

## Refresh 2026-11-16

`references/conventions.md` changed since `7db79f7`: the citation form for
operator decisions now needs a date. Step 2 now cites decisions by date.

status: ready
```

## Symbol map

The mapper writes `tasks/<task-id>/map-<n>.md`. It runs only when the profile
has a `map` stage, at the start of each round, once for each task in the round.
`<n>` is the number of the attempt the map is for. The task record reference
defines how attempts are numbered.

Read the code as it is at that moment, not as the plan saw it. Every path the
map names to change or create is within the task's `touches` (a trailing `/`
covers everything under that directory); never name a path outside it to be
changed or created. Code to imitate may be anywhere in the working tree and is
read-only for the worker.

### Contents

A map has these parts, in this order:

1. The heading `# Map: <task-id>, attempt <n>`.
2. `## Change`: one bullet per existing file to change, with each symbol
   (function, type, section heading) and its location as `path:line`, and a few
   words on what changes in it. A new file is not listed here. Write "None." if
   the task only creates files. Every path is within `touches`.
3. `## New code`: where each new file, symbol or section goes, as the file and
   the symbol or heading it follows. Write "None." if the task adds nothing.
   Every path is within `touches`.
4. `## Imitate`: the neighbouring code to copy the style and structure of, as
   `path:line`, with a few words on what to copy. These paths may be anywhere
   in the working tree, inside `touches` or not, and are read-only: the worker
   reads them and does not change them.
5. The status line, always last: `status: done`.

Be exact and short. A line number and a symbol name are enough; do not
paste code.

### Example

```markdown
# Map: T-003, attempt 1

## Change

None.

## New code

- `references/settings.md`: a heading per key, after the intro paragraph.

## Imitate

- `references/tasks.md:10`: how an entry format is shown with a fenced example.
- `references/conventions.md:1`: the opening paragraph that names the specs
  implemented.

status: done
```

## The map binds the worker

The worker changes only what the map names, and keeps to the plan. If the map
is wrong or incomplete, for example a named symbol is not where the map says, or
the task needs a change the map does not name, the worker:

1. stops and makes no other change;
2. does not explore the code to find its own way;
3. writes what is wrong in its report, ending with `status: map-wrong`.

The mapper then runs again for that task with the worker's report and writes
`map-<n+1>.md`. This is a new attempt number but not a revision round. After two
such restarts the task is `blocked`; see the escalation reference
(${CLAUDE_PLUGIN_ROOT}/references/escalation.md) for what happens then.
