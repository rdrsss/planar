---
name: run
description: "Planar plugin: execute an approved project's tasks. Checks the profile, the approval gate and the working tree, then takes each task through worker, checks, review, approval and commit, writing every step to the task's record; resumes an interrupted run from those records. Use when the operator asks to run a task, the next task or all tasks of a Planar project, or to continue or resume its run, as /planar:run."
---

# Run a project's tasks

Provenance: implements PRD-R16, PRD-R19, PRD-R23, PRD-R26, PRD-R27 and PRD-R32;
TS-3.2, TS-4.2, TS-4.5, TS-4.6, TS-6.3, TS-7.2, TS-7.3, TS-8.6 to TS-8.8,
TS-8.10 to TS-8.12, TS-9.1, TS-9.5, TS-17.6, TS-17.7, TS-19.1, TS-19.2,
TS-11.4, TS-19.5 to TS-19.7, TS-20.1, TS-21.1, TS-21.2, TS-21.5 and TS-23; layout
entry LAY-1.3.5.

You are the session the operator is talking to. Do these steps in order.
The plugin's references are the definitions and are not repeated here: when
a step names a reference, read that file in full and follow it exactly.

## Rules for the whole run

- **Only these means.** Use file reads and writes, shell commands, and
  dispatching a subagent with a prompt and, when the profile gives them, a
  model and effort. Use nothing else: no other skill invocation, no
  background task, no workflow, no task-list or planning tool. To ask the
  operator something, end your reply with the question and wait.
- **Where you write.** Write only inside the project directory and the
  working tree. Never edit the settings file or a profile.
- **Git.** Never push, merge, rebase, or remove a branch or a worktree. The
  operator does those.
- **Paths.** Absolute paths are kept out of packets, reports, checks files
  and records as `${CLAUDE_PLUGIN_ROOT}/references/packets.md` says.
- **A refusal is final.** When an opening check fails, write nothing and
  create nothing. Your whole final reply is the plain statement that check
  gives, followed, only when `<home>` is not the default, by the one-line
  notice of step 2 naming the directory in use. Do not repair the cause,
  and do not go on to a later check.

## 1. Work out what is asked

From the operator's words take:

- **The project**, if named.
- **The tasks**: named task IDs ("run task T-003", "T-001 and then T-002");
  "the next task", which is the one next task; or "all tasks", which is
  every unticked task. The ready rule and "next task" are defined in
  `${CLAUDE_PLUGIN_ROOT}/references/tasks.md`, "Next task".
- **A resume.** A task is **in flight** when it is unticked and its record
  has a `base`. "Continue", "resume", "carry on" or "pick up where it
  stopped", with no task named, asks for every task in flight; if none is
  in flight, it asks for the next task. "The next task" on a project with a
  task in flight is the first of those in file order, not a new one.
- **A stop instruction**, if any. It takes one of three forms:
  - "stop after `<step>` of `<task>`" ("run T-003 and stop after the
    worker-done step"). `<step>` is a step name of the task record. Plain
    words name the step they describe: "the worker" is `worker-done`,
    "the checks" is `checks-done`, "the review" is `reviewed`. If you
    cannot tell which step is meant, ask before starting.
  - "one round", "after one round": stop when the first round is complete.
  - "stop after `<task>`": stop when that task's round is complete.
- **Answers given in advance**, if any ("if asked X, answer Y"). Keep each
  with the question it is for. It is used only as section 5 says.
- **The mode**, if the operator asks for workflow mode.

## 2. Resolve home, settings, project, policy and profile

Read `${CLAUDE_PLUGIN_ROOT}/references/settings.md` and
`${CLAUDE_PLUGIN_ROOT}/references/project.md`, then:

1. Resolve `<home>` as "Resolving `<home>`" says (`printenv PLANAR_HOME`).
   If it is not the default, tell the operator which directory you are
   using, in one line, once, in whichever reply ends this run.
2. Read `<home>/settings.yaml` if it exists. With no file, every default
   applies. Report an unknown key or bad value as the settings reference
   says and carry on.
3. Find the project directory, as the project reference and the settings
   keys `projects_dir` and `projects.<project>.path` define it.
   - Project named: if its directory or its `project.yaml` does not exist,
     say so and stop. Create nothing.
   - Project not named: read `project.yaml` in each directory under the
     projects directory and take the project whose `repository` is the
     current git repository, compared as the project reference compares
     repositories: `repository.remote` against `git remote get-url origin`
     first, and `repository.path` against `git rev-parse --show-toplevel`
     only when the project has no remote. If there is not exactly one, list
     the projects and ask which.
4. Read `project.yaml`. The repository is its `repository.path`.
5. Resolve the policy in force and its rules `commit` and `worktree`
   ("Policy in force"), the mode (`orchestration.mode`, unless the operator
   asked for one), and the project's `replication` setting. Never assume a
   rule's value; use the one resolved here wherever the loop names the rule.
6. Writing nothing, resolve the name of the profile in force ("Profile in force") and find
   its file as `${CLAUDE_PLUGIN_ROOT}/references/profiles.md`, "Resolving a
   profile to a file", says. Shipped profiles are
   `${CLAUDE_PLUGIN_ROOT}/profiles/<name>.yaml`, for example
   `${CLAUDE_PLUGIN_ROOT}/profiles/minimal.yaml`. Read the profile file now,
   before any check.

## 3. Opening checks

Do the three checks in this order: the profile, the approval gate, the
working tree. All are read-only except the last action of check 3. The
first one that fails ends the run as "A refusal is final" says, and no
`tasks/` directory may exist afterwards that did not exist before.

### Check 1. The profile

Validate the profile file read in step 2 against
`${CLAUDE_PLUGIN_ROOT}/references/profiles.md`, "Validating a profile" and
"Keys": every key at every level, every stage name, every value, every
required key and stage, and every `agent`.

- An `agent: <name>` exists when `${CLAUDE_PLUGIN_ROOT}/agents/<name>.md`
  exists, and is then dispatched as the subagent type `planar:<name>`
  (`planar:worker`, `planar:reviewer`); otherwise when an agent of exactly
  that name is available to this session, and is then dispatched under that
  exact name.
- On any failure, stop. Say which stage, agent or key it is, that it is
  unknown (or what else is wrong with it), and the file: "Profile `<name>`
  (`<file>`) is refused: stage `<stage>` is unknown. Nothing was run." Do
  not repair the file, substitute another profile, or run with part of it.

Then, with a valid profile, stop with a plain statement that this version
of the run skill does not do it yet, and that nothing was run, when any of
these holds:

- the profile has `driver: orchestrator`;
- the mode is `workflow`.

### Check 2. The approval gate

Defined in `${CLAUDE_PLUGIN_ROOT}/references/project.md`, "The approval
gate" and "Digests".

1. If `spec_set.status` in `project.yaml` is not `approved`, stop. Say:
   "The spec set of project `<project>` is not approved (its status is
   `<status>`). Nothing was run."
2. Recompute the digest of each of the five spec files, with the commands
   of "Digests", run from the project directory. `tasks.md` is hashed with
   every checkbox read as unticked.
3. Compare each printed digest with the same file's entry in
   `spec_set.digests`. A missing file or a missing entry is a mismatch. On
   any mismatch, stop. Say, naming every such file: "`<file>` no longer
   matches what was approved. Nothing was run. The spec set has to be
   amended or reviewed again before a run."

The loop repeats this comparison before every round (its step 1), with the
same commands; a mismatch there stops the run with the same statement.

### Check 3. The working tree

Defined in `${CLAUDE_PLUGIN_ROOT}/references/loop.md`, "Before the first
round", items 3 to 5, and the `worktree` rule in
`${CLAUDE_PLUGIN_ROOT}/references/settings.md`.

1. The repository must be a git repository:
   `git -C "<repository>" rev-parse --show-toplevel`. If it fails, stop and
   say the project's repository is not a git repository.
2. If the projects directory is inside the repository, it must be ignored
   by git (`git -C "<repository>" check-ignore -q "<projects directory>"`).
   If not, stop and say so.
3. Under `worktree: project`, the working tree is
   `<project directory>/worktree`, on branch `planar/<project>`:
   - If it exists and `git -C "<project directory>/worktree" branch
     --show-current` prints `planar/<project>`, use it as it is.
   - If it does not exist: run `git -C "<repository>" status --porcelain`.
     If that prints anything, ask the operator first, naming the paths:
     those changes will not be in the worktree. Go on only on a yes. Then
     create it from the commit the repository has checked out:

     ```sh
     git -C "<repository>" worktree add -b "planar/<project>" "<project directory>/worktree" HEAD
     ```

   - Anything else (the directory exists but is not that worktree, or the
     branch exists without it): stop and put it to the operator. Remove
     nothing.
4. Under `worktree: none`, the working tree is the repository's own
   checkout on its current branch. When the project directory has no
   `tasks/` directory yet, run `git -C "<repository>" status --porcelain`;
   if it lists any path outside the project's replication target, stop.
   Say: "The working tree has uncommitted changes: `<paths>`. Under
   `worktree: none` a run starts only from a clean tree. Nothing was run."
   When `tasks/` exists, this is a restart: the loop reference, "At the
   start of every run", says what to do.
5. If replication is enabled for the project, do the target check of
   `${CLAUDE_PLUGIN_ROOT}/skills/replicate/SKILL.md`, "Check the target",
   now, writing nothing. If it asks the operator a question, stop with that
   question and run nothing (PRD-R23, TS-11.4).

## 4. Drive the loop

The profile has `driver: session`, so you are the driver: the
"orchestrating actor" of every reference.

1. **Read once.** Read `${CLAUDE_PLUGIN_ROOT}/references/loop.md` in full,
   and the references it needs for the bookkeeping:
   `${CLAUDE_PLUGIN_ROOT}/references/task-record.md` (the record, who
   writes, files are the truth),
   `${CLAUDE_PLUGIN_ROOT}/references/packets.md` (packets, checks file,
   dispatch wording) and `${CLAUDE_PLUGIN_ROOT}/references/tasks.md`. You
   have now read the loop reference, the profile and this skill; do not
   read any of the three again during a task. Read the others below only
   when the loop first sends you to them:
   `${CLAUDE_PLUGIN_ROOT}/references/escalation.md`,
   `${CLAUDE_PLUGIN_ROOT}/references/plans.md`,
   `${CLAUDE_PLUGIN_ROOT}/references/conventions.md`.
2. **Reconcile first.** On every start, before any step of the loop
   reference, when the project has a `tasks/` directory: for each task
   directory there, confirm the record against the files beside it, as
   the task record reference says under "Files are the truth" and
   "Resume". Trust no `state` you have not confirmed this way. Where the
   two disagree, the files win: correct the record as that reference says
   and record one gap for the task (item 7), giving as the step `TS-7.2`,
   as what happened the task ID, what the record said and what the files
   show, and nothing for the other fields. Leave alone a task whose
   `state` is `approved` or whose box is ticked but which has no `approved`
   line: that is an interrupted approval. Neither reset it nor log a gap;
   the loop reference's "At the start of every run" finishes it in the next
   step. Then work out
   which tasks are in flight (step 1) and, for each asked task whose record
   is in `question` or `blocked` with no `answered` line after it, put it
   to the operator as section 5 says before you drive anything else.
3. **Follow the loop reference yourself**, from "At the start of every run"
   through the rounds, step by step, doing each step's file and log line
   before the next. It is the whole procedure, from the record to the tick
   and the commit. Do not skip, merge or reorder steps, and do not do a
   worker's or a reviewer's work yourself.
4. **Which tasks.** Only the tasks the operator asked for (step 1) may
   enter a round; in the loop's step 1 read "ready tasks" as the ready
   tasks among them. A named task that is already ticked is reported, not
   run. If an asked task is not ready because a task outside those asked
   for is unticked, say which and stop before writing anything for it; do
   not widen the run yourself. The run ends when every asked task is
   ticked, or when none of them can be taken.
5. **Dispatches.** Dispatch the `implement` stage's agent and each reviewer
   as the subagent type worked out in check 1, with the prompt the packets
   reference gives under "Dispatch": the absolute paths of the working tree
   and of the packet, the instruction to work in the working tree, a
   reviewer's focus, and nothing else about the task. Pass a model and
   effort only when the profile gives them. When a worker's step is run
   again on a resume, add only the note the task record reference gives
   under "Resume".
6. **Models in the record.** Every log line names a model by its full ID,
   for example `claude-sonnet-5-5`, never an alias or a short name. A model
   in a profile is a full ID, written as given. For a step you do yourself,
   the actor is `session` and the model you are running on: use your own
   full model ID, which you know from your environment. For an agent the
   profile gives no model, it runs on your model: write your full model ID.
   The mode field of every line is `skill`.
7. **The gap skill.** Where the loop or the task record reference says to
   record a gap, read `${CLAUDE_PLUGIN_ROOT}/skills/gap/SKILL.md` and do
   what it says for this project (it is the skill `/planar:gap`).
8. **Everything else is the loop's.** Each of these is defined in full at
   the place named; go there when you reach it and do exactly that. This
   skill adds only section 5.

   | When | Where it is defined |
   | --- | --- |
   | Which checks run on an attempt; a failed check | loop reference, step 7, then step 14 |
   | Changes requested; the revision limit; the stall guard | loop reference, step 14 |
   | A reviewer's open question; a blocked task | loop reference, step 15, and the escalation reference |
   | A task in flight, or an interrupted approval | task record reference, "Resume"; loop reference, "At the start of every run" and step 2 |
   | A stop instruction | loop reference, "Stop point" |
   | The last asked task is ticked | loop reference, step 1 once more: when every task of the project is ticked it leads to "After the last task", the final check |

## 5. When the run needs the operator, or reaches a stop point

This version dispatches no decider: every question and blocked task goes
to the operator, as the escalation reference says under "Autonomy off".
When `autonomy.enabled` is true in the settings, say once, at the start of
the run, that this version does not dispatch a decider, and treat every
escalation as autonomy off.

**A question or a blocked task.** First do what the loop's step 15 says:
the `question` or `blocked` line is in the record, and every other task of
the round has finished its steps. Then:

1. If the operator gave an answer in advance for this very question (step
   1), take it as the operator's answer, word for word, and go to item 3.
   Never use one for a different question, and never answer for the
   operator yourself.
2. Otherwise stop: end your reply with the report of section 6, and after
   it:
   - the task ID and its title;
   - for a question: the question in full, as the review or the plan has
     it, with the section it cites and the file it is in;
   - for a blocked task: the reason in full, as its `blocked` line has
     it, the attempts made, and the checks file or review that holds the
     detail;
   - what you need: for a question, the answer; for a blocked task, what
     to do with it, asked as a question ("What should be done with
     T-003?") with the choices: allow one more attempt, change the task
     or the spec first, or leave it blocked.
3. When the operator answers, in this session or a later one, append the
   `answered` line to the task's record: what was asked and where (the
   review file and its question, `plan.md`, or the `blocked` line), the
   answer or decision in the operator's words, and where it is recorded
   (the spec that owns the cited section, or this record only). Then go on
   in the same reply: return to the loop reference's "At the start of
   every run", which seals an answer that cites a spec section before the
   digests are compared again, and continue the rounds from step 1. Write
   to a spec only there. If the decision is to leave a task blocked, drive
   the other asked tasks and report this one as pending. If the operator
   allows one more attempt, it runs on the next rung of the ladder when
   there is one, and otherwise on the same rung.

**A stop point.** Complete the named step in full, with its file and its
log line, or the round; do no step after it, not even the next one of the
same round; then end with the report of section 6. Add: the task, the step
you stopped after, and that asking to continue the project resumes from
the records. Ask the operator nothing. A stop instruction holds for this
run only and is written nowhere.

## 6. Report to the operator

When the run ends, for whatever reason, your final reply states:

- each task ticked in this run, and each asked task that was already ticked;
- each commit made: its short hash, its task and the branch it is on; or
  that nothing was committed because the policy's `commit` rule is `none`;
- anything left pending: each asked task not ticked, with its `state` from
  its record and the reason (changes requested, a question, blocked, not
  ready), and any question waiting for the operator, asked in full;
- where the work is: the working tree and its branch, and that nothing was
  pushed or merged;
- for each task driven in this run, the number of attempts made, from its
  record;
- the final check's result when it ran: every check passed, or each check
  that failed, with its task; and each gap entry recorded in this run;
- whatever else the loop reference or the escalation reference says to
  report at the end of a run.

Take all of it from the task list, the records and `git log`, not from
memory.
