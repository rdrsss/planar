# Knowledge: decisions, artifacts, annotations, questions

Durable project knowledge lives in four entity kinds plus typed links between them. Each
kind has its own verbs. There is no parallel knowledge store and no direct database path.

| Kind | Use for | Lifecycle |
|------|---------|-----------|
| Decision | A choice and its rationale. | `proposed` to `accepted`, `superseded` or `withdrawn`. |
| Artifact | A durable document: spec, ADR, design note, research. | Status field; kinds are fixed (see `planar artifact add --help`). |
| Annotation | An anchored review note on a file and line range. | Tag, resolve, dismiss, archive. |
| Question | An open uncertainty that blocks or shapes work. | `open` to `answered` or `wontfix`. |

## Resolving targets

1. `planar scope show --json` shows the scope a read or create will use. To act in
   another scope, pass `--scope <slug>`.
2. A typed reference such as `decision:9` goes straight to the matching
   `show <id> --json`. A name or phrase goes through the scoped `list ... --json` of that
   kind: exact title, then case-insensitive exact title, then case-insensitive substring.
   One kind's list does not find another kind's rows.
3. Zero candidates means nothing matches; several means a typed id is needed to pick one.
4. A link takes two typed endpoints and a relationship from the allowed set below.

## Scope behaviour of these verbs

- Creates (`decision add`, `artifact add`, `annotate add`, `question add`) resolve the
  scope of the new row from the cwd or `--scope`. Run from the owning repo.
- Of the existing-entity verbs here, only `decision accept` and `decision withdraw`
  compare your scope with the entity's (exit 5 on mismatch, no bypass). Every other
  update, including every `question` verb, `artifact update`, `annotate update` and
  `decision supersede`, writes without comparing. The stored scope is in
  `show --json`.
- On `artifact update` and `annotate update`, `--scope` is a patch field that moves the
  entity, not a permission.
- `annotate add` accepts `--plan` and `--task`, but does not check that those targets
  share the new annotation's scope. Create the annotation in the resolved scope and link
  it explicitly instead.
- Every `annotate` and `links` verb, `links list` included, and every question, decision
  and artifact write refuse inside a git worktree (exit 8). Run them from the parent
  checkout.

## Links

`planar links add <from-kind:id> <to-kind:id> --relationship <rel>` takes typed integer
ids only, never slugs. Allowed relationships: `derives-from`, `depends-on`, `addresses`,
`verifies`, `cites`, `supersedes`, `touches`. The old spelling `blocks` is refused; use
`depends-on`. Supported endpoints: `plan`, `task`, `question`, `test_scenario`, `artifact`,
`decision` and `annotation`; resolve a scenario through `planar scenario show` and an
annotation through `planar annotate show`.

- `links add` is unguarded on purpose and may cross scopes.
- Run `planar links list <kind:id> --json` before adding. An identical existing tuple is
  a successful no-op (skipped), not a failed duplicate.
- A `supersedes` link records a relationship only. It does not change either decision's
  status; it is not a status change.

## Recording a decision chain

1. Resolve the plan, the supporting artifact and any annotation target, with scopes.
2. `planar decision add "<title>" --body <text> --rationale <text>`; keep the returned id
   and read it with `planar decision show <id> --json`.
3. Link it, for example `decision:<id> addresses plan:<id>` and
   `decision:<id> cites artifact:<id>`.
4. An anchored note carries only the anchor facts actually known.
5. Verify with the `show` verbs and `planar links list decision:<id> --json`.

Each step is an independent write; a failed later step leaves the earlier ones in place.

## Artifacts and annotations

- Prefer `--from-file` for an existing document and `--body` for inline text.
- `artifact update --body @file` reads the file verbatim. For a workbench spec, pass a
  body-only file; see [preparation.md](preparation.md).
- Use annotations for anchored review notes, not general documents. After creating one,
  `planar annotate verify` reports whether its anchor is clean or has drifted.

## Questions

- Add a question with
  `planar question add "<short title>" --body <text>`, adding `--plan` when it belongs
  to one.
- Link it to what it blocks: `planar question link <id> task:<id> --relationship <rel>`.
- Close it with `planar question answer <id> --answer <text>` or
  `planar question wontfix <id> --reason <text>`. Answered
  questions appear in resume packets, so the answer should stand on its own.
- Read back with `planar question show <id> --json`. An empty filtered list is a no-op,
  not a warning.

## Promote and demote

`planar promote <kind:id> --to <association-slug>` moves a plan, task, question,
scenario, artifact or decision from its current scope to an association, typically when
personal work turns out to matter. `planar demote <kind:id>` returns it to global
personal scope; association-to-association moves go through `promote`. Both are planning
writes and refuse in a worktree.

- An entity already in the target scope is a skip; `show --json` gives its current scope.
- A promoted entity joins the target association's next workbench export.
- Verify with `planar <kind> show <id> --json`.
