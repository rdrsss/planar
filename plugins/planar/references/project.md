# Project directory and `project.yaml`

Implements PRD-R17 (spec location) and PRD-R25 (name collisions), TS-3
(project directory, `project.yaml`, approval gate, `context.md`, naming,
collisions), TS-21 (approval digests and amendments) and TS-23 (TS-23.3,
TS-23.6 and TS-23.10), and the reference
layout entry LAY-1.5.3 and the project directory layout LAY-3, including LAY-3.26 and TS-19.10. This file is
the single definition of what a project directory holds, who writes each
file, the `project.yaml` format, the approval gate, the digest commands and
how an approved spec set is amended. Follow it whenever you create, read or
change a project.

## What a project is

A project is one piece of work, not one repository. A repository may have many
projects. Each project has one self-contained directory that holds its spec
set and its task records. Nothing else about a project is kept at the top
level of `<home>`.

The directory is `<projects_dir>/<project>/`. By default that is
`~/.planar/projects/<project>/`. The operator can move it in settings, either
for all projects (`projects_dir`) or for one (`projects.<project>.path`);
resolve it as the settings reference
(`${CLAUDE_PLUGIN_ROOT}/references/settings.md`) says.

## The directory

```text
<projects_dir>/<project>/
├── project.yaml          identity and spec-set status
├── context.md            the operator's statement, sources, decisions
├── prd.md
├── tech-spec.md
├── layout.md
├── testing-spec.md
├── tasks.md              task list
├── ids.md                every ID ever issued
├── gaps.md               gap log
├── decisions.md          decisions taken by the decider
├── reviews/
│   ├── spec-check-<n>.md
│   ├── spec-review-<n>-<focus>.md
│   └── final-checks.md
├── tasks/
│   └── <task-id>/        one directory per task
│       ├── record.md
│       ├── plan.md               when the profile has a plan stage
│       ├── map-<n>.md            when the profile has a map stage
│       ├── worker-packet-<n>.md
│       ├── worker-report-<n>.md
│       ├── checks-<n>.md
│       ├── checks-<n>-pending-<k>.md   when checks are unavailable
│       ├── checks-<n>-rerun.md   when a failed check was run a second time
│       ├── checks-<n>-rerun-pending-<k>.md   when reruns are unavailable
│       ├── reviewer-packet-<n>.md
│       └── review-<n>-<focus>.md
├── scratch/
│   └── scratch-<purpose>/  project-local scratch
└── worktrees/
    └── wt-<project>/     default named git worktree
```

`<n>` is the attempt number, counting from 1. `<task-id>` is the task's ID,
for example `T-008`.

Write each file only if you are its writer:

| Path | Written by |
| --- | --- |
| `project.yaml` | `project` creates it; `draft`, `check` and `review` update `spec_set`; `run` updates it only to seal an amendment (see Amending) |
| `context.md` | `project`, `draft` |
| `prd.md`, `tech-spec.md`, `layout.md`, `testing-spec.md` | `draft` |
| `tasks.md` | `draft`; `run` ticks checkboxes |
| `ids.md` | `check` |
| `gaps.md` | `gap` |
| `decisions.md` | `run`, `decider` |
| `reviews/spec-check-<n>.md` | `spec-checker` |
| `reviews/spec-review-<n>-<focus>.md` | `spec-reviewer` |
| `reviews/final-checks.md` | the orchestrating actor in skill mode, `check-runner` in workflow mode |
| `tasks/<task-id>/record.md` | the orchestrating actor; the session that asked the operator, for `answered` lines only |
| `tasks/<task-id>/plan.md` | `planner` |
| `tasks/<task-id>/map-<n>.md` | `mapper` |
| `tasks/<task-id>/worker-packet-<n>.md`, `reviewer-packet-<n>.md` | the orchestrating actor |
| `tasks/<task-id>/worker-report-<n>.md` | `worker` |
| `tasks/<task-id>/checks-<n>.md`, `checks-<n>-rerun.md` and their numbered pending batches | the orchestrating actor in skill mode, `check-runner` in workflow mode |
| `tasks/<task-id>/review-<n>-<focus>.md` | `reviewer` |
| `worktrees/wt-<project>/` (default) | `review` at approval, or `run` |
| `scratch/scratch-<purpose>/` | creating actor with recorded lifecycle owner |

The formats of `ids.md`, the spec files, `tasks.md`, task records, gaps and
the other reports are defined in their own references; this file defines only
the project directory, `project.yaml` and `context.md`.

New project-local scratch, including disposable repositories, temporary homes
and generated local eval outputs, belongs under the real project
`scratch/scratch-<purpose>/`; canonicalize and refuse symlink escapes. Record
lifecycle and cleanup ownership. An explicit external eval-output override is
retained and recorded; canonical task, spec and review records stay in place.
Existing roots are not silently moved: inventory owners, readers, processes,
Git registration, frozen inputs and evidence; move only quiescent eligible
roots, using `git worktree move` for registered trees and verifying branch,
HEAD, index and status. Preserve evidence bytes and hashes, append a relocation
map, and record blocked moves. Frozen absolute paths require fresh reviewed
inputs before reuse; do not bulk rewrite history, delete evidence or introduce
permanent root compatibility symlinks (TS-19.10).

## Naming a project

Choose the name in this order:

1. A name the operator gives. It always wins.
2. Otherwise, if the work comes from a tracker issue and settings prefer
   `tracker` naming: `<KEY>-<slug of the title>`, for example
   `ABC-123-add-export`.
3. Otherwise a slug of the work, taken from the operator's statement, or from
   the repository name when the statement gives nothing better. A slug is
   lower case letters, digits and hyphens only, at most 40 characters, for
   example `widgets-colours`.

Tell the operator the name before you write anything.

## Creating a project: collisions

Do these in order and write nothing until the operator has answered:

1. List the existing projects whose `repository` matches the new one: the same
   `remote`, or, when there is no remote, the same `path`. If there are any,
   show them and ask whether to continue one of them or start a new project.
2. If the chosen name is already a directory under the projects directory,
   ask the operator what to do and offer an example, such as the same name
   with a number suffix (`widgets-colours-2`). Never pick a new name, and
   never reuse the existing directory, unless the operator tells you to.

## `project.yaml`

`project` creates it. Example:

```yaml
version: 1
name: widgets-colours
naming: context            # context | tracker | operator
created: 2026-11-14
repository:
  path: /path/to/widgets   # working copy on this machine
  remote: git@example.com:org/widgets.git   # if any
spec_set:
  status: drafting         # drafting | checked | reviewed | approved
  checked: null            # date and report
  reviewed: null           # round, date, and each report with its verdict
  spec_lenses: []          # lenses confirmed for this project
  approved: null           # date
  digests: {}              # file name: sha256, written at approval
  amendments: []           # see Amending
```

- `naming` records how the name was chosen: `context`, `tracker` or
  `operator`.
- `repository.remote` is omitted when the repository has none.
- `spec_set.status` is one of `drafting`, `checked`, `reviewed`, `approved`.
- `spec_set.digests` is empty until approval; see Digests.
- `spec_set.spec_lenses` lists the extra spec review lenses confirmed for this
  project. How a lens is chosen and applied is defined in the spec review
  reference.
- When a spec review returns `request-changes`, leave `status` at `checked`
  and record the verdict in `reviewed`. The next edit by `draft` sets
  `status` to `drafting`.

## `context.md`

`project` creates it and `draft` adds to it. It holds, in this order:

1. The operator's statement of the work, in their own words.
2. The sources gathered for the specs.
3. A numbered, dated list of operator decisions.
4. A change log.

Each time `draft` edits a spec, it appends one change-log entry: the date, the
section IDs it changed and one line saying why. A citation of the form
"operator decision, <date>" resolves to an entry in the decisions list. The
spec reviewer judges whether the specs fit the operator's intent against this
file.

Example:

```markdown
# Context: widgets-colours

## Statement

"Let people pick a colour for each widget and show the colour in the list."

## Sources

- src/widgets/list.cpp:40, the list renderer
- README.md, "Widgets"

## Decisions

1. 2026-11-14: colours are a fixed palette of eight, not free-form.

## Change log

- 2026-11-14: PRD-R2, TS-4.1. Added the fixed palette (decision 1).
```

## The approval gate

`run` refuses to start unless `spec_set.status` is `approved` and every spec
file still matches its digest. A spec set is approved only by the operator.

## Digests

When the operator approves, `review` records the SHA-256 of each of the five
spec files (`prd.md`, `tech-spec.md`, `layout.md`, `testing-spec.md`,
`tasks.md`) in `spec_set.digests`, using a shell command: `shasum -a 256` or
`sha256sum`, whichever the machine has. Keep only the hex digest, not the
file name the command prints. Approval is of that content.

The digest of `tasks.md` is taken with every ticked checkbox read as
unticked, so ticking a task does not change it:

```sh
shasum -a 256 prd.md                                # a spec file
sed 's/^- \[x\]/- [ ]/' tasks.md | shasum -a 256    # tasks.md
```

Run the commands from the project directory. Example result:

```yaml
digests:
  prd.md: 3f1c…e9a0          # full 64-character hex digest in the real file
  tech-spec.md: 8b27…41d5
  layout.md: c0de…77b2
  testing-spec.md: 5a90…0c3e
  tasks.md: e41d…9f66
```

`run` recomputes every digest before each round. A mismatch stops the run,
whoever made the edit.

## Amending an approved spec set

An edit to an approved spec re-runs the ID check, which does not change the
status of an approved set. Then the operator chooses:

- **Sign off**, for a small amendment: add an entry to `spec_set.amendments`,
  rewrite the digests, and leave `status` as `approved`.
- **Full review**, for a changed requirement: set `status` back to `checked`
  and run spec review again.

If the operator does not say which, it is a full review.

An amendment entry has four fields: the date, the files changed, a one-line
reason, and who made the decision.

```yaml
amendments:
  - date: 2026-11-15
    files: [tech-spec.md]
    reason: Clarified the palette size after operator review.
    who: operator
```

Two cases are sealed without the ID check or a choice:

- **An answer given during a run.** When the operator answers a question
  raised during a run, the answer is itself the sign-off. A question cites the
  section it is about, and the spec that defines that section owns it. The
  session that asked writes the question and answer to the task record, as
  an `answered` line. The orchestrating actor, when it next runs,
  records them under "Answered questions" in the owning spec (format in the
  spec conventions reference, `${CLAUDE_PLUGIN_ROOT}/references/conventions.md`),
  adds an amendment entry with `who: operator`, and rewrites the digests. An
  entry in "Answered questions" adds no ID, so no check is run. Only an
  answer that cites a spec section is an amendment: an operator's decision
  on a `blocked` task, or an answer that concerns no spec section, stays in
  the task record only, and nothing is written to a spec or sealed for it
  (TS-23.3).
- **A decider amendment.** In an autonomous run the decider may answer a
  question and may amend a task entry in `tasks.md` (its title, `cites`,
  `touches`, `checks` or `after`). It may not change any other spec text. The
  orchestrating actor seals each such change as an amendment with
  `who: decider D-<n>` (the `D-` entry in `decisions.md`; see
  `${CLAUDE_PLUGIN_ROOT}/references/escalation.md`) and rewrites the digests.
  These amendments are listed for the operator at the end of the run, and the
  operator can reverse any of them through the sign off or full review choice
  above.

```yaml
  - date: 2026-11-16
    files: [tasks.md]
    reason: T-012 touches list corrected.
    who: decider D-3
```
