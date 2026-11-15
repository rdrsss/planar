# Packets, reports, checks and reviews

The single definition of the five files that pass work between agents in the
execution loop: the worker packet, the worker report, the checks file, the
reviewer packet and the review. Implements PRD-R9 to PRD-R13, TS-8 (TS-8.1 to
TS-8.5 and TS-8.7), TS-19.7 and layout entry LAY-1.5.7. Follow it whenever you
write or read one of these files. Where the files live, who writes each and the
task record are in the project and task record references
(${CLAUDE_PLUGIN_ROOT}/references/project.md,
${CLAUDE_PLUGIN_ROOT}/references/task-record.md).

## Rules for all five files

- Each is a file in `tasks/<task-id>/` in the project directory, named
  `worker-packet-<n>.md`, `worker-report-<n>.md`, `checks-<n>.md`,
  `reviewer-packet-<n>.md` or `review-<n>-<focus>.md`, where `<n>` is the
  attempt number. The task record reference says how they are kept
  (${CLAUDE_PLUGIN_ROOT}/references/task-record.md).
- Each ends with the line(s) shown for it below, and nothing follows them. A
  file without them is incomplete.
- No packet or report contains an absolute path (TS-19.7). Name every file
  relative to the root of the working tree. This holds for text you copy in as
  well: in copied text, and in check output, write `<working tree>` wherever
  the absolute path of the working tree root appears and `~` wherever the
  user's home directory appears. That substitution is the only edit allowed to
  copied text.
- A packet is the complete input for one agent. The agent must not need to open
  any file to understand its task, so quote what it needs in full, never
  summarise it, and never say "see" a file.
- Quote a spec section exactly as the spec has it, with its ID. A cited ID that
  names a section with subsections is quoted with all of them.

## Dispatch

Give the agent exactly this, and no other detail of the task:

- the path of the working tree and the path of its packet, and an instruction
  to work in the working tree;
- for a reviewer, its focus (`spec-fit`, `correctness` or `all`; the profiles
  reference says what each judges,
  ${CLAUDE_PLUGIN_ROOT}/references/profiles.md).

Example: "Work in the working tree at `<working tree path>`. Your packet is
`<packet path>`. Read it first and follow it." Neither path is ever written
into a packet or report. An agent writes its own file (report or review) in the
directory that holds its packet.

## Worker packet

Written by the orchestrating actor as `worker-packet-<n>.md`. Sections, in this
order:

1. `# Worker packet: <task-id>, attempt <n>`.
2. `## Task`: the task's entry from the task list, copied exactly (the format
   is in ${CLAUDE_PLUGIN_ROOT}/references/tasks.md).
3. `## Paths you may create or change`: the `touches` paths, one bullet each,
   then `Base commit: <short hash>`, the commit the attempt starts from.
4. `## Cited spec sections, in full`: one `### <ID> (from <spec file name>)`
   per ID in the task's `cites`, then the section quoted in full.
5. `## Checks this task must pass`: one `### <ID>` per check, then the check
   copied from the testing spec in full, including its command, and its
   `Applies to` line if it has one. These are the task's `checks` plus the
   common checks that apply to the task's paths (the task list reference
   defines them).
6. When the profile has a plan stage, `## Plan`; when it has a map stage,
   `## Map`. Each holds the whole file, copied exactly, in a tilde fence (see below).
7. From attempt 2 only, `## Earlier reviews`: every review of this task from
   every earlier attempt, each as `### Attempt <n>, <focus>` and the whole
   review in a tilde fence. After a review that raised a question, add
   `Question answered:` and the answer, if one was recorded. When a question
   was answered, the answer is carried in full in every later worker packet
   and reviewer packet (TS-8.7). Omit the section on attempt 1.
8. The last line: `status: complete`.

Embed a diff or another Markdown file inside a fence of four tildes (`~~~~`),
which a three-backtick fence inside the content cannot close; use a longer run
of tildes if the content holds four. Do not add a rule, hint or instruction to a packet beyond what
this section lists; the worker's rules are in the next section.

Example (attempt 2, with a map and an earlier review):

`````markdown
# Worker packet: T-007, attempt 2

## Task

- [ ] **T-007** Show widget colour in the list
  - cites: PRD-R2, TS-4.1
  - touches: src/widgets/list.cpp
  - checks: TEST-C1
  - after: T-006

## Paths you may create or change

- src/widgets/list.cpp

Base commit: 4f2a9c1

## Cited spec sections, in full

### PRD-R2 (from prd.md)

- **PRD-R2 Colour shown.** The widget list shows each widget's colour name
  after its title.

### TS-4.1 (from tech-spec.md)

- **TS-4.1 List row.** A row is `<title> (<colour>)`; a widget with no colour
  shows `<title>`.

## Checks this task must pass

### TEST-C1

- **TEST-C1** The list shows colours. Run from the root of the working tree:

  ```sh
  ./build/list-test --filter colour
  ```

## Map

~~~~markdown
# Map: T-007, attempt 2

## Change

- `src/widgets/list.cpp:40`: `renderRow`, add the colour suffix.

## New code

None.

## Imitate

- `src/widgets/list.cpp:62`: how `renderBadge` formats optional text.

status: done
~~~~

## Earlier reviews

### Attempt 1, spec-fit

~~~~markdown
# Review: T-007, attempt 1, spec-fit

## Findings

- **F1** cites: TS-4.1. A widget with no colour shows `Title ()`; it must
  show `Title`.

verdict: request-changes
status: complete
~~~~

status: complete
`````

## Worker

The worker implements the task from its packet and does nothing else.

- Change only the `touches` paths, and within the map when there is one. Do not
  commit, stage or tick anything. Do not approve your own work.
- If the map is wrong or incomplete, stop, make no further change and do not
  explore; write the problem in the report and end it `status: map-wrong`
  (${CLAUDE_PLUGIN_ROOT}/references/plans.md).
- The checks are run independently, and your report is not a source for their
  results.
- Write `worker-report-<n>.md` last.

## Worker report

Written by the worker. Three sections, in this order, then the last line:

1. `# Worker report: <task-id>, attempt <n>`.
2. `## What I changed`: one bullet per path, with a few words on the change.
3. `## What I could not do`: anything in the task left undone, and why. Write
   "None." if nothing.
4. `## Context missing from the packet`: every point you had to infer, and every
   fact you needed that the packet did not give. Write "None." if nothing.

Last line: `status: done`, or `status: map-wrong` as above. Nothing else.

Example:

```markdown
# Worker report: T-007, attempt 2

## What I changed

- `src/widgets/list.cpp`: `renderRow` appends the colour, and omits the
  brackets when the widget has none.

## What I could not do

None.

## Context missing from the packet

None.

status: done
```

## Checks file

Written by the orchestrating actor, or by the `check-runner` agent in workflow
mode, as `checks-<n>.md`, after the worker is done and before any reviewer is
dispatched. Run every check of the task, the common ones included. Take nothing
from the worker's report.

- A command check: run it from the root of the working tree. Record its
  command exactly as run, its exit code and its full output, unsummarised.
  Nothing in the output is altered except the substitution in the rules above:
  `<working tree>` for the working tree root's absolute path and `~` for the
  user's home directory. It passed or failed as the check's own text says.
- A scenario check: carry it out as the testing spec describes. Record what
  you did and what you observed, and whether it passed.
- A check that is only a question for the reviewer (a `Question for the
  reviewer` row) is not run here; the reviewer packet carries it.

Sections, in this order: `# Checks: <task-id>, attempt <n>`; one `## <ID>` per
check; `Passed: <n>, failed: <n>`; the last line, `status: complete`. Under each
check, `Kind: command` or `Kind: scenario`, `Result: passed` or `Result:
failed`, then the record. Write the command as run from the working tree
root and never write that root's path.

Example:

~~~~markdown
# Checks: T-007, attempt 2

## TEST-C1

Kind: command
Result: passed
Command: `./build/list-test --filter colour`
Exit code: 0
Output:

```text
[==========] 2 tests ran.
[  PASSED  ] 2 tests.
```

## TEST-S2

Kind: scenario
Result: failed
Done: started the list with one widget that has no colour.
Observed: the row read `Title ()`.

Passed: 1, failed: 1

status: complete
~~~~

## Reviewer packet

Written by the orchestrating actor as `reviewer-packet-<n>.md`, only when every
check of the attempt passed. One packet serves every reviewer of the attempt.
Sections, in this order:

1. `# Reviewer packet: <task-id>, attempt <n>`.
2. `## Task`, `## Paths you may create or change` (with the base commit) and
   `## Cited spec sections, in full`: exactly as in the worker packet.
3. `## Change`: the task's change in full, in a tilde fence marked `diff`. The
   task's change is everything under the task's `touches` paths that differs
   from the attempt's base commit, new files included. Take it with
   `git add --intent-to-add <new files under the touches paths>`, so that a
   new file shows in the diff, then `git diff <base commit> -- <touches
   paths>`.
4. `## Unattributed changes`: every path that differs from the round's base
   commit and belongs to no task in the round (a round may hold several tasks
   coded at once, and each task's change is its own), by path with its diff.
   The same list goes in every reviewer packet of the round. Write "None." if
   there is none.
5. When the profile has a map stage, `## Map`: the whole file in a tilde
   fence, as in the worker packet.
6. `## Checks`: the whole `checks-<n>.md` in a tilde fence, then any `Question for
   the reviewer` rows of the task's checks, copied in full.
7. From attempt 2 only, `## Earlier reviews`: exactly as in the worker packet,
   so that a repeated finding can be recognised.
8. The last line: `status: complete`.

Example (attempt 2, with a map):

`````markdown
# Reviewer packet: T-007, attempt 2

## Task

- [ ] **T-007** Show widget colour in the list
  - cites: PRD-R2, TS-4.1
  - touches: src/widgets/list.cpp
  - checks: TEST-C1
  - after: T-006

## Paths you may create or change

- src/widgets/list.cpp

Base commit: 4f2a9c1

## Cited spec sections, in full

### PRD-R2 (from prd.md)

- **PRD-R2 Colour shown.** The widget list shows each widget's colour name
  after its title.

### TS-4.1 (from tech-spec.md)

- **TS-4.1 List row.** A row is `<title> (<colour>)`; a widget with no colour
  shows `<title>`.

## Change

~~~~diff
--- a/src/widgets/list.cpp
+++ b/src/widgets/list.cpp
@@ -40,3 +40,4 @@
-  return w.title;
+  return w.colour.empty() ? w.title : w.title + " (" + w.colour + ")";
~~~~

## Unattributed changes

None.

## Map

~~~~markdown
# Map: T-007, attempt 2

## Change

- `src/widgets/list.cpp:40`: `renderRow`, add the colour suffix.

## New code

None.

## Imitate

- `src/widgets/list.cpp:62`: how `renderBadge` formats optional text.

status: done
~~~~

## Checks

~~~~markdown
# Checks: T-007, attempt 2

## TEST-C1

Kind: command
Result: passed
Command: `./build/list-test --filter colour`
Exit code: 0
Output:

```text
[  PASSED  ] 2 tests.
```

Passed: 1, failed: 0

status: complete
~~~~

## Earlier reviews

### Attempt 1, spec-fit

~~~~markdown
# Review: T-007, attempt 1, spec-fit

## Findings

- **F1** cites: TS-4.1. A widget with no colour shows `Title ()`; it must
  show `Title`.

verdict: request-changes
status: complete
~~~~

status: complete
`````

## Reviewer

A reviewer judges one task's result, from its focus, against the cited spec
sections. It writes only `review-<n>-<focus>.md` and changes nothing else. It
does not fix what it finds.

- Check the change against the cited sections and, where the packet has a map,
  against the map.
- A change outside `touches`, a change outside the map or a failed check
  cannot be approved. Judge the task's own change (`## Change`) only.
- List every path in `## Unattributed changes` in your review, under its own
  heading (below), and do not withhold approval of an otherwise correct task
  because of one. The orchestrating actor withholds approval of every task in
  the round until it has dealt with the unattributed change: it records the
  path in each task's record
  (${CLAUDE_PLUGIN_ROOT}/references/task-record.md), then deletes the file if
  it is new or restores it from the round's base commit if it changed.
- Read every earlier review in the packet. Mark a finding that restates an
  earlier one.
- Never answer a question yourself. If the task cannot be judged without an
  answer, raise it.

## Review

Sections, in this order:

1. `# Review: <task-id>, attempt <n>, <focus>`.
2. `## Findings`: one bullet per finding, or "None." Each finding is
   `- **F<k>** cites: <ID>. <what is wrong, and what must change>`. IDs run
   `F1`, `F2`, ... within the review. Every finding cites a spec section ID or,
   for a map breach, the map (`cites: map`). Number the findings from most
   serious to least: a failed check, then a change outside `touches` or the
   map, then a spec requirement not met, then the rest.
3. If a finding restates one in an earlier review, whichever reviewer raised
   it, end it with `repeats: <attempt>/<focus>/<finding>`, naming the attempt,
   the reviewer's focus and the finding, for example `repeats: 1/all/F2`. The
   focus is part of the mark because each reviewer numbers from F1. A finding
   that restates none carries no mark. Marks are what stop a loop
   that keeps meeting the same finding
   (${CLAUDE_PLUGIN_ROOT}/references/profiles.md).
4. `## Unattributed changes`, only when the packet lists any: one bullet per
   path. They are reported here and are not findings, so they do not affect
   the verdict.
5. `## Question`, only when you raise one: the question, citing the section it
   is about, as `cites: <ID>`. It goes to the operator or the decider
   (${CLAUDE_PLUGIN_ROOT}/references/escalation.md), and a new attempt starts
   once it is answered.

The last two lines are the verdict and the status line, in that order:

- `verdict: approve`: nothing needs to change. No finding.
- `verdict: request-changes`: at least one finding.
- `verdict: open-question`: a `## Question` section.

then `status: complete`.

Example:

```markdown
# Review: T-007, attempt 2, spec-fit

## Findings

- **F1** cites: TS-4.1. A widget with no colour still shows `Title ()` in the
  empty-list path at `src/widgets/list.cpp:71`; it must show `Title`.
  repeats: 1/spec-fit/F1
- **F2** cites: map. The change adds `formatColour` at `src/widgets/list.cpp:12`,
  which the map does not name.

verdict: request-changes
status: complete
```
