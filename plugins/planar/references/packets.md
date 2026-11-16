# Packets, reports, checks and reviews

The single definition of the five files that pass work between agents in the
execution loop: the worker packet, the worker report, the checks file, the
reviewer packet and the review. Implements PRD-R9 to PRD-R13, TS-8 (TS-8.1 to
TS-8.5, TS-8.7, TS-8.8, TS-8.11 and TS-8.12), TS-19.5, TS-19.7, TS-19.8, TS-23
and layout entry LAY-1.5.7. Follow it whenever you write or read one of these
files. Where the files live, who writes each and the
task record are in the project and task record references
(${CLAUDE_PLUGIN_ROOT}/references/project.md,
${CLAUDE_PLUGIN_ROOT}/references/task-record.md).

## Rules for all five files

- Each is a file in `tasks/<task-id>/` in the project directory, named
  `worker-packet-<n>.md`, `worker-report-<n>.md`, `checks-<n>.md`,
  `reviewer-packet-<n>.md` or `review-<n>-<focus>.md`, where `<n>` is the
  attempt number. A malformed completed packet may have a replacement named
  `worker-packet-<n>-replacement-<k>.md` or
  `reviewer-packet-<n>-replacement-<k>.md`, as specified below. The second
  run of a failed check is a second checks file,
  `checks-<n>-rerun.md` (see "Checks file"). Pending batches use
  `checks-<n>-pending-<k>.md` or `checks-<n>-rerun-pending-<k>.md`.
  The task record reference says
  how they are kept
  (${CLAUDE_PLUGIN_ROOT}/references/task-record.md).
- Each ends with the line(s) shown for it below, and nothing follows them. A
  file without them is incomplete.
- No packet or report contains an absolute path (TS-19.7). Name every file
  relative to the root of the working tree. This holds for text you copy in as
  well: in copied text, and in check output, write `<working tree>` wherever
  the absolute path of the working tree root appears and `~` wherever the
  user's home directory appears. Make the two substitutions in that order,
  the working tree first and then the home directory, because the working
  tree may lie under the home directory. That substitution is the only edit
  allowed to copied text.
- A packet is the complete input for one agent. The agent must not need to open
  any file to understand its task, so quote what it needs in full, never
  summarise it, and never say "see" a file.
- Quote a spec section exactly as the spec has it, with its ID. A cited ID that
  names a section with subsections is quoted with all of them.

## Resolved configuration evidence

Before dispatch, follow `${CLAUDE_PLUGIN_ROOT}/references/profiles.md` to
freeze the source path and digest, schema version, actual host and selected
role bindings with source paths. Include these in packets, results and session
audit observations. For Codex also record configured and observed model and
effort separately as `${CLAUDE_PLUGIN_ROOT}/references/hosts.md` requires;
never revise a packet with observations obtained later.

Each worker and reviewer packet has a `## Resolved configuration` section,
containing the frozen profile source path and digest, schema version, actual
host, and each selected role binding and its source path. For Codex include
`host: codex`, `configured_model`, `configured_effort`, `observed_model`,
`observed_effort` and `observation_source` for each recipient; use
`unavailable` for observations not available at creation. Apply the path
substitutions above. Before writing `status: complete`, validate that this
section and every other required section are present and accurate against the
frozen configuration and durable inputs. Do not mark a packet complete if any
required input is missing or cannot be verified; stop at that boundary and
report the blocker. Missing runtime observations alone do not block dispatch.

## Replacing a malformed completed packet

A final status line does not make a packet with missing or invalid required
content safe to dispatch. Preserve it unchanged. If the durable inputs suffice
to reconstruct a valid packet for the same task and attempt, write a new
immutable replacement with the appropriate name above, starting `k` at 1 and
increasing it for each replacement of that packet kind in the attempt. Include
`## Replacement` immediately after the heading, naming the superseded packet
and explaining its defect. Render and validate all required sections before the
final status line. Append the usual `packet-rendered` or
`review-packet-rendered` six-field log line naming the replacement and the
superseded file in its result; that line selects the replacement as authoritative.
For either packet kind this selection line does not change `state`, including
when a later boundary or stop has already been reached. Reconcile the record
against all applicable completed files and log-only states, and retain the
latest valid boundary. If interrupted after the replacement is complete but
before its selection line, verify it and append that selection line on resume;
then reconcile valid later files without redispatching completed work. If selection is ambiguous, or the
inputs cannot be verified, stop and report the blocker. Never edit the original,
change the attempt or reset a counter merely to repair a packet. Use the
selected packet for subsequent worker or reviewer dispatches and packet inputs;
preserve existing results and audit evidence. If work already used the defective
packet, determine whether those results remain valid from durable evidence;
when this cannot be established, stop rather than inventing or discarding results.

Session audit observations are separate immutable supplements, not edits to
packets or results. The record writer captures start, acceptance, completion
or interruption and later discoveries as specified in
`${CLAUDE_PLUGIN_ROOT}/references/task-record.md` (PRD-R35, TS-7.5).

## Dispatch

Give the agent exactly this, and no other detail of the task:

- the path of the working tree and the path of its packet, and an instruction
  to work in the working tree;
- for a reviewer, its focus (`spec-fit`, `correctness` or `all`; the profiles
  reference says what each judges,
  ${CLAUDE_PLUGIN_ROOT}/references/profiles.md).

The planner, the mapper and the check-runner are given the path of the working
tree in the same way. What else each of them is given is in the loop reference
(${CLAUDE_PLUGIN_ROOT}/references/loop.md, "What each agent is given")
(TS-23.4).

Example: "Work in the working tree at `<working tree path>`. Your packet is
`<packet path>`. Read it first and follow it." Neither path is ever written
into a packet or report. An agent writes its own file (report or review) in the
directory that holds its packet.

## Worker packet

Written by the orchestrating actor as `worker-packet-<n>.md`. Sections, in this
order:

1. `# Worker packet: <task-id>, attempt <n>`.
2. `## Resolved configuration`: the evidence above.
3. `## Task`: the task's entry from the task list, copied exactly (the format
   is in ${CLAUDE_PLUGIN_ROOT}/references/tasks.md).
4. `## Paths you may create or change`: the `touches` paths, one bullet each,
   then `Base commit: <short hash>`, the commit the attempt starts from.
5. `## Cited spec sections, in full`: one `### <ID> (from <spec file name>)`
   per ID in the task's `cites`, then the section quoted in full.
6. `## Checks this task must pass`: one `### <ID>` per check, then the check
   copied from the testing spec in full, including its command, and its
   `Applies to` line if it has one. These are the task's `checks` plus the
   common checks that apply to the task's paths (the task list reference
   defines them).
7. When the profile has a plan stage, `## Plan`; when it has a map stage,
   `## Map`. Each holds the whole file, copied exactly, in a tilde fence (see below).
8. From attempt 2 only, `## Earlier reviews`: every review of this task from
   every earlier attempt, each as `### Attempt <n>, <focus>` and the whole
   review in a tilde fence. After a review that raised a question, add
   `Question answered:` and the answer, if one was recorded. When a question
   was answered, the answer is carried in full in every later worker packet
   and reviewer packet (TS-8.7). Omit the section on attempt 1.
9. Only when the previous attempt ended on a failed check,
   `## Previous checks`: that attempt's `checks-<n>.md`, whole, in a tilde
   fence, then its `checks-<n>-rerun.md` in a second tilde fence if there is
   one (TS-8.11). Omit the section otherwise.
10. The last line: `status: complete`.

Embed a diff or another Markdown file inside a fence of four tildes (`~~~~`),
which a three-backtick fence inside the content cannot close; use a longer run
of tildes if the content holds four. Apart from the replacement section specified above, do not add a rule, hint
or instruction to a packet beyond what this section lists; the worker's rules are in the next section.

Example (attempt 2, with a map and an earlier review):

`````markdown
# Worker packet: T-007, attempt 2

## Resolved configuration

- Profile source: `profiles/minimal.yaml`; digest: `<digest>`; schema version: 2
- Host: codex
- Selected bindings and source paths: `<frozen bindings and paths>`
- configured_model: `<requested model>`; configured_effort: `<requested effort>`
- observed_model: unavailable; observed_effort: unavailable; observation_source: unavailable

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

- Change only the `touches` paths, and within the map when there is one. An
  edit outside `touches` is not part of the task's change: it appears to the
  reviewers as an unattributed change (see "Reviewer packet") and is removed.
  Do not commit, stage or tick anything. Do not approve your own work.
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

## Interrupted steps

On resume, the task record reference determines completeness and the host
reference governs interrupted handoff (PRD-R41, TS-7.3, TS-24.5). A complete
worker report leads to checks; an incomplete worker is dispatched again with
the existing packet and a note that the tree may contain a partial change.
An incomplete review is rerun from its start. Keep completed independent
check and review results that still apply to the same candidate. Never rewrite
a completed numbered artifact or invent a missing packet or result from chat.
Pending batches retain the special checks-boundary behavior below.

## Checks file

Written by the orchestrating actor, or by the `check-runner` agent in workflow
mode, as `checks-<n>.md`, after the worker is done and before any reviewer is
dispatched. Run the checks of the task, the common ones included: the command
checks on every attempt, and the scenario checks the loop reference says run
on this attempt (${CLAUDE_PLUGIN_ROOT}/references/loop.md, "Step 7. Run the
checks") (TS-8.12). Take nothing from the worker's report.

In a parallel round a failed check is run a second time, as the loop reference
says ("Step 16"). That run is written to `checks-<n>-rerun.md`, in the format
below, holding only the checks run again; `checks-<n>.md` is not changed
(TS-19.8).

When any required check cannot run, write an immutable pending batch named
`checks-<n>-pending-<k>.md`, with `k` starting at 1 and increasing for each
batch at this boundary. For the parallel rerun boundary use
`checks-<n>-rerun-pending-<k>.md` independently. Use the same heading and
per-check sections as below, including all results obtained in this batch and
an entry for each still outstanding check: `Kind: command` or `Kind: scenario`,
`Result: pending`, and `Reason: <why it could not run>`. End with
`Passed: <n>, failed: <n>, pending: <n>` and `status: complete`. These counts
are for that batch, not a claim that the check step is complete. A completed
pending batch is durable evidence, not a completed checks step.

On resume, run only outstanding checks and write another pending batch if any
remain unavailable. Once none remain outstanding, write the ordinary checks
file for that boundary. It contains every required result for that boundary,
copying the completed per-check records unchanged from the pending batches
and adding the newly obtained results; its counts cover the whole boundary.
Never overwrite a pending batch or rerun an already completed result at that
boundary. For a later-attempt scenario sequence, retain each required phase:
after the previously failed scenarios pass, run the full scenario set as
step 7 requires, even if a scenario has already run in the first phase.
Identify phase in its check section when needed to distinguish those results.
A reviewer packet carries the final checks file and, if applicable, the final
rerun file; pending batches remain in the task directory as evidence.

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
check of the attempt passed, at its first run or at its second. One packet serves every reviewer of the attempt.
Sections, in this order:

1. `# Reviewer packet: <task-id>, attempt <n>`.
2. `## Resolved configuration`: the evidence above for the reviewers.
3. `## Task`, `## Paths you may create or change` (with the base commit) and
   `## Cited spec sections, in full`: exactly as in the worker packet.
4. `## Change`: the task's change in full, in a tilde fence marked `diff`. The
   task's change is everything under the task's `touches` paths that differs
   from the attempt's base commit, new files included. Take it with
   `git add --intent-to-add <new files under the touches paths>`, so that a
   new file shows in the diff, then `git diff <base commit> -- <touches
   paths>`.
5. `## Unattributed changes`: every unattributed change in the working tree,
   by path with its diff. A changed path is attributed when it lies under the
   `touches` of any task that has uncommitted work in the tree, whether or
   not that task is in the round, or when the replicate step wrote it; every
   other changed path is an unattributed change (TS-19.5). The loop reference
   holds the definition in full and says how the list is made
   (${CLAUDE_PLUGIN_ROOT}/references/loop.md, "Uncommitted work and
   attribution" and "Step 8"). A worker's edit outside its task's `touches`
   is not in `## Change`; it appears here. The same list goes in every
   reviewer packet of the round. Write "None." if there is none.
6. `## Earlier uncommitted tasks`: the paths of tasks that have uncommitted
   work and are not in the round, one bullet per path with the task it
   belongs to. These paths are attributed and are never deleted or restored
   (TS-8.8). Where such a task's `touches` overlap this task's, add the
   `## Change` section of that task's latest reviewer packet, in a tilde
   fence, so that the reviewer can tell the hunks apart. Write "None." if
   there is none.
7. When the profile has a map stage, `## Map`: the whole file in a tilde
   fence, as in the worker packet.
8. `## Checks`: the whole `checks-<n>.md` in a tilde fence, then, after a
   second run, the whole `checks-<n>-rerun.md` in a tilde fence of its own,
   then any `Question for the reviewer` rows of the task's checks, copied in
   full.
9. From attempt 2 only, `## Earlier reviews`: exactly as in the worker packet,
   so that a repeated finding can be recognised.
10. The last line: `status: complete`.

Example (attempt 2, with a map):

`````markdown
# Reviewer packet: T-007, attempt 2

## Resolved configuration

- Profile source: `profiles/minimal.yaml`; digest: `<digest>`; schema version: 2
- Host: codex
- Selected bindings and source paths: `<frozen bindings and paths>`
- configured_model: `<requested model>`; configured_effort: `<requested effort>`
- observed_model: unavailable; observed_effort: unavailable; observation_source: unavailable

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

## Earlier uncommitted tasks

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
  cannot be approved. Judge the task's own change (`## Change`) only. A
  worker's edit outside `touches` is not in `## Change`: it is listed under
  `## Unattributed changes` and handled as the next rule says. The paths
  under `## Earlier uncommitted tasks` belong to other tasks; do not judge
  them.
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
2. `## Answers to the question rows`, only when the packet's `## Checks`
   carries `Question for the reviewer` rows: one bullet per row, with the
   row's ID, `yes` or `no`, and a line of reason, for example
   `- TEST-I3: yes. Both docs describe the new row format.` The section is
   not a finding and does not affect the verdict; a row the change fails is
   also raised as a finding below.
3. `## Findings`: one bullet per finding, or "None." Each finding is
   `- **F<k>** cites: <ID>. <what is wrong, and what must change>`. IDs run
   `F1`, `F2`, ... within the review. Every finding cites a spec section ID or,
   for a map breach, the map (`cites: map`). Number the findings from most
   serious to least: a failed check, then a change outside `touches` or the
   map, then a spec requirement not met, then the rest. A finding that asks
   for no change is a note: write `(note)` after its ID, as in
   `- **F2** (note) cites: TS-4.1. <what you observed>`, and number the notes
   last. A note does not affect the verdict.
4. If a finding restates one in an earlier review, whichever reviewer raised
   it, end it with `repeats: <attempt>/<focus>/<finding>`, naming the attempt,
   the reviewer's focus and the finding, for example `repeats: 1/all/F2`. The
   focus is part of the mark because each reviewer numbers from F1. A finding
   that restates none carries no mark, and neither does a note: a note asks
   for no change, so it carries no `repeats:` mark and the stall guard ignores
   it (TS-23.13). Marks are what stop a loop
   that keeps meeting the same finding
   (${CLAUDE_PLUGIN_ROOT}/references/profiles.md).
5. `## Unattributed changes`, only when the packet lists any: one bullet per
   path. They are reported here and are not findings, so they do not affect
   the verdict.
6. `## Question`, only when you raise one: the question, citing the section it
   is about, as `cites: <ID>`. It goes to the operator or the decider
   (${CLAUDE_PLUGIN_ROOT}/references/escalation.md), and a new attempt starts
   once it is answered.

The last two lines are the verdict and the status line, in that order:

- `verdict: approve`: nothing needs to change. No finding, or notes only.
- `verdict: request-changes`: at least one finding that is not a note.
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
