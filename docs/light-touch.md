# Planar Light Touch

A reference for using Planar to capture research, exploration, and learning
**without engaging the agent-driven workflow**. Skip `/pl-spec-draft`, skip
`/pl-spec-ingest`, skip the orchestrator. Use the four base capture verbs
directly. When the work matures into a real feature, you graduate to the
full workflow — the captured material drafts the spec for you.

This is the mode to use when:

- You're exploring something new (a language, a library, an approach) and
  don't know what the eventual feature shape is yet.
- You're learning by doing and want a durable record of what you tried,
  what worked, and why you made the calls you made.
- You're triaging an open-ended question and want a journal of partial
  findings before committing to a direction.
- The work might or might not become a real feature — and that's fine.

This is **not** the mode to use when:

- The work is a known feature with a clear shape — use `/pl-spec-draft`
  instead so the proper plan structure exists from day one.
- The work needs to coordinate with other agents or be propagated to
  external systems (Jira, GitHub Issues) — those workflows assume the
  full plan / task structure exists.

## The pattern in one paragraph

Create one **anchor plan** for your exploration with `status=active`.
Capture each insight as an **artifact**, each call you make as a
**decision**, each open uncertainty as a **question**, and each concrete
next step as a **task**. All four entity types link back to the anchor
plan. There are no milestones, no child plans, no spec documents, no
ingest pass. The plan is your journal; the entities are your captured
thinking. When you're ready to commit to a real feature, run
`/pl-spec-draft` and the drafter uses your captured material as input.

## The four verbs

These four verbs are all you need for light-touch capture.

### `planar artifact add`

For capturing **what you learned**. One artifact per investigation,
finding, comparison, or reference write-up.

```
planar artifact add "Survey of Zig CLI argument-parsing libraries" \
  --kind research \
  --plan <anchor-id> \
  --body @notes.md
```

Use `--kind research` for research findings. Future-you can filter them
with `planar artifact list --kind research`.

### `planar decision add`

For capturing **calls you made**. One decision per concrete choice —
"I picked X over Y because Z." Decisions are write-once; if you change
your mind later, record the new decision and run
`planar decision supersede <old-id> --by <new-id>` to mark the old one
superseded by it. This is the right shape:
you don't lose the original reasoning, you add the new one alongside.

```
planar decision add "Use Zig stdlib argv parsing instead of zig-cli" \
  --plan <anchor-id> \
  --body "zig-cli pulls in deps we don't need; stdlib is sufficient for our flag set."
```

### `planar question add`

For capturing **what you don't know yet**. Each unresolved uncertainty
becomes a queryable entity. Run `planar question list --status open --scope
<your-scope>` periodically to see what's still hanging.

```
planar question add "How do we handle SIGINT cleanup across coroutines in Zig?" \
  --body "Cobra gives us graceful shutdown automatically; Zig needs manual signal handling."
```

When you figure out the answer, `planar question answer <id> --answer "<resolution>"`
closes it and records the resolution.

### `planar task add`

For capturing **what's next**. Concrete experiments, sub-investigations,
files-to-read. Tasks have a status (`todo` / `doing` / `done` / `cancelled`)
so you can see what's queued, what's in flight, what's finished.

```
planar task add "Port `planar version` as the smallest possible Zig verb" \
  --plan <anchor-id> \
  --priority 50
```

Keep priorities loose. In light-touch mode you're not running a sprint;
the priority just helps you scan "what's the next thing to try."

## Where the body actually lives

The `--body` flag on every `add` and `update` verb writes content
**directly into the SQLite row**. No file is auto-created. After
capture, the body exists only in the database until you ask for it.

That's the right default for short-form capture — a sentence, a
paragraph, a one-line note. Fast, no file management, just a CLI
call.

For longer-form writing where you'd rather use your editor, the
workbench is the bridge:

```bash
# 1. Capture the entity (DB row only)
planar artifact add "Survey of Zig CLI libs" --plan 5 --body "Stub — expanding in workbench."

# 2. Materialize the workbench tree for the plan
planar workbench push 5

# 3. Edit the .md file directly in your editor
$EDITOR ~/.planar/workbench/<assoc>/p5-<slug>/<id>-<slug>.md

# 4. Sync edits back to the DB
planar workbench pull 5
```

The workbench writes one `.md` per entity under a per-plan directory.
Artifacts sit at the top level beside the plan's `README.md`; the other
kinds are organized by directory:

```
~/.planar/workbench/<assoc>/p<plan-id>-<plan-slug>/
├── README.md
├── 12-survey-of-zig-cli-libs.md
├── decisions/
│   └── 4-use-zig-stdlib-argv-parsing.md
├── questions/
│   └── 7-how-to-handle-sigint-cleanup.md
└── tasks/cross/
    └── 23-port-planar-version-as-smallest-zig-verb.md
```

Each file has a YAML frontmatter block (`entity_kind`, `entity_id`,
etc.) followed by the body. **Edit only the body**, then `pull` to
sync. The frontmatter is the contract that maps file → DB row;
mangling it breaks the sync.

### When to use each

| Use case | Use this |
|----------|----------|
| Capturing a one-liner ("decided X over Y") | `planar decision add ... --body "..."` |
| Capturing a quick finding (a paragraph or two) | `planar artifact add ... --body "..."` |
| Writing a long-form artifact in your editor | Capture stub with `--body`, then `workbench push` + edit + `pull` |
| Iterating on multiple entities in one editing session | `workbench push`, open the directory in your editor, `workbench pull` once |
| Updating a body without re-typing it | `planar artifact update <id> --body @new.md` |
| Wanting your notes git-trackable | The workbench tree is just files — back it up however you like |

Many short entities live their whole life in the DB and never need
a workbench file. That's fine — the workbench is opt-in, not
mandatory.

### Editor-first authoring (the dominant rhythm)

Plan 226 closed the friction gap. Every entity now has a unified
`<entity> edit <id>` verb that pushes (if needed), opens `$EDITOR`
on the workbench file, validates frontmatter mutations on save, and
pulls the result back into the DB — one command instead of three.

```
planar artifact edit 42       # opens $EDITOR on the artifact's workbench file
planar task edit 47           # same flow for tasks
planar question edit 12       # ... questions
planar decision edit 8        # ... decisions
planar scenario edit 3        # ... scenarios
```

Companion verbs share the same shape:

```
planar <entity> view <id>     # exec $PAGER on the workbench file
planar <entity> diff <id>     # unified diff: DB-rendered vs FS contents
planar workbench edit <plan>  # bulk: push, exec $EDITOR on the feature dir, pull on save
```

Bulk-review flows for sweeping through open questions or in-flight tasks:

```
planar question review [<plan>]   # REVIEW.md with every open question + originating artifact
planar task review     [<plan>]   # TASKS.md with editable Status/Priority/Next-action per task
```

Save the focal file (`REVIEW.md` / `TASKS.md`); each populated block
applies via the appropriate per-entity verb. Blank blocks skip.

Conflict surface: each `edit` verb auto-pulls the latest DB state
into the workbench before opening `$EDITOR`. Pass `--no-pull` to skip
that step when you know the workbench is already current and want to
avoid clobbering an in-progress edit. If a concurrent change lands
between open and save, the save reports the conflict via the standard
`workbench resolve` / `workbench status` flow.

## Setting up your anchor plan

One command. No spec ceremony.

```
planar plan create "Zig migration exploration" \
  --status active \
  --slug zig-exploration
```

`status=active` (not `draft`) signals "this work is happening, not
proposed." Skipping the draft phase is deliberate — drafts imply
review-and-commit; exploration doesn't.

You can have multiple active exploration plans simultaneously. Use
slugs that read naturally — they show up everywhere.

## Common recipes

### Capture an insight from the last hour

```
planar artifact add "<one-line summary>" --kind research --plan <id> \
  --body "<prose summary; what you tried, what worked, what didn't>"
```

Don't agonize over the body — first-pass thoughts are fine. You can
update the artifact later with `planar artifact update <id> --body @file`.

### Lock in a design choice as you make it

```
planar decision add "<the choice>" --plan <id> \
  --body "<the reasoning>"
```

The body matters more than the title here. Future-you reading this
six months later needs the "because" to make sense of it.

### Record a question you can't resolve right now

```
planar question add "<the question>" --body "<context; what you tried; why it's open>"
```

Worth running `planar question list --status open --scope <yours>` at the
start of each session — it surfaces stale questions you might be
able to answer now.

### Make a concrete next experiment a task

```
planar task add "<the experiment>" --plan <id> --priority 50
```

When you start working on it: `planar task update <id> --status doing`.
When done: `planar task done <id>`.

### Find everything you've captured today (or this week)

```
planar artifact list --plan <id>
planar decision list --plan <id>
planar question list --plan <id>
planar task list --plan <id>
planar tree               # hierarchical view of the cwd-derived scope
```

`planar plan show <id>` returns just that plan's own row; use the
per-kind list verbs above for its children. `planar tree` (invoked
from inside the scope, or with `--scope <slug>` / `--all-scopes`)
walks plans, tasks, artifacts, decisions, and questions hierarchically
across the whole scope.

### Review what you've learned

```
planar artifact show <id>
planar decision list --plan <id>     # browse the choices you've made
planar question list --status open          # what's still unresolved
```

## Graduating to a real plan

The signal: you start to know what the actual feature should be. At that
point, draft a real plan that builds on what you've already captured:

```
/pl-spec-draft "Migrate Planar CLI from Go to Zig"
```

The spec drafter has access to your exploration plan's artifacts,
decisions, and questions. The resulting spec:

- Cites your research artifacts (e.g., "Per the survey at [artifact:42]...")
- Pre-resolves questions you've already answered (no "Open Questions"
  section for things you've already decided)
- Inherits gotchas you've already discovered as risks
- Inherits your design choices as decisions

This is the synthesis pattern from plan 96, applied internally: from
exploration → committed feature spec. The cost of the exploration phase
pays off in a sharper, less hand-wavy spec.

You can also link the real plan back to the exploration plan via
`planar plan link <real-id> plan:<exploration-id> --relationship derives-from`,
so the provenance is queryable.

## What to NOT do in light-touch mode

- **Don't run `/pl-spec-draft` on the exploration itself.** Specs are
  for committed work; exploration is exploration. The spec ceremony
  is heavy enough that it'll discourage capture if you force it on
  every observation.
- **Don't run `/pl-spec-ingest`.** No need to decompose what's not yet
  a feature.
- **Don't worry about milestone hygiene.** The exploration plan stays
  flat (no child plans). It's a working journal, not a deliverable.
- **Don't agonize over artifact bodies.** First-pass thoughts captured
  in 30 seconds beat polished prose you never write. You can always
  `planar artifact update <id>` later.
- **Don't try to make decisions "feel finished."** Decisions can be
  superseded. Capture the call, capture the reasoning, move on.
- **Don't capture trivia.** Skip the "I figured out Zig syntax for
  `if`" stuff. Capture choices, findings, and gotchas. The bar is
  "would future-me want to know this?"

## Quick-reference cheat sheet

```bash
# Set up
planar plan create "<topic>" --status active --slug <slug>

# Capture as you go (artifact for what, decision for why, question for ?, task for next)
# --body writes directly to the DB. No file is created automatically.
planar artifact add "<title>" --kind research --plan <id> --body "<inline>"
planar artifact add "<title>" --kind research --plan <id> --body @<file>
planar decision add "<choice>" --plan <id> --body "<reasoning>"
planar question add "<question>" --plan <id> --body "<context>"
planar task add "<experiment>" --plan <id> --priority 50

# Materialize the workbench tree when you want to edit on disk
planar workbench push <plan-id>
$EDITOR ~/.planar/workbench/<assoc>/p<plan-id>-<slug>/<id>-<slug>.md
planar workbench pull <plan-id>

# Move task states
planar task update <id> --status doing
planar task done <id>
planar task cancel <id>

# Resolve questions
planar question answer <id> --answer "<resolution>"
planar question wontfix <id>

# Browse
planar tree                                      # hierarchical view of the cwd-derived scope
planar question list --status open                      # what's unresolved
planar decision list --plan <id>                 # all your choices
planar artifact list --plan <id>                 # all your captures

# Graduate to a real plan when ready
/pl-spec-draft "<the real feature title>"
planar plan link <real-id> plan:<explore-id> --relationship derives-from
```

## Things this mode is also useful for

Not just learning a new language. Light-touch capture works for:

- **Triaging an open-ended bug** — each finding is an artifact, each
  reproduction step is a task, each unknown is a question.
- **Vendor evaluation** — comparing options across multiple dimensions,
  capturing each finding, deciding at the end.
- **Spike work on a feature you might or might not build** — the
  "should we even do this?" investigation that happens before a real
  spec.
- **Reading a codebase you don't own** — each insight about how the
  code is structured becomes an artifact; questions about why
  decisions were made become questions you can answer if you
  eventually reach out.
- **Personal post-mortems** — each lesson is an artifact, each
  what-I-would-do-differently is a decision recorded against a plan
  scoped to the incident.

The shape generalizes: anywhere you're learning by doing and want a
durable record without the overhead of formal planning.
