---
name: review
description: "Planar plugin: review a project's spec set and take the operator's approval. Requires a passing check, dispatches the profile's spec reviewers and one for each project lens, saves their reports as reviews/spec-review-<n>-<focus>.md, asks the operator to approve only when every reviewer approves, records the digests, and handles amendments to an approved set. Use when the operator asks to review or approve the specs, or to sign off or review an amendment, as /planar:review."
---

# Review the spec set

Implements PRD-Q21 (amendments), PRD-R16 (spec review), PRD-R36 (focuses and lenses) and PRD-R38
(gap analysis), with TS-3.2 (approval gate), TS-17.1 (shipped profiles),
TS-11.4 (replicate step), PRD-R23 (replication), TS-21 (TS-21.1 digests, TS-21.3 amending, TS-21.4 status after a failed review), TS-22 (spec
review) and TS-10; layout entry LAY-1.3.4. The formats are defined in the
references named below, which you read and follow; they are not restated here.

Use only file reads and writes, shell commands and subagent dispatch.

## 1. Find the project and require a passing check

Resolve the project directory as `${CLAUDE_PLUGIN_ROOT}/references/settings.md`
and `${CLAUDE_PLUGIN_ROOT}/references/project.md` say. Check the environment
variable `PLANAR_HOME` first, with a shell command such as
`printenv PLANAR_HOME`: when it is set it replaces `~/.planar/` as `<home>`, and
you look for the project only there. Use the project the operator names; never
substitute another project for it. Read `project.yaml`. If that project does not
exist under `<home>`, tell the operator to create one first and stop, without
asking a question and without looking elsewhere.

Find the highest `reviews/spec-check-<n>.md`. Review requires a passing check
(TS-22.1): stop and tell the operator to run `check` first if there is no
report, if its last line is not `status: pass`, or if `spec_set.status` is
`drafting`. If a spec file is newer than that report, say so and ask the
operator to run `check` again.

If `spec_set.status` is `approved`, go to step 7.

## 2. Resolve the reviewers

Resolve the profile in force as the settings and profiles references say
(`${CLAUDE_PLUGIN_ROOT}/references/profiles.md`, "Resolving a profile to a
file"). Take `spec_review.reviewers` from it; if the profile has none, report
that and stop. Add one lens reviewer for each lens in `spec_lenses` of the
project in settings and in `spec_set.spec_lenses` of `project.yaml`, without
duplicates.

`<n>` is one more than the highest round number in `reviews/spec-review-*`,
or 1 if there is none. The focuses, lenses and report format are in
`${CLAUDE_PLUGIN_ROOT}/references/spec-review.md`.

## 3. Dispatch the spec reviewers

Dispatch the `spec-reviewer` agent once for each entry of
`spec_review.reviewers` and once for each lens (TS-22.1). Give each the
project directory, its focus or lens, and `<n>`. A profile reviewer runs on
the `model` and `effort` its entry gives; a lens runs on the model of the
profile's first spec reviewer. Pass them on the dispatch when set. Each
reviewer writes `reviews/spec-review-<n>-<focus>.md` itself; do not write or
edit a report.

From round 2, also give each reviewer its earlier reports and the list of what
changed since: the change-log entries in `context.md` dated after the previous
round's reports (TS-22.5).

Read every report. Its last two lines are a verdict, `verdict: approve` or
`verdict: request-changes`, then `status: complete`. A report that is missing
or lacks them is a reviewer that did not finish: say so and do not count it as
an approval.

## 4. The result

The result is `request-changes` if any report says so, and `approve` only when
every report approves (PRD-R36).

- **Request changes.** Leave `spec_set.status` at `checked` and record the
  round, the date and each report with its verdict in `spec_set.reviewed`
  (TS-21.4). Show the operator the blockers and should-fix findings, each with
  its section ID, and say `draft` is next; the next edit by `draft` sets the
  status to `drafting`. Do not ask for approval.
- **Approve.** Set `spec_set.status` to `reviewed`, record the round, date and
  reports in `spec_set.reviewed`, and go to step 5.

Never answer the operator's open questions and never edit a spec.

## 5. Ask the operator to approve

Show the operator the reviewers' verdicts, the should-fix findings and notes
(which are handled as amendments), the questions the specs should have asked,
and the report paths. Ask the operator to approve or not. Only the operator
approves; a reviewer's approval is not the operator's. If the operator wants a
change, leave the status at `reviewed` and say `draft` is next.

## 6. On approval

1. Record the digests (TS-21.1) with a shell command, from the project
   directory, as "Digests" in `${CLAUDE_PLUGIN_ROOT}/references/project.md`
   defines: `shasum -a 256` or `sha256sum` of each of the five spec files, and
   of `tasks.md` with `sed 's/^- \[x\]/- [ ]/'` applied before hashing. Keep
   only the hex digest. Write them to `spec_set.digests`.
2. Set `spec_set.status` to `approved` and `spec_set.approved` to the date
   from `date +%Y-%m-%d`.
3. Prepare the working tree as "Before the first round" in
   `${CLAUDE_PLUGIN_ROOT}/references/loop.md` says for the policy's `worktree`
   rule, asking the operator first when that reference says to.
4. If replication is on for the project, replicate as
   `${CLAUDE_PLUGIN_ROOT}/skills/replicate/SKILL.md` says (PRD-R23, TS-11.4),
   after the working tree is prepared. The digests and status are already
   written; the copy is committed on its own under the policy's `commit`
   rule. If it asks the operator about the target, stop there.
5. Tell the operator that `run` is next and that it refuses to start if a spec
   file no longer matches its digest (TS-3.2).

## 7. An approved set that has been amended

Every spec edit after approval re-runs the ID check (PRD-Q21, TS-21.3).
Require a `reviews/spec-check-<n>.md` newer than the edit and ending
`status: pass`; if there is none, tell the operator to run `check` and stop.
The check leaves the status at `approved`. "Amending an
approved spec set" in `${CLAUDE_PLUGIN_ROOT}/references/project.md` defines the
two choices; ask the operator which, and treat no answer as a full review.

- **Sign off.** Add an entry to `spec_set.amendments` with the date, the files
  changed, a one-line reason and `who: operator`; recompute and rewrite the
  digests as in step 6; leave the status `approved`.
- **Full review.** Set `spec_set.status` to `checked`, set
  `spec_set.approved` to `null`, and go to step 2. Digests are rewritten only
  when the operator approves again (step 6).

Write nothing else: no spec, `context.md`, `ids.md` or reviewer report.
