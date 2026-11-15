---
name: check
description: "Planar plugin: run the ID, citation and traceability check on a project's spec set. Dispatches the spec-checker agent, saves its report as reviews/spec-check-<n>.md, and on a pass updates ids.md and the spec-set status. Use when the operator asks to check the specs, or before review, as /planar:check."
---

# Check the spec set

Implements PRD-R20 (ID and citation check) and PRD-R37 (traceability), with
TS-5.3 (ID index), TS-5.5 (traceability), TS-21.3 (amending), TS-10 and
TS-11.5; layout entry LAY-1.3.3. The formats are defined in the references
named below, which you read and follow; they are not restated here.

Use only file reads and writes, shell commands and subagent dispatch.

## 1. Find the project

Resolve the project directory as `${CLAUDE_PLUGIN_ROOT}/references/settings.md`
and `${CLAUDE_PLUGIN_ROOT}/references/project.md` say. Read `project.yaml`. If
the project does not exist, tell the operator to create one first and stop.
Stop too if any of the five spec files is missing, and name it.

## 2. Pick the report number

`<n>` is one more than the highest `reviews/spec-check-<n>.md`, or 1 if there
is none.

## 3. Dispatch the checker

Dispatch the `spec-checker` agent once. Give it the project directory and
`<n>`, and nothing else. It does its counting with shell commands, writes
`reviews/spec-check-<n>.md` itself and edits nothing else
(`${CLAUDE_PLUGIN_ROOT}/references/project.md` lists it as the writer). Do not
write or edit the report yourself.

Read the report. Its last line is `status: pass` or `status: fail`. If it is
missing or the file is incomplete, treat the check as not run: say so, change
nothing, and stop.

## 4. On a pass

1. Update `ids.md`, which only this skill writes, in the form
   `${CLAUDE_PLUGIN_ROOT}/references/conventions.md` gives ("ID index"): keep
   the heading `# IDs issued`, rewrite the second line as ``Date: <date>.
   Report: `reviews/spec-check-<n>.md`.`` with the date from
   `date +%Y-%m-%d`, add each new ID as `live` (the report lists each with its file), set a
   section that is now a withdrawn stub to `withdrawn`, and never delete or
   reorder an existing line. If `ids.md` does not exist, create it with every
   ID defined in the specs as `live`, using shell commands to collect them.
2. Set `spec_set.status` to `checked` and `spec_set.checked` to the date and
   the report path in `project.yaml`, unless the status is `approved`. An
   approved set keeps its status: a check does not change it (TS-21.3). Tell
   the operator that the choice between sign off and full review is next, as
   "Amending an approved spec set" in
   `${CLAUDE_PLUGIN_ROOT}/references/project.md` says; the `review` skill
   handles it.

## 5. On a fail

Do not touch `ids.md`. Unless the status is `approved`, set `spec_set.status`
to `drafting`. Show the operator the failures the report names, and say the
specs do not go to review until a check passes. Do not dispatch a spec
reviewer and do not create any `spec-review` file.

## 6. Finish

Tell the operator the result, the report's path and what is next: `review` on
a pass, `draft` on a fail. Write nothing else: no spec, `context.md` or task
record.
