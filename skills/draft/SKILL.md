---
name: draft
description: "Planar plugin: draft or revise a project's specs. Records the operator's statement, sources and decisions in context.md, drafts or revises one spec at a time (PRD, tech spec, layout, testing spec, task list) with stable IDs and cited evidence, logs every change, records answers to open questions, and suggests spec review lenses. Use when the operator asks to draft, write, edit or revise specs for a Planar project, or to answer an open question, as /planar:draft."
---

# Draft the specs

Provenance: implements PRD-R1, PRD-R2, PRD-R3, PRD-R4, PRD-R5 and PRD-R6;
TS-3.3, TS-5 (TS-5.1, TS-5.2, TS-5.4) and TS-22.3; layout entry LAY-1.3.2.
The plugin's references are the definitions and are not repeated here: read
the files named below in full and follow them exactly.

Use only file reads and writes, shell commands and subagent dispatch. This
skill needs no subagent.

## Rules

- **One spec at a time.** Draft or revise a single spec, finish it, then go
  to the next. Do not mix the contents of two specs.
- **Never write into the repository.** Everything goes in the project
  directory. Read the repository as far as the specs need.
- **Never answer an open question yourself.** Only the operator answers
  (PRD-R6). The operator may say "your recommendation" or the equivalent:
  that is their answer, so record it as the operator's, give the
  recommendation in the answer's text, and date it.
- **Never invent a source.** A claim with no source is marked an assumption
  (PRD-R4).
- **Do not touch `ids.md`, the approval digests or the review reports.**
  `check` and `review` own them.

## 1. Find the project

Resolve the project directory as `${CLAUDE_PLUGIN_ROOT}/references/settings.md`
and `${CLAUDE_PLUGIN_ROOT}/references/project.md` say (the `project` skill
does this when the project does not exist yet; if none exists, tell the
operator to create one first and stop). Read `project.yaml` and `context.md`.

If `spec_set.status` is `approved`, the set is amended, not drafted: "Amending
an approved spec set" in `${CLAUDE_PLUGIN_ROOT}/references/project.md` defines
the two choices, sign off or full review. Say which choice the operator must
make, and make no edit to a spec before the operator has chosen. For an
amendment you make only the spec edit and its change-log entry in
`context.md` (step 3). Writing the amendment entry and digests, and the
sign-off or full-review step, belong to the `review` skill.

## 2. Gather context (PRD-R1)

Before drafting any spec, gather what the specs will cite: the relevant code
in the repository, documents the operator names, and the operator's own
statements. Read them; run read-only shell commands to find them.

Write what you gathered into `context.md`, which holds, in this order, the
statement, the sources, the numbered and dated decisions and the change log;
the format is "`context.md`" in
`${CLAUDE_PLUGIN_ROOT}/references/project.md` (TS-3.3).

- Keep the operator's statement in their own words. Add to it; never
  paraphrase it away.
- List each source as a file and line or section, a URL, or a document the
  operator named. A new project's `context.md` must list at least one source
  before the first spec is drafted. If the repository holds nothing relevant,
  the operator's statement is the source.
- Record each operator decision as a new numbered entry with its date. Never
  renumber and never edit an earlier decision: add a later one that
  supersedes it.

Ask the operator when the work is unclear enough that the specs would be
guesses, and end your reply. Otherwise go on and mark what remains unknown as
an assumption.

## 3. Draft or revise one spec

Follow `${CLAUDE_PLUGIN_ROOT}/references/specs.md` for what each of the five
specs holds (PRD-R2), the order they are drafted in, and the Mermaid rule for
diagrams (PRD-R5). Draft in that order, each spec from the ones before it.
Take the entry format of `tasks.md` from
`${CLAUDE_PLUGIN_ROOT}/references/tasks.md`.

Follow `${CLAUDE_PLUGIN_ROOT}/references/conventions.md` for every spec
(PRD-R3, PRD-R4, PRD-R6; TS-5.1, TS-5.2, TS-5.4):

- Give every section another document may refer to an ID, with the prefix of
  the file it is in. Never change an ID when you edit; add a new one instead
  of renumbering; withdraw a section by the stub the reference describes.
- Cite the source of every factual claim in the form the reference lists, or
  mark it `*Assumption:*`. Cite an operator decision only by a date that is in
  the decisions list of `context.md`.
- Give each spec "Open questions" and "Answered questions" sections, in that
  order, each question with an ID; write "None." in an empty one.
- The PRD carries its acceptance scenarios in a `gherkin` block.

When the operator asks for all five specs, draft them one after another in
the order above, and do not stop between them unless a question needs the
operator.

After each spec is written, append one change-log entry to `context.md`: the
date, the section IDs you added or changed, and one line saying why, citing
the decision number where there is one (TS-3.3, TS-22.5).

## 4. Record answers

When the operator answers an open question, move it from "Open questions" to
"Answered questions" in its spec as the conventions reference says: the
answer, its date and the ID of the section that implements it. If the answer
is a decision that other specs cite, also add it to the decisions list in
`context.md`. Change the sections the answer affects, and log the change as
in step 3.

## 5. Update the project

After any edit to a spec of a set that is not approved, set
`spec_set.status` in `project.yaml` to `drafting`, and change nothing else in
that file except the lenses in step 6. Leave `checked`, `reviewed`, `approved`
and `digests` as they are. When the set is approved (an amendment, step 1),
leave `spec_set.status` as it is as well: the `review` skill sets it as the
reference says once the operator has chosen sign-off or full review.

## 6. Suggest review lenses (TS-22.3)

When the spec set is drafted, suggest any extra spec review lenses that fit
this work, as the spec review reference defines (`references/spec-review.md`):
name each lens and say in one line why it fits. Record only the lenses the
operator confirms, in `spec_set.spec_lenses` in `project.yaml`. Suggesting
is not confirming: add none unasked.

## 7. Finish

Say which specs you drafted or changed, which questions are open, and that
the next step is the `check` skill. Do not run the check or approve anything.
