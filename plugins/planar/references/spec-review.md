# Spec review

The single definition of the traceability rules, the spec review focuses and
lenses, and the spec review report format. Implements PRD-R36, PRD-R37 and
PRD-R38, TS-5.5 and TS-22, and layout entry LAY-1.5.12. Follow it whenever you
run the traceability check or write or read a spec review. The spec files and
their IDs are defined in ${CLAUDE_PLUGIN_ROOT}/references/specs.md, and the
settings that name the reviewers in ${CLAUDE_PLUGIN_ROOT}/references/settings.md
and ${CLAUDE_PLUGIN_ROOT}/references/profiles.md.

## Traceability rules

The check confirms, mechanically, that every requirement is built and tested
(PRD-R37, TS-5.5). A spec set that fails it does not go to review. Three rules:

- every requirement (`PRD-R<n>`) is in a task's `cites`, or is listed in
  `tasks.md` under "Requirements no task builds" with a reason;
- every requirement has a row in the testing spec's coverage section that
  names at least one check, or gives a reason there is none;
- every task has at least one entry in `cites`, and every check defined in the
  testing spec is in some task's `checks` or in the list of checks common to
  all tasks.

A failure of any rule fails the check. The check shows only that the links are
there; the consistency reviewer judges whether the coverage is real.

## Stage

The `review` stage requires a passing check, then dispatches the
`spec-reviewer` agent once for each entry in the profile's
`spec_review.reviewers` and once for each of the project's lenses (TS-22.1).
Each is given the project directory, the focus or lens, and the round number,
which is one more than the highest round in `reviews/`. A profile reviewer runs
on the resolved host binding its entry gives; a lens runs on the binding of the profile's first
spec reviewer. Each reviewer reports separately (PRD-R36) and writes
`reviews/spec-review-<n>-<focus>.md`, where `<n>` counts review rounds.

The status becomes `reviewed` only when every reviewer of the round approves;
any `request-changes` is the result. Then the operator is asked.

## Standard focuses

A profile lists its spec reviewers by focus. The three standard focuses
(PRD-R36, TS-22.2) are below; the focus `all` is the three in one report, and
the minimal profile has one reviewer covering `all`.

### intent

What the operator asked for, against what is specified (PRD-R38). The reviewer
works from the operator's recorded statement and decisions in `context.md`.

- For each entry in `context.md` (the statement and each dated decision) the
  report gives a table row naming the sections that carry it out, or `gap`. An
  entry that asks for several things gets a row for each.
- A row may read `deferred` when a later entry withdrew or postponed the item.
- It then lists anything specified that no entry asks for, and questions the
  specs should have put to the operator.
- Any `gap` row means `request-changes`.

This is distinct from the gap log, which is about missing tooling.

### consistency

Whether the five specs agree with each other, and whether what the
traceability check passed is real coverage. The reviewer reports:

- contradictions between the five specs;
- a requirement the tech spec does not implement;
- a tech-spec part no task builds;
- task order that cannot work;
- whether each named check could fail if the requirement it covers were not
  met.

### feasibility

Whether it can be built as written. The reviewer reports:

- steps that cannot be done as written;
- every factual claim about the platform or the codebase that is marked as an
  assumption or has no source, each as `spike: <what to try>`.

An unrun spike that a task depends on means `request-changes`.

## Lenses

A project can add lenses for what its work touches (PRD-R36, TS-22.3):

- `api`: every interface the work adds or changes: its contract, errors,
  versioning and callers;
- `security`: trust boundaries, inputs, secrets, permissions;
- `data`: stored data, its format, migration and rollback.

A lens is added for a project by `projects.<name>.spec_lenses` in settings, or
by `spec_lenses` in `project.yaml`. Each lens is a reviewer of its own with its
own report, under every profile. While drafting, `draft` suggests a lens when
the specs add or change an interface, cross a trust boundary or store data; if
the operator agrees it is recorded in `project.yaml`. A lens that finds nothing
in its subject says so and approves.

## Findings

Every finding cites section IDs, is ranked blocker, should-fix or note, and
says what would settle it (TS-22.4). A reviewer approves when it has no blocker
and its should-fix items can be handled as amendments. A `gap` row and an unrun
spike that a task depends on are blockers.

## Later rounds

From round 2, each reviewer is given the earlier reports for its focus and
first says, for each earlier finding, whether it is closed. It then reviews
only what changed since, which `review` lists for it from the change log in
`context.md` (TS-22.5).

## Report format

The report is `reviews/spec-review-<n>-<focus>.md`, with sections in this
order:

1. `# Spec review: <project>, round <n>, <focus>`.
2. From round 2 only, `## Earlier findings`: one bullet per earlier finding,
   with `closed` or `open` and a line of reason.
3. For `intent` (and `all`), `## Intent table`: a table with the columns
   `Entry`, `Item`, `Sections`; the last column names the sections that carry
   the item out, or reads `gap` or `deferred`. Then `## Not asked for` and
   `## Questions the specs should have asked`, each a list or "None.".
4. For `consistency` (and `all`), `## Consistency`: the items above, or "None.".
5. For `feasibility` (and `all`), `## Feasibility`: the items above, with each
   assumption as `spike: <what to try>`, or "None.". For a lens, the section
   is named for the lens and says so when it finds nothing in its subject.
6. `## Findings`: one bullet per finding, or "None.", as
   `- **F<k>** (<blocker|should-fix|note>) cites: <ID>. <what is wrong>. Settled by: <what would settle it>.`
   IDs run `F1`, `F2`, ... within the report, most serious first.

The last two lines are the verdict and the status line, in that order:

- `verdict: approve`: no blocker, and every should-fix can be handled as an
  amendment;
- `verdict: request-changes`: at least one blocker, or a should-fix that
  cannot be handled as an amendment;

then `status: complete`. Nothing follows them.

Example:

```markdown
# Spec review: widgets, round 1, intent

## Intent table

| Entry | Item | Sections |
| --- | --- | --- |
| statement | Show colour in the list | PRD-R2, TS-4.1 |
| decision 2026-11-02 | Sort by title | gap |

## Not asked for

None.

## Questions the specs should have asked

- Should a missing colour show a placeholder?

## Findings

- **F1** (blocker) cites: PRD-R3. The 2026-11-02 decision to sort by title is
  carried out by no section. Settled by: a requirement and a tech-spec section
  for the sort.

verdict: request-changes
status: complete
```
