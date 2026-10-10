# Planar CLI Reference

Reference for every `planar` subcommand. Authoritative current surface for the installed binary. For machine-readable help, use `planar <subcommand> --help`.

**Source of truth:** schema across `migrations/00001_foundation.up.sql` through `migrations/00041_contextual_annotation_threads.up.sql`. Every "schema effects" section below cites real columns from those migrations. The CLI surface is served by C++26 binaries built via CMake (see [docs/architecture.md](architecture.md) and [docs/toolchain-parity.md](toolchain-parity.md)); the Zig implementation under `zig/`, retained through the port as its parity oracle, was deleted at the M10 cutover. See [docs/architecture.md § Application tables](architecture.md#application-tables) for the migration-by-migration table inventory.

---

## Conventions

### Output Formats

- **Human mode (default):** line-oriented text, formatted for terminal reading. Columns may be elided for narrow terminals.
- **Machine mode (`--json`):** newline-delimited JSON objects, one per result row. Each object uses snake_case keys matching the underlying schema column names. Consumers may rely on field presence being stable within a major version.
- Commands that produce a single entity (`show`, `add`, `create`) emit one JSON object under `--json`.
- Commands that produce a list emit one JSON object per line.
- Commands that produce no data (e.g. `done`, `block`, `clear`) emit a single-line confirmation in human mode and `{"ok":true,"id":<id>}` under `--json`.

### Argument Patterns

- `<kind:id>` — a typed entity reference. Examples: `task:42`, `plan:7`, `question:3`, `artifact:12`, `decision:5`, `scenario:9`, `session:101`. The colon separates kind from integer id. Used wherever the command accepts a reference to any entity kind.
- `<plan-id>`, `<task-id>`, `<snapshot-id>`, `<link-id>`, `<event-id>` — shorthand for a bare integer id when the kind is fixed by context.
- `<slug>` — a human-readable identifier for associations and external systems; must match `[a-z0-9:._-]+`.
- `<system-slug>:<external-id>` — a namespaced external reference; example: `acme-jira:PROJ-1234`, `side-gh:owner/repo#42`.

### The `@<file>` free-text grammar

A free-text value documented below as "May be `@<file>`" is read from a file when its **first byte** is `@`: the file's bytes replace the value, raw. An `@` anywhere else in the value is literal text. An unreadable file is refused at exit `2` with `error: read <flag>: FileNotFound` (it does not name the path) before anything is written, so no row, session, or patch results. The verbs with the grammar: `plan create`/`plan update --summary`, `task add`/`task update --body`, `question add --body`, `scenario add --body`, `decision add --body`/`--rationale`, `artifact add`/`artifact update --body`, `handoff --note`, `capture note <body>`, `capture command --outcome`, `capture end --summary`, `capture snapshot --note`/`<body>`. `question answer --answer` and `annotate add`/`annotate update --body` are deliberately literal; every other flag takes literal text.

### Scope Shorthand

`--scope` accepts these forms wherever it appears:

| Form | Meaning |
|------|---------|
| `--scope repo:<slug>` | Scope to the named project slug. |
| `--scope assoc:<slug>` | Scope to the named association slug. |
| `--scope <slug>` | Backward-compatible shorthand for an association slug. |
| `--scope global` | Global / personal scope (no repo or association filter). |

Use the `repo:` prefix for project rows. A bare slug is always parsed as an
association for compatibility with existing workspace and project-association
flows.

**Mutating commands** (`task add`, `plan create`, `question add`, `scenario add`, `decision add`, `artifact add`) resolve scope through `resolve_for_write`: explicit flag (threaded through verbatim, not validated) → meta-workspace arm → cwd derivation with most-specific-wins, using the same specificity ranking as reads (task 6746, so a member repo outranks an org containing it) → otherwise **global scope, exit 0**. Cwd is the only default; there is no ambient stack. Note the last step: an unregistered cwd does NOT refuse on the write path, though it does on the read path. See [Write resolution in `docs/concepts.md`](./concepts.md#write-resolution) for the measured behaviour, including the `project_unassociated` exception. `link`, `unlink` and `planar-ext ext create` declare `--scope` and never read it, and `planar-ext ext propagate` reads and discards it; none of the four resolves a write scope.

> **Stale claim (task 6140, 2026-09-09).** The "workspace root with member
> projects" refusal named above is FABRICATED against the current C++
> binary — see the stale-section banner on the workspace-root refusal in
> [`docs/concepts.md § Scope`](./concepts.md#scope) for the measurement.
> Filed as task 6675; not corrected here.

See that section for the full algorithm, the membership-aware cross-scope guard, and the specificity ranking.

Source annotations on success: `[from flag]`, `[from cwd]`. The resolved scope and source are printed on success (human output) and included as `scope_kind`, `scope_id`, and `scope_source` fields in `--json` output. There is no flag to opt out of strict resolution — see [Cross-scope guard § No escape hatch](#cross-scope-guard) for the actual remedies.

**Query commands** (`plan list`, `task list`, `question list`, `scenario list`, `decision list`, `artifact list`, `search`, `tree`) use `ResolveForRead`, which derives the in-scope set from cwd: at a workspace root the org plus every member project; at a member project root the most specific registered repo wins, with longer `projects.root_path` matches beating shorter parent roots. Outside any registered scope, reads refuse unless `--scope global` is passed explicitly. `--scope` on a query is a filter, not a strict pick.

### Exit Codes

Taken from `src/cmd/planar/exit.cppm`, which is the authoritative table for
the `planar` binary.

| Code | Meaning | Raised by |
|------|---------|-----------|
| `0` | Success. | — |
| `1` | Generic failure: entity **not found**, an unmapped error, a busy source. | `not_found`, `generic_failure`, `busy_source` |
| `2` | **Bad input**: invalid value, invalid entity ref, and every **parse failure** — unknown flag, missing argument, missing flag value, too many positionals. | `invalid_input`, `invalid_entity_ref`, `parse_error` |
| `3` | Operational-plane **sync conflict**; requires an explicit `sync resolve`. | `sync_conflict` |
| `5` | **Cross-scope write refused** (see the cross-scope guard above). | `scope_mismatch` |
| `6` | Precondition conflict: slug conflict, or the entity already exists. | `slug_conflict`, `already_exists` |
| `7` | Database schema is **newer** than this binary supports. | `schema_version_ahead` |
| `8` | **Worktree-gate refusal**: a planning verb was run from inside a git worktree. Outside the `domain_error_kind` bucket table entirely — `planar.cmd.planar.worktree_gate` returns it directly, before the parser even runs, so it fires even when the invocation's flags would also fail to parse. `--scope` does not bypass it. | `planar.cmd.planar.worktree_gate::check` |
| `10` | **Update available** — not a failure. Only `planar update --check` returns it, when the installed release differs from the latest one (see [Domain: `update`](#domain-update)). Outside the `domain_error_kind` table; the handler returns it through `passthrough_code`. No other verb returns 10 of its own (`workflow run` passes a workflow's own status through verbatim). | `planar update --check` |
| `64` | Handler is **not implemented** — a placeholder verb. NOT `EX_USAGE`. | `not_implemented` |
| `130` | **Interrupted by `SIGINT`** (128 + 2): a plain `planar update` caught it before its installer hand-off, removed its download directory and released the mutation lock. Outside the `domain_error_kind` table, through `passthrough_code`. | `planar update` |
| `143` | **Interrupted by `SIGTERM`** (128 + 15), as for `130`. | `planar update` |

**Usage errors exit `2`, not `64`.** An unknown flag, a missing required
positional and a missing flag value are all `parse_error` on this binary. This
is oracle-matched: 536 pinned refusals in
`scripts/parity-data/6316-refusal-differential.jsonl` agree on `2` for those
arms. `64` is reserved for a verb whose handler does not exist yet.

### The code for a usage error depends on WHICH BINARY

The four binaries do not share one table, deliberately — `planar-watch`'s
`exit.cppm` header records why a shared, binary-parameterized helper was
removed (a call site one token short silently applied the operator binary's
policy). Measured, and confirmed live:

| condition | `planar` | `planar-agent` / `-watch` / `-ext` |
|-----------|----------|------------------------------------|
| parse failure (unknown flag, missing arg) | `2` | `1` |
| schema version **behind** | `1` | `7` |

So a script that branches on an exit code must know which binary produced it.
Do not port a `planar` expectation onto `planar-agent` unchanged.

### Examples and exit codes in `--help` and `schema`

Every leaf verb of `planar`, `planar-agent`, `planar-watch` and `planar-ext` ends its `--help` page with an `Examples:` section (one invocation per line) and an `Exit codes:` section (`<code>  <meaning>` per line). `planar task update --help` is the model:

```text
Examples:
  planar task update 42 --status doing
  planar task update 42 --title "Write the migration" --priority 1

Exit codes:
  0  Success.
  1  Generic failure: entity not found, an unmapped error, or a busy source.
  ...
```

Both sections come from the same table as the `docs.examples` and `docs.exitCodes` arrays of the `schema` catalog (`docs.exitCodes` holds `{"code":N,"meaning":"..."}` objects), so the page and the catalog cannot disagree; `planar schema --command "planar task update"` prints the data. A group such as `planar task` has no sections and `"examples":[]`. The table lives in each binary's `docs.cppm` (`src/cmd/<binary>/docs.cppm`) beside its summary table, and a leaf lists only the codes its handler can return, in ascending order. `planar` and `planar-agent` leaves other than `schema`, `completion`, `version` and `help` each carry at least one example. `planar-execute` publishes its exit codes and examples in `schema` only, because its usage banner is hand-written. `make cli-usage-check` validates every `docs.examples` entry against the live catalogs the same way it validates the invocations in these documents, and reports how many it checked.

### Error envelope tags — one vocabulary per binary (task 6902)

Every failing `--json` invocation on `planar` or `planar-agent` writes an
ADDITIVE one-line JSON error envelope to **stdout**, alongside the existing
pinned `error: <verb>: <Tag>` text on stderr (task 6844, decision 1145):

```
{"error":{"verb":"<verb>","tag":"<tag>"}}
```

**One JSON document per stream (task 6903).** A handful of `planar`
handlers write their own JSON payload to stdout and THEN fail (`audit
handoff-readiness --json` at exit 1 once the pass rate is below
threshold; `templates validate --json` at exit 2 once an issue is found).
For those, the envelope above is NOT appended — the handler's own payload
is the entire document. Appending a second JSON document on the same
stream would break a `json.loads(proc.stdout)` consumer, which expects
exactly one value. This is detected once, at the dispatch site, by
tracking whether the handler wrote anything to stdout before failing —
not by special-casing individual verbs — so it applies uniformly to any
handler with this shape, present or future. A handler that writes
NOTHING before failing (the common case — `resume validate <missing
task>`, `audit session <missing id>`, and most others) is unaffected: the
envelope is still the only document on stdout.

**The two binaries spell `<tag>` in two DIFFERENT, DELIBERATE vocabularies.
A script that branches on `tag` must know which binary produced the
envelope, exactly as it already must for the exit code above.**

| Binary | `<tag>` vocabulary | Source | Example |
|--------|---------------------|--------|---------|
| `planar` | snake_case `domain_error_kind` enumerator names — one distinct name per kind (`kind_name`, `src/cmd/planar/exit.cppm`). | `kind_name(err.kind)`, always. | `not_found`, `busy_source`, `invalid_input`, `parse_error`, `scope_mismatch` |
| `planar-agent` | CamelCase Zig-style engine tags — the same spelling the pinned `error: <verb>: <Tag>` stderr line already ends in (`derive_tag`, `src/cmd/planar-agent/exit.cppm`). Falls back to `kind_name(err.kind)` only when the handler's own text does not already end in a bare CamelCase tag. | `derive_tag(err)`. | `ClaimNotFound`, `Busy`, `IllegalTransition`, `QueryFailed` |

The split is intentional, not an oversight: `planar-agent`'s tag is reused
directly from the handler's own Zig-style failure text because that text is
already strictly more specific than `planar`'s `domain_error_kind`, which
collapses many distinct engine failures into `generic_failure` (task 6843's
`Busy`/`QueryFailed` split exists precisely so that distinction is not lost
again behind a shared envelope shape). `planar` never emits a CamelCase tag,
and `planar-agent` never emits a bare `domain_error_kind` name except as its
documented fallback. **Do not assume the same condition produces the same
tag spelling on both binaries** — the busy-source case is the sharpest
example: a lock held past the timeout emits `Busy` from `planar-agent
heartbeat --json` and `busy_source` from `planar task update --json` for
the same underlying SQLite busy condition (see
`src/cmd/planar/handlers/task/task_busy_leaf.t.cpp` and the `Busy`/`QueryFailed`
scenario in this feature's test spec).

### Capture Behavior

Every command that writes to the database appends a `session_entries` row to the current active session for the affected task (if the command is task-scoped). If no active session exists, one is created automatically with the current process's vendor string. This is the automatic capture described in the tech spec — agents do not need to call a separate save-state command.

Commands that are read-only (list, show, status) do not create sessions or session entries unless the command is explicitly capture-oriented (see [capture domain](#domain-capture)).

Vendor identity for auto-created sessions is taken from the `PLANAR_VENDOR` environment variable (default: `"cli"`). A vendor session id is taken from `PLANAR_VENDOR_SESSION_ID` if set.

**Session auto-creation policy:** one session per `(vendor, vendor_session_id)` tuple. If `vendor_session_id` is unset (env var missing), one session per process for the duration of the active task. The `vendor` is read from `$PLANAR_VENDOR`; `vendor_session_id` is read from `$PLANAR_VENDOR_SESSION_ID`. Both are unset at install time; vendor harnesses are responsible for setting them on invocation.

---

### Version metadata

`planar version`, `planar-agent version`, `planar-watch version` and
`planar-ext version` print six tokens: program, shortened sha (with
`+dirty` when applicable), build date, `cxx`, compiler, and release tag.
An untagged build prints `dev` in the last position. The sha token remains
the install manifest's `build_id`.

Each accepts `version --json`, which emits `release`, full `sha`, `date`,
boolean `dirty`, and `compiler`. Version queries do not open the database.
`planar-execute` has no version verb.

## Top-Level Usage

```
planar [GLOBAL FLAGS] <subcommand> [subcommand args]
```

**Bare invocation.** When `planar` is called with no subcommand it prints the root help/usage text and exits 0, on a TTY or otherwise. There is no interactive terminal interface: `planar explore` is a reserved leaf that currently prints its own help page and exits 0 — see [Domain: `explore`](#domain-explore).

### Global Flags

The database path is overridden via the `PLANAR_DB` environment variable (not a `--db` flag); it defaults to `~/.planar/planar.db`.

| Flag | Description | Default |
|------|-------------|---------|
| `--json` | Emit machine-readable newline-delimited JSON instead of human text. | off |

`--json` is the only flag accepted before the subcommand; the root node declares no others. There is no quiet flag (neither the long nor the `-q` short form) and no `-v` / `-vv` verbosity flags — passing any of them fails at parse time with `error: <cmd>: The following argument was not expected: <flag>`, exit 2 — no `--no-scope-check` — see [Cross-scope guard § No escape hatch](#cross-scope-guard) — and no `--color` / `--no-color` flag or `NO_COLOR` handling: the binary does not colorize any output today, so there is no color mode to select or suppress (task 6140; these were documented aspirationally and never implemented / were removed without the docs following). Every leaf that supports JSON output also declares its own `--json`, which is the form the per-command tables below show.

Global flags must appear before the subcommand. They are not repeated in per-command option tables below.

---

## Cross-scope guard

The cross-scope guard is a refusal mechanism that runs at the top of ten specific verbs — those that walk from a parent entity to derived rows, or mutate one existing entity in a way the audit judged worth guarding (see [Guarded verbs](#guarded-verbs); it is NOT on every mutating verb). It is layered on top of the strict write-scope resolver described in [Scope Shorthand](#scope-shorthand): the resolver picks the operator's intended scope; the guard then compares that against the *target entity's* stored `(scope_kind, scope_id)` and refuses if they disagree. See [docs/concepts.md § Cross-scope guard](./concepts.md#cross-scope-guard) for the conceptual model.

### Resolution → guard pipeline

For every guarded verb:

1. Look up the target entity's stored scope. Global-scoped entities short-circuit and are accepted from any operator scope.
2. Resolve the operator's write scope through the cwd-primary algorithm (explicit `--scope` flag → cwd derivation, most-specific-wins → otherwise global).
3. Compare the two scopes with the membership-aware coverage rule: equality matches, and an operator scope `assoc:<org>` covers any entity scoped to one of the org's member projects (via `project_associations`). Otherwise refuse with exit 5. The reverse direction (operator project, entity org) does not cover.

### Refusal message

The refusal is a multi-line message naming both scopes and listing three remediation paths:

```
<kind> <id> belongs to <entity-scope> but the resolved write scope is <op-scope>.

       This usually means your cwd is inside a different repo than the
       <kind>'s owning project/association. To proceed:

         a. cd into a directory inside <entity-scope>, OR
         b. pass --scope <entity-scope> explicitly

       Refusing cross-scope write without explicit operator intent
```

The exit code is `5` (`domain_error_kind::scope_mismatch`) — the same code as the ambiguous-workspace-root refusal described in [Scope in `docs/concepts.md`](./concepts.md#scope) — see e.g. `spec ingest --apply`'s and `feedback triage set`'s refusal messages, both worded "Refusing cross-scope write; pass --scope \<slug\> or cd into the right repo." An unresolvable `--scope` slug (`SlugNotFound`) is the only scope-related failure that exits `1`. No database writes occur before either refusal.

### No escape hatch

There is no flag that downgrades a cross-scope-guard refusal to a warning. `--no-scope-check` does not exist on this binary — it is absent from every command's flag set in `planar schema`, and passing it fails at parse time with exit 2 (`error: <cmd>: The following argument was not expected: --no-scope-check`), not with the scope guard's own exit code. (An engine-layer `guard_write` bypass parameter of the same shape exists at `src/engine/identity/scope.cppm` and is unit-tested, but no `cmd/` handler ever calls it with `true` — no verb can reach it from the CLI.)

The only remedies are:

- `cd` into a directory inside the entity's owning scope, or
- pass `--scope <entity-scope>` explicitly.

There is no way to force a genuinely cross-scope write through a flag; do it by resolving to the correct scope instead.

### `--scope` is four different flags

The remedy above — "pass `--scope <entity-scope>`" — only works on verbs where
`--scope` selects the operator's **write scope**. It does not mean the same
thing everywhere. Measured at task 6075:

| Meaning | Effect of passing `--scope` | Verbs |
|---------|----------------------------|-------|
| **Write-scope selector** | Sets the operator scope the write resolves under. This is the one the guard's remedy assumes. | `plan create`, `task add`, `task update`, `artifact add`, `decision add`, `question add`, `scenario add`, `annotate add`, `closure compute`, `spec ingest`, `feedback triage set`, `audit publish-decision` |
| **Patch field** | **Reassigns the entity's stored scope** — it moves the row. | `plan update`, `artifact update`, `annotate update` |
| **Read filter** | Restricts which rows are listed. | `plan list`, `artifact list`, `decision list`, `question list`, `scenario list`, `annotate list`, `search`, `tree`, `dashboard`, `health` |
| **Inert** | Accepted, parsed, discarded. | `planar-ext ext propagate` |

The consequence worth stating plainly: on `plan update`, `artifact update` and
`annotate update` there is **no way to authorize a cross-scope write with
`--scope`**, because `--scope` there performs the move instead. Those verbs
are unguarded today, so nothing refuses — but the documented escape hatch
does not exist for them, and could not be offered without a new flag.

### Guarded verbs

**Measured against the source at task 6075 (2026-09-11) and re-measured at
task 6825 (2026-09-21), not aspirational.** Ten verbs carry a cross-scope
check; eight `guard_with_membership` call sites implement them (`sync push`
and `sync pull` share one; `decision accept` and `decision withdraw` share
their transition helper). Every other mutating verb — including several this
table claimed for years — performs **no** scope comparison.

| Verb | Guards against | Comparison | Call site |
|------|---------------|-----------|-----------|
| `spec ingest <plan> --apply` | `plan` | membership-aware | `src/cmd/planar/handlers/spec/command.cpp` |
| `feedback triage set <id>` | owning entity | membership-aware | `src/cmd/planar/handlers/feedback/command.cpp` |
| `audit publish-decision <id>` | `decision` | membership-aware | `src/cmd/planar/handlers/audit/command.cpp` |
| `decision accept <id>` | `decision` | membership-aware | `src/cmd/planar/handlers/decision/command.cpp` |
| `decision withdraw <id>` | `decision` | membership-aware | `src/cmd/planar/handlers/decision/command.cpp` |
| `task update <task-id>` | `task` | membership-aware | `src/cmd/planar/handlers/task/command.cpp` |
| `closure compute` | resolved write scope | membership-aware | `src/cmd/planar/handlers/closure/command.cpp` |
| `planar-ext sync push <link\|kind:id>` | `plan` or `task` | membership-aware | `src/cmd/planar-ext/handlers/sync/sync.cpp` |
| `planar-ext sync pull <link\|kind:id>` | `plan` or `task` | membership-aware | `src/cmd/planar-ext/handlers/sync/sync.cpp` |
| `planar-ext sync resolve <event-id>` | the event's target entity | membership-aware | `src/cmd/planar-ext/handlers/sync/sync.cpp` |

`--all` forms of `sync push` / `sync pull` are unguarded; the bulk fan-out is
an explicit opt-in.

#### One comparison, since decision 1121

Every guarded call site uses the cmd-layer `guard_with_membership`, which
accepts an entity at `repo:<project>` when the operator's write scope is an
association that project belongs to (via `project_associations`). The reverse
direction — operator `repo:`, entity `assoc:` — still refuses with exit 5.

This was not always true. Until decision 1121 (task 6735), `task update` and
`closure compute` called `engine::identity::check_scope_guard` directly —
strict equality after `assoc:` normalization — so an `assoc:<org>` →
`repo:<member>` write was **accepted** by `spec ingest --apply` and
**refused** by `task update`, from the identical working directory. Task 6075
measured the split; decision 1121 widened those two to match the rest, on the
grounds that strict equality could not tell "my own member repo" from "an
unrelated repo" and refused both identically. The guard stops a write leaking
sideways, not an association acting on its own member.

### Verbs with no scope guard

Every mutating verb not listed in the table above writes without comparing the
operator's scope to the entity's. That includes ones a reader would reasonably
expect to be guarded:

`plan update`, `plan step add/done/skip`, `task done`, `task reopen`,
`task block`, `question edit`, `scenario edit`, `decision edit`,
`decision supersede`, `artifact update`, `annotate update`,
`planar-ext ext create --from`, `planar-ext ext propagate-one`, `link`, `unlink`.

Editions of this document before 2026-09-11 listed most of those as guarded.
They were not, and are not. Treat the table above as the whole set.

### Explicitly unguarded by design

Verbs audited and deliberately left unguarded. The absence is recorded so future readers do not interpret it as an oversight.

| Verb class | Rationale |
|-----------|-----------|
| `*_link` (e.g. `task link`, `plan link`, `links add`, `links remove`, `task touches add`, `task touches remove`) | `entity_links` is cross-entity by design; polyrepo `touches`/`derives-from` edges legitimately cross scopes. |
| `*_add` / `*_create` (e.g. `task add`, `plan create`, `question add`, `scenario add`, `decision add`, `artifact add`, `ext register`) | New entity resolves its own `--scope` through the write resolver; `--plan` / `--parent` are references, not scope inheritance. |
| `sync push --all` / `sync pull --all` | Bulk fan-outs that the operator opts into explicitly via `--all`. |
| `planar-ext ext propagate` | `external_links` carries no scope column, so there is nothing to compare against; `--scope` is accepted and discarded. |
| Read-only verbs (`show`, `list`, `status`, `audit trail`, `audit session`, `tree`, `health`) | Reads do not corrupt state; audit-from-anywhere is the common case. |
| Identity bucket (`assoc *`, `init`, `promote`, `demote`, `scope show`, `scope suggest`, `workspace *`) | Associations *are* scope; `promote`/`demote` deliberately cross scopes (that is their purpose). |
| Operator-state (`handoff`, `capture *`, `resume`, `resume validate`) | Manages vendor-session rows in `sessions` / `context_snapshots` / `handoffs`; not project-scoped entities. The polyrepo handoff workflow (session in repo A references a task in repo B) is supported by design. |

Each guarded verb's per-command entry below cross-references this section.

---

## Domain: `init`

Initializes the Planar database and registers the current directory as a project. Must be run before any other command on a fresh install. Subsequent runs on an already-initialized database are safe (idempotent).

---

### `planar init`

**Synopsis:**
```
planar init [--name <text>] [--slug <slug>] [--skip-project] [--allow-no-repo] [--force]
```

**Description:** Idempotently ensure the config file exists (via `config init`), apply the embedded migration corpus (compiled into the binary at configure time from `migrations/` via `cmake/generate_migrations.cmake`) against the configured database (creating it if absent), then register the current working directory as a project if it is not already registered. Human output names the `assoc create` and `assoc add` commands that establish the project's planning scope; `--json` retains the stable initialization result shape without prose guidance. Order: ensure config → apply migrations → create project row.

**Workspace-shape guardrail:** when cwd has no `.git` of its own but contains one or more immediate child directories that do, `planar init` refuses with a hint pointing at `planar workspace init`. A bare init in a polyrepo workspace directory would otherwise register a semantically-wrong project row for the workspace itself. Pass `--allow-no-repo` to override and register the non-repo cwd as a standalone project anyway (`--force` is a different flag — see the options table). See [Domain: `workspace`](#domain-workspace) and [concepts.md § Workspace](concepts.md#workspace).

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--name <text>` | Human-readable project name. | Basename of current working directory. |
| `--slug <slug>` | Explicit project slug; with `--force`, names the existing registration to repoint. | Slug derived from the directory basename. |
| `--skip-project` | Apply migrations only; do not register a project. | off |
| `--allow-no-repo` | Proceed even when cwd has no `.git` but contains child repos (escape hatch for the workspace-shape guardrail). | off |
| `--force` | Overwrite an existing project registration: repoint the slug's existing row (`root_path`, name, git remote) under the same id instead of the default insert-or-ignore, which leaves an existing row unchanged. Use after moving a checkout. | off |

**Output (human):**
```
planar initialized
  db:      ~/.planar/planar.db
  schema:  <integer version from schema_migrations>
  project: <slug> (id: <id>)
  next:    `planar assoc create project:<slug> --kind project`
           `planar assoc add project:<slug> <root-path>`
```

**Output (`--json`):**
```json
{"ok":true,"db":"<path>","schema_version":<int>,"project_id":<id>,"project_slug":"<slug>"}
```

**Schema effects:**
- Applies all pending migrations (recorded in `schema_migrations`).
- Inserts or ignores into `projects(slug, name, root_path, git_remote)`.

**Capture:** No session created.

**Exit codes:**
- `1` — database path not writable; or the workspace-shape guardrail refused (cwd has no `.git` but contains child repos and `--allow-no-repo` was not passed).
- `2` — migration application failed.

---

## Domain: `scope`

Inspects the cwd-derived scope and proposes associations the current directory could be wired into. Scope itself is no longer managed as ambient state: it is a pure function of `(--scope flag, cwd, db schema)`. To change resolved scope, `cd` into a registered scope or pass `--scope <slug>` to the target verb.

Plan 153 M5 removed the `planar scope use`, `planar scope pop`, and `planar scope clear` subcommands and dropped the underlying `active_scope` table (migration `0009_drop_active_scope.sql`). The removed verbs survive as exit-1 redirect stubs that print a one-line corrective message pointing at this section; they will be removed entirely in a future release.

---

### `planar scope show`

**Synopsis:**
```
planar scope show [--scope <slug>]
```

**Description:** Print the scope Planar resolves for the current working directory. Plan 153 M5 rewrote this verb: it no longer reads the (removed) `active_scope` table — it runs `ResolveForRead` against cwd (optionally overridden by `--scope <slug>` to preview an override) and prints the resulting set.

The resolved set follows the read-resolver contract:
- At a workspace root, the org plus every member project.
- At a member project root, the project (cross-repo `touches` participants are surfaced at query time, not in `scope show`).
- Outside any registered scope, the verb prints `resolved scope: none` and instructs the operator to `cd` or pass `--scope`.

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--scope <slug>` | Preview an explicit override. Resolves through the same parser the write resolver uses. | unset (cwd) |

**Output (human, member project cwd):**
```
resolved scope (from cwd):
  project:billing  (root_path: /home/user/work/acme/billing)

To override, pass --scope <slug> to any verb, or cd into a
different registered scope.

(The active scope stack was removed in plan 153 M5; scope is now
derived from your current working directory.)
```

**Output (human, workspace root cwd):**
```
resolved scope (from cwd):
  org:acme  (root_path: /home/user/work/acme)
  project:billing  (root_path: /home/user/work/acme/billing)
  project:shipping  (root_path: /home/user/work/acme/shipping)

To override, pass --scope <slug> to any verb, or cd into a
different registered scope.
```

**Output (human, outside any registered scope):**
```
resolved scope: none (cwd not inside any registered Planar scope)

cd into a registered scope or pass --scope <slug> to any verb.
```

**Output (`--json`):** A single object:
```json
{
  "resolved_scopes": [
    {"kind":"association","id":3,"slug":"acme","name":"Acme","kind_label":"org"},
    {"kind":"association","id":7,"slug":"billing","name":"Billing","kind_label":"project"}
  ],
  "source": "cwd",
  "cwd": "/home/user/work/acme"
}
```

`source` is one of `"cwd"`, `"flag"`, or `"none"`. The outside-any-scope branch emits `"resolved_scopes": []` and `"source": "none"`.

**Schema effects:** Reads `associations`, `project_associations`, `projects`. No writes.

**Capture:** None (read-only).

---

### `planar scope use|pop|clear`

Removed in plan 153 M5. Invoking any of these verbs prints a one-line corrective message on stderr and exits 2:

```
error: `planar scope use` was removed in plan 153 M5; the active scope stack is gone. Pass --scope <slug> to individual verbs, or cd into a registered scope. Run `planar scope show` to inspect the cwd-derived scope.
```

To switch scope: `cd` into the target project / workspace root, or pass `--scope <slug>` to the verb in question. To inspect: `planar scope show`.

---

### `planar scope suggest`

**Synopsis:**
```
planar scope suggest
```

**Description:** Inspect the current working directory and list the associations the cwd project is a member of. Read-only — there is no ambient scope to apply suggestions to. To act on a suggestion, pass `--scope <slug>` to the target verb or `cd` into that scope. (Companion to `assoc detect`, which proposes new association *creation*; `scope suggest` proposes existing associations the cwd qualifies for.)

**Output (human):**
```
suggested scope based on cwd:
  org:rdrsss  (from git remote)
  path:company  (from parent dir)
```

**Output (`--json`):** One object per proposal:
```json
{"slug":"org:rdrsss","association_id":3,"reason":"from git remote"}
{"slug":"path:company","association_id":11,"reason":"from parent dir"}
```

When no proposals match, `--json` emits `{"proposals":[]}`.

**Schema effects:** Reads `associations`, `project_associations`, `projects`. No writes.

**Capture:** None.

---

## Domain: `assoc`

Manages associations — the many-to-many tags that group repos into named scopes. The subcommand name is `assoc` only; there is no `association` alias (`planar association list` fails at parse time, exit 2).

---

### `planar assoc list`

**Synopsis:**
```
planar assoc list [--kind <kind>] [--json]
```

**Description:** List all known associations.

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--kind <kind>` | Filter by association kind: `org`, `project`, `client`, `personal`, `ad-hoc`, `host`, `path`, `lang`. | all |

There is no `--workbench` filter; the `workbench` column is reported but not filterable.

**Output (human):**
```
slug             kind       workbench  members
org:acme         org        yes (→ ~/work/acme-export)  3
project:billing  project    no         2
side:weekend     personal   no         1
```

**Output (`--json`):** One object per association:
```json
{"id":3,"slug":"org:acme","name":"Acme Org","kind":"org","auto_detected":false,"workbench_enabled":true,"workbench_export_path":"~/work/acme-export","created_at":"...","updated_at":"..."}
```

**Schema effects:** Reads `associations`, `project_associations`.

**Capture:** None.

---

### `planar assoc create <slug>`

**Synopsis:**
```
planar assoc create <slug> [--name <text>] [--kind <kind>]
```

**Description:** Create a new association with the given slug.

**Arguments:**

| Argument | Description |
|----------|-------------|
| `<slug>` | Unique association slug, e.g. `org:acme`, `project:billing`. Must match `[a-z0-9:._-]+`. |

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--name <text>` | Human-readable name. | Derived from slug. |
| `--kind <kind>` | One of `org`, `project`, `client`, `personal`, `ad-hoc`. | `ad-hoc` |

**Output (human):**
```
created association: org:acme (id: 3)
```

**Output (`--json`):**
```json
{"ok":true,"id":3,"slug":"org:acme"}
```

**Schema effects:** Inserts into `associations(slug, name, kind, auto_detected=0, workbench_enabled=0)`.

**Capture:** None.

**Exit codes:**
- `1` — slug already exists (`UNIQUE` violation).
- `1` — kind not in the allowed CHECK set.

---

### `planar assoc add <slug> <repo-path>`

**Synopsis:**
```
planar assoc add <slug> <repo-path>
```

**Description:** Add a repo (by its filesystem path) to an association.

**Arguments:**

| Argument | Description |
|----------|-------------|
| `<slug>` | Association slug. |
| `<repo-path>` | Filesystem path to the project root. Must already be registered as a project. |

**Output (human):**
```
added ~/work/billing to org:acme
```

**Schema effects:**
- Reads `projects` by `root_path`.
- Inserts into `project_associations(project_id, association_id, source='user')`.

**Capture:** None.

**Exit codes:**
- `1` — association slug not found.
- `1` — repo path not registered as a project (run `init` in that directory first).

---

### `planar assoc remove <slug> <repo-path>`

**Synopsis:**
```
planar assoc remove <slug> <repo-path> [--json]
```

**Description:** Remove a repo from an association.

**Schema effects:** Deletes from `project_associations` where `project_id` matches `repo-path` and `association_id` matches `slug`.

**Exit codes:**
- `1` — association or project not found, or membership does not exist.

---

### `planar assoc members <slug>`

**Synopsis:**
```
planar assoc members <slug>
```

**Description:** List all projects (repos) that are members of the named association, with their source (auto-detected or user-defined).

**Output (human):**
```
members of org:acme (3):
  ~/work/billing    source: user
  ~/work/api        source: auto:git-remote
  ~/work/frontend   source: auto:path
```

**Output (`--json`):**
```json
{"project_id":1,"slug":"billing","root_path":"~/work/billing","source":"user","created_at":"..."}
```

**Schema effects:** Reads `project_associations`, `projects`, `associations`.

**Capture:** None.

---

### `planar assoc detect [--apply]`

**Synopsis:**
```
planar assoc detect [--apply] [--json]
```

**Description:** Inspect the current working directory's git remote host/org and parent path, then propose new association slugs using the conventional auto-tag rules (`host:`, `org:`, `path:`, `lang:`). Without `--apply`, prints proposals only. With `--apply`, creates proposed associations that do not already exist and adds the current project to them.

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--apply` | Create proposed associations and add current project. | off |

**Output (human):**
```
proposed associations:
  host:github.com   (from git remote host)  [will create]
  org:rdrsss        (from git remote org)   [already exists, will add]
  path:projects     (from parent dir)        [will create]
```

**Schema effects (without `--apply`):** Reads only. No writes.
**Schema effects (with `--apply`):**
- Inserts into `associations(slug, name, kind, auto_detected=1)` for each new association.
- Inserts into `project_associations(project_id, association_id, source='auto:git-remote' | 'auto:path' | 'auto:lang')`.

**Capture:** None.

---

## Domain: `plan`

Plans are the top-level structured intent for a body of work. They may be hierarchical (via `parent_plan_id`) and contain ordered steps (`plan_steps`) that optionally materialize as tasks.

---

### `planar plan create <title>`

**Synopsis:**
```
planar plan create <title> [--scope <scope>] [--parent <plan-id>] [--summary <text>] [--slug <slug>] [--status <status>]
```

**Description:** Create a new plan with the given title under the cwd-derived write scope, or the explicitly specified scope. When cwd resolves to a registered project with no association, implicit creation is refused instead of silently falling back to global scope. Create and add an association as directed by `planar init`, or pass `--scope global` when global ownership is intentional.

**Arguments:**

| Argument | Description |
|----------|-------------|
| `<title>` | Plan title (required, free-form text). |

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--scope <scope>` | Override scope for this entity. See [scope shorthand](#scope-shorthand). | cwd-derived write scope |
| `--parent <plan-id>` | Parent plan id for hierarchical plans. | none |
| `--summary <text>` | One-paragraph summary. May be a file path prefixed with `@`. | none |
| `--slug <slug>` | Explicit slug for the new plan. | none |
| `--status <status>` | Initial status. An unknown value fails with `unknown status '<value>'`, exit 1. | `draft` |

**Output (human):**
```
plan 7: "Implement billing module"  [draft]  slug:implement-billing-module  (scope: association:3 [from cwd])
```

**Output (`--json`):**
```json
{"ok":true,"id":7,"title":"Implement billing module","slug":"implement-billing-module","status":"draft","scope_kind":"association","scope_id":3,"scope_source":"cwd"}
```

**Schema effects:** Inserts into `plans(scope_kind, scope_id, title, summary, status='draft', parent_plan_id)`.

**Capture:** Appends `session_entries` row with `prefix='action'` and body summarizing plan creation.

**Exit codes:**
- `1` — `--parent` plan id not found.
- `1` — scope not resolvable.
- `5` — cwd resolves to a registered project with no association and `--scope` was omitted.

---

### `planar plan show <plan-id>`

**Synopsis:**
```
planar plan show <plan-id>
```

**Description:** Show a plan's details, its steps (with status), and direct child plans.

**Output (human):**
```
plan 7: "Implement billing module"  [active]
scope: association:org:acme
summary: Build the payment gateway integration.

steps:
  1. [done]        Design the data model
  2. [in-progress] Implement the API layer  → task:42
  3. [pending]     Write integration tests

child plans: none
```

**Output (`--json`):**
```json
{"id":7,"title":"...","summary":"...","status":"active","scope_kind":"association","scope_id":3,"parent_plan_id":null,"created_at":"...","updated_at":"...","steps":[{"id":1,"ordinal":1,"body":"Design the data model","status":"done","task_id":null},...],"children":[]}
```

**Schema effects:** Reads `plans`, `plan_steps`, `tasks` (for step task links).

**Capture:** None (read-only).

**Exit codes:**
- `1` — plan id not found.

---

### `planar plan list`

**Synopsis:**
```
planar plan list [--scope <scope>] [--status <status>] [--parent <plan-id>] [--touches <repo-slug>]
```

**Description:** List plans matching the given filters. Defaults to the cwd-derived scope set and all non-abandoned, non-done statuses.

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--scope <scope>` | Filter by scope. See [scope shorthand](#scope-shorthand). | cwd-derived read set |
| `--status <status>` | Filter by status: `draft`, `active`, `paused`, `done`, `abandoned`. Repeatable. | `draft,active,paused` |
| `--parent <plan-id>` | Only show children of this plan. | all plans |
| `--touches <repo-slug>` | Return plans scoped to this repo plus plans with `entity_links(relationship='touches', to_kind='repo')` for this repo. | off |

**Output (human):**
```
id   status   scope                 slug                title
7    active   assoc:billing         billing-module      Implement billing module
12   draft    repo:platform-core    auth-refactor       Refactor authentication
```

The `scope` column shows where the plan lives: `global`, `repo:<slug>`, or `assoc:<slug>`. Column width auto-sizes to the longest label in the result set.

**Output (`--json`):** One object per plan (same fields as `plan show` minus step detail).

**Schema effects:** Reads `plans`.

**Capture:** None.

---

### `planar plan update <plan-id>`

**Synopsis:**
```
planar plan update <plan-id> [--title <text>] [--slug <slug>] [--summary <text>] [--status <status>] [--parent <plan-id>]
```

**Description:** Update mutable fields on a plan. Status changes are validated against the plan transition matrix: `draft → active`, `active → {paused, done, abandoned}`, `paused → active`; `done` and `abandoned` are terminal. Illegal moves exit non-zero and leave the plan unchanged. There is no `--force` flag for plans — terminal plans have no operator escape path via this verb.

Note: `plan recompute-status` deliberately bypasses this matrix (it is an engine-internal aggregate roll-up, not an operator transition). Direct operator status changes always go through this verb and are matrix-checked.

**Scope guard:** NONE. `plan update` performs no scope comparison; the write proceeds from any cwd. Note also that `--scope` on this verb is a **patch field** that reassigns the plan's scope, not a write-scope override. See [Cross-scope guard](#cross-scope-guard).

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--title <text>` | New title. | unchanged |
| `--slug <slug>` | New slug. Must remain unique within the plan's slug namespace. | unchanged |
| `--summary <text>` | New summary. May be `@<file>`. | unchanged |
| `--status <status>` | New status. Must be a legal transition from the current status per the matrix: `draft → active`, `active → {paused, done, abandoned}`, `paused → active`. | unchanged |
| `--parent <plan-id>` | Reparent the plan. `--parent 0` clears the parent. | none |

**Schema effects:** Updates `plans(title, slug, summary, status, updated_at)`.

**Capture:** Appends `session_entries` row with `prefix='action'`.

**Exit codes:**
- `1` — plan id not found.
- `1` — invalid status value or illegal transition.

---

### `planar plan recompute-status`

**Synopsis:**
```
planar plan recompute-status (--plan <plan-id> | --all) [--json]
```

**Description:** Re-fire the [plan-status auto-promotion invariant](concepts.md#plan) (plan 304) against a stored plan, applying the same transition matrix that runs inside `task.Add` / `task.Update` / `task.Done` / `task.Reopen` / `task.Cancel` / `task.Block`. Each plan is processed in its own transaction; the matrix is idempotent so re-running against an already-correct DB is a no-op.

This verb is the recovery surface for:
- Plans that drifted under older binaries (before plan 304 shipped).
- Plans whose task statuses were edited via direct SQL or a code path that bypasses `task.Update`.
- Any case where `planar plan show <p>` reports a status that disagrees with the task aggregate.

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--plan <plan-id>` | Recompute one plan by id. | none |
| `--all` | Recompute every plan in the resolved scope. Mutually exclusive with `--plan`. | off |

Either `--plan` or `--all` is required.

**Output:**

```
plan 287: draft → done
plan 288: draft → done
plan 289: done (no change)

recomputed 15 plans; 8 transitioned
```

(The `--all` form prints one line per plan that transitions; idempotent no-ops are silent unless invoked with `--plan` on a single plan.)

**Schema effects:** Updates `plans(status, updated_at)` per transition. Emits one `session_entries` row with `prefix='note'` and a body beginning `plan_status:` per transition (per the plan 304 §Audit trail convention).

**Exit codes:**
- `0` — success (whether or not any transitions occurred).
- `1` — user error: neither `--plan` nor `--all` supplied, or both supplied, or plan not found.

---

### `planar plan closeout <plan-id>`

**Synopsis:**
```
planar plan closeout <plan-id> [--dry-run] [--json]
```

**Description:** Delivery-evidence gate for plan closeout. Evaluates whether a plan is safe to close (the DB hard gate) and, in apply mode (no `--dry-run`), marks it `done` when the gate passes.

This is an **operator verb** (`planar` binary only — not `planar-agent`). It is the explicit operator action to close an anchor plan that `recompute-status` cannot auto-close, and the safety net for any plan where the operator wants to confirm delivery evidence before closing.

**Hard gate (DB-evidence; all three must pass):**

1. **All tasks terminal** — every task on the plan (and recursively on all descendant plans) is `done` or `cancelled`. Open tasks (`todo`/`doing`/`blocked`) block.
2. **All descendant plans terminal** — every child plan (recursively via `parent_plan_id`) is `done` or `abandoned`. Open descendants block.
3. **No live claims** — no `active`, non-expired `agent_work_claims` on the plan's tasks. Expired/stale claims do **not** block — they are reported as warnings.

Cancelled tasks are **terminal history** — counted in the audit output but they do not block closeout. This is the authoritative statement that satisfies the cancelled-terminal contract.

**Advisory git-evidence (reported, never blocks):**

For each distinct `(repo_root, branch, head_sha_at_claim)` tuple from `agent_work_claims` joined to the plan's tasks, the verb performs best-effort git checks:
- Detects the remote's target branch (`git symbolic-ref --short refs/remotes/origin/HEAD`, fallback to `main`/`master`).
- `head_sha_at_claim` ancestry: `git merge-base --is-ancestor` — a weak "base is in target" signal, labeled as such.
- Branch merged check: `git branch --merged <target>`.
- Branch absent (deleted post-merge or abandoned): reported as `"branch absent — inconclusive"`.
- No locality data on any claims: `"no commit attribution — inconclusive (hardens once session-commit capture is wired)"`.
- Git unavailable or not a repo: `"git-evidence unavailable"`.

Git-evidence failures never block apply.

**A plan already `done` or `abandoned` (task 6889).** `closeout` on an already-terminal plan
short-circuits to `ready:true, applied:false` before evaluating the gate rules — there is
nothing left to close. `hard_evidence.tasks` / `.descendants` / `.claims` are still the REAL
counts for that plan (collected the same as a live evaluation), so `--dry-run`/`--json` on an
already-closed plan still tells the operator what closeout found, not an all-zero
placeholder. Only `git_evidence` (empty) and `epic_merge` (unset, even with `--check-merge`)
are skipped on this path — advisory locality data that was genuinely never collected, unlike
the hard-evidence counts.

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--dry-run` | Evaluate and report only; never writes. Always exits 0 so callers can read the structured `{ready, blocked_by}` JSON before deciding. | off |
| `--check-merge` | Include advisory epic-branch merge roll-up: for each contributing branch from `agent_work_claims`, report how many are merged to the target branch. Never blocks. Absent branches are inconclusive. | off |
| `--json` | Emit structured JSON. | off |

**JSON shape:**

```json
{
  "plan": 42,
  "ready": true,
  "applied": false,
  "hard_evidence": {
    "tasks":       { "open": 0, "done": 5, "cancelled": 1 },
    "descendants": { "open": 0, "terminal": 2 },
    "claims":      { "live": 0, "stale": 0 },
    "finalization_tasks": 2
  },
  "blocked_by": [],
  "git_evidence": [
    {
      "repo_root": "/home/user/myrepo",
      "branch": "feature/foo",
      "target_branch": "main",
      "base_merged": true,
      "branch_merged": true,
      "note": "base-merged=true (weak signal); branch-merged=true"
    }
  ],
  "epic_merge": {
    "target_branch": "main",
    "total_branches": 3,
    "merged_count": 2,
    "note": "2 of 3 contributing branch(es) merged to main (advisory; absent branches inconclusive)"
  },
  "warnings": []
}
```

**Field notes:**
- `hard_evidence.finalization_tasks` — count of tasks on the plan (and descendants) whose slug begins with `finalize-`, `merge-`, or `reconcile-`. These are tasks the janitor/orchestrator creates to track merge or reconciliation work. Advisory labeling only; finalization tasks follow the same terminal rules as any task and do NOT affect the hard gate.
- `epic_merge` — present when `--check-merge` is passed; `null` otherwise. Also `null` when no locality data exists in `agent_work_claims`. The field is advisory and never affects the gate.
- `epic_merge.merged_count` — branches confirmed merged to the target; branches that no longer exist locally are excluded as inconclusive (not counted as merged or unmerged).

**Semantics:**
- `applied=true` — the plan was marked `done` in this invocation.
- `applied=false` — either `--dry-run` was passed, the plan was already terminal, or the hard gate failed.
- `ready=true` + already terminal → no-op (`applied=false`); exit 0.
- `ready=false` + `--dry-run` → exit 0 (preview; caller reads the JSON to decide).
- `ready=false` + apply (no `--dry-run`) → non-zero exit; plan unchanged.

**Schema effects:** On apply, updates `plans(status, updated_at)` and records an `audit_log` row. No new table is created.

**Exit codes:**
- `0` — gate passed (applied, already terminal, or dry-run — including blocked dry-runs).
- `1` — plan not found.
- `3` — hard gate blocked (apply path only; blocked_by non-empty); plan unchanged.

---

### `planar plan next <plan-id>`

**Synopsis:**
```
planar plan next <plan-id> [--include-claimed] [--include-stale] [--json]
```

**Description:** Bucketed claim-aware "what's next on this plan" view. Returns every task on the plan classified into one of four buckets:

| Bucket | Meaning |
|--------|---------|
| `available` | Task status `todo` (or `doing` without an active claim) and ready to be pulled. |
| `claimed` | Task has an active unexpired entry in `agent_work_claims`. |
| `stale` | Task has a `stale` claim, or an `active` claim whose lease has expired without a reconcile pass. |
| `blocked` | Task status `blocked`, OR a `todo`/`doing` task carrying an outbound `depends-on` edge (`entity_links`) to a task whose status is not `done`/`cancelled`. |

Applies the SAME dependency-exclusion clause `planar-agent pull`/`peek` apply (task 6841 / decision D6): a task with an open `depends-on` blocker never lands in `available`. This is a distinct query from `pull`/`peek`'s selector — not literally shared code — but the two agree on which rows are dispatchable, so an operator reading `plan next` sees the same eligibility an agent's `pull`/`peek` would (task 6898). `plan next` additionally folds a routing-packet-readiness gate into `blocked` that `pull`/`peek` do NOT apply (they exclude on the dependency clause only, per D6). This is the operator's read surface; agents call `planar-agent peek` / `pull`. There is no `planar agent` subcommand by design — agent observability lives on `planar-watch` (M8) and ritual writes live on `planar-agent`.

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--include-claimed` | Show the claimed bucket in text mode. (`--json` always carries every bucket.) | off |
| `--include-stale` | Show the stale bucket in text mode. | off |
| `--json` | Emit JSON instead of text. | off |

**JSON shape:**

```json
{
  "plan_id": 12,
  "available": [Task, ...],
  "claimed":   [{"task": Task, "claim": ClaimRow}, ...],
  "stale":     [{"task": Task, "claim": ClaimRow}, ...],
  "blocked":   [Task, ...],
  "summary": {
    "available": 3, "claimed": 1, "stale": 0, "blocked": 1, "done": 5
  }
}
```

`ClaimRow` is the canonical `agent_work_claims` row shape including the locality columns (`repo_root`, `branch`, `head_sha_at_claim`, `dirty_at_claim`) and worktree columns (`worktree_id`, `worktree_path`). The selector includes tasks from the plan and every descendant plan. An applicable claim is chosen with `task` claims taking precedence over `plan_step` claims, then the nearest ancestor `plan` claim.

**Exit codes:**
- `0` — success, including empty / all-done plans.
- `1` — plan id not found, or invalid integer.

---

### `planar plan recommend-strategy <plan-id>`

**Synopsis:**
```
planar plan recommend-strategy <plan-id> [--json] [--closure-source declared|derived]
```

**Description:** Read-only execution-strategy recommender. Computes the parallel-eligible subset of a plan's open (`todo`) tasks by applying the six parallel-eligibility rules and reports the eligible subset plus the serialized remainder with per-task exclusion reasons. This is the single source of truth for parallelizability (decision 370) consumed by the orchestrator and the parallel-fan-out gate; the rules are not re-derived elsewhere. Writes nothing.

Two open tasks are parallel-eligible iff **all six** hold:

| Rule | Condition |
|------|-----------|
| 1 | No `blocked_by` chain (transitive `depends-on` closure) to another not-done task in the plan. |
| 2 | Disjoint touch set. A task's touch set is the file paths it declares via `task touches add <task> <repo> --path <p>` (`task_touch_paths`), with the coarse repo slug (`entity_links` `touches`) used only for repos that have no path-level declaration. Path detail refines the coarse signal: two tasks editing different files in the same repo are disjoint (eligible). An **empty** touch set is treated as "touches everything" and is never eligible. Two tasks whose touch sets intersect **both** drop (drop-both-on-tie). Eligibility is only as complete as the declared touches — declaring touches accurately is operator/orchestrator hygiene (the omission failure mode is safe: an undeclared task serializes rather than falsely parallelizing). |
| 3 | No schema migration touched — any task touching `migrations/*.sql` serializes (migration numbering is linear). Unilateral drop. |
| 4 | No singleton authoritative file touched — `CLAUDE.md`, `AGENTS.md`, `docs/cli-reference.md`, `docs/architecture.md`. Unilateral drop. |
| 5 | No `open` question linked to the task. |
| 6 | No `proposed` decision linked to the task. |

A task may be excluded by multiple rules; every rule it trips is listed in `excluded_by`.

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--json` | Emit JSON instead of text. | off |
| `--closure-source declared\|derived` | Rule-2 overlap signal: `declared` (default, `task_touches`) or `derived` (computed closure). | `declared` |

**JSON shape:**

```json
{
  "plan_id": 12,
  "parallel_eligible": [ { "id": 1, "slug": null, "title": "..." } ],
  "serialized": [
    { "id": 5, "slug": null, "title": "...",
      "excluded_by": [ { "rule": 3, "reason": "excluded by rule 3: touches migrations/00099_x.sql" } ] }
  ],
  "summary": { "open_tasks": 6, "eligible": 3, "serialized": 3, "fan_out_available": true },
  "recommended_note": "parallel-fanout available: 3 eligible tasks"
}
```

`summary.fan_out_available` is `true` when at least two tasks are eligible.

**Exit codes:**
- `0` — success, including plans with no eligible tasks.
- `1` — plan id not found, or invalid integer.

---

### `planar plan descendants <plan-id>`

**Synopsis:**
```
planar plan descendants <plan-id> [--json]
```

**Description:** Emit the anchor plan's full subtree — child plans and tasks — in dependency-topological order (anchor → child plans → tasks). Read-only; queries and reports, no writes.

The topological ordering follows `parent_plan_id` chains for plans. A task reaches the subtree either by carrying the `plan_id` of a plan in the walk or through a `derives-from` edge in `entity_links`; a task arriving by both routes for the same plan appears once, but one linked to several plans in the walk appears once per plan. (Until task 6307 only the edge route was read, so a task attached the ordinary way — `task add --plan`, which writes no edge — was missing from the output. This sentence described the intent correctly the whole time; the code did not.) Use this verb to enumerate the complete work graph for a plan before feeding it to a propagation or orchestration step.

**Arguments:**

| Argument | Description |
|----------|-------------|
| `<plan-id>` | Anchor plan id (integer). |

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--json` | Emit the subtree as JSON instead of human text. | off |

**Output (human):**
```
plan:7 "Add Checkout RPC" [active]
  plan:8 "Protos Changes" [active]
    task:21 "Define CheckoutRequest proto" [todo]
    task:22 "Regenerate stubs" [todo]
  plan:9 "API Layer" [active]
    task:23 "Implement handler" [todo]
```

**Output (`--json`):**
```json
{
  "plan_id": 7,
  "title": "Add Checkout RPC",
  "nodes": [
    {"kind":"plan","id":7,"title":"Add Checkout RPC","status":"active","parent_plan_id":null,"depth":0},
    {"kind":"plan","id":8,"title":"Protos Changes","status":"active","parent_plan_id":7,"depth":1},
    {"kind":"task","id":21,"title":"Define CheckoutRequest proto","status":"todo","plan_id":8,"depth":2},
    {"kind":"task","id":22,"title":"Regenerate stubs","status":"todo","plan_id":8,"depth":2},
    {"kind":"plan","id":9,"title":"API Layer","status":"active","parent_plan_id":7,"depth":1},
    {"kind":"task","id":23,"title":"Implement handler","status":"todo","plan_id":9,"depth":2}
  ]
}
```

**Schema effects:** Reads `plans`, `tasks`. No writes.

**Capture:** None (read-only).

**Exit codes:**
- `0` — success (including plans with no descendants).
- `1` — plan id not found, or invalid integer.

---

### `planar plan step add <plan-id> <body>`

**Synopsis:**
```
planar plan step add <plan-id> <body> [--after <ordinal>] [--scope <scope>]
```

**Description:** Append a new step to a plan. By default inserts at the end. `--after <ordinal>` inserts after the specified ordinal, renumbering subsequent steps.

**Scope guard:** NONE. `plan step add` performs no scope comparison against the parent plan. See [Cross-scope guard](#cross-scope-guard).

**Arguments:**

| Argument | Description |
|----------|-------------|
| `<plan-id>` | Target plan id. |
| `<body>` | Step description text. |

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--after <ordinal>` | Insert after this step ordinal. | appends at end |
| `--scope <scope>` | Declared and never read by the handler. | none |

**Output (human):**
```
step 3 added to plan 7
```

**Output (`--json`):**
```json
{"ok":true,"id":3,"plan_id":7,"ordinal":3,"body":"Draft the schema","status":"pending","task_id":null,"created_at":"…","updated_at":"…"}
```
The `ok` sentinel matches every other mutation verb; `status` is one of `pending` / `done` / `skipped`, and `task_id` is non-null once the step is linked.

**Schema effects:** Inserts into `plan_steps(plan_id, ordinal, body, status='pending')`.

**Capture:** Appends `session_entries` row with `prefix='action'`.

**Exit codes:**
- `1` — plan id not found.

---

### `planar plan step done <step-id>`

**Synopsis:**
```
planar plan step done <step-id> [--scope <scope>]
```

**Options:**

| Flag | Description |
|------|-------------|
| `--scope <scope>` | Declared and never read by the handler. |

**Description:** Mark a plan step as done.

**Scope guard:** NONE. `plan step done` performs no scope comparison. See [Cross-scope guard](#cross-scope-guard).

**Output (`--json`):**
```json
{"ok":true,"id":3,"plan_id":7,"ordinal":3,"body":"Draft the schema","status":"pending","task_id":null,"created_at":"…","updated_at":"…"}
```
The `ok` sentinel matches every other mutation verb; `status` is one of `pending` / `done` / `skipped`, and `task_id` is non-null once the step is linked.

**Schema effects:** Updates `plan_steps(status='done', updated_at)`.

**Capture:** Appends `session_entries` row with `prefix='action'`.

**Exit codes:**
- `1` — step id not found.

---

### `planar plan step skip <step-id>`

**Synopsis:**
```
planar plan step skip <step-id> [--scope <scope>]
```

**Options:**

| Flag | Description |
|------|-------------|
| `--scope <scope>` | Declared and never read by the handler. |

**Description:** Mark a plan step as skipped.

**Scope guard:** NONE. `plan step skip` performs no scope comparison. See [Cross-scope guard](#cross-scope-guard).

**Output (`--json`):**
```json
{"ok":true,"id":3,"plan_id":7,"ordinal":3,"body":"Draft the schema","status":"pending","task_id":null,"created_at":"…","updated_at":"…"}
```
The `ok` sentinel matches every other mutation verb; `status` is one of `pending` / `done` / `skipped`, and `task_id` is non-null once the step is linked.

**Schema effects:** Updates `plan_steps(status='skipped', updated_at)`.

**Capture:** Appends `session_entries` row with `prefix='action'`.

**Exit codes:**
- `1` — step id not found.

---

### `planar plan step link <step-id> <task-id>`

**Synopsis:**
```
planar plan step link <step-id> <task-id> [--scope <scope>]
```

**Options:**

| Flag | Description |
|------|-------------|
| `--scope <scope>` | Declared and never read by the handler. |

**Description:** Associate a plan step with the task that materializes it. Sets `plan_steps.task_id`. Use `entity_links` (via `plan link`) for richer relationships.

**Output (`--json`):**
```json
{"ok":true,"id":3,"plan_id":7,"ordinal":3,"body":"Draft the schema","status":"pending","task_id":null,"created_at":"…","updated_at":"…"}
```
The `ok` sentinel matches every other mutation verb; `status` is one of `pending` / `done` / `skipped`, and `task_id` is non-null once the step is linked.

**Schema effects:** Updates `plan_steps(task_id)`.

**Exit codes:**
- `1` — step or task not found.

---

### `planar plan link <plan-id> <to-kind:to-id> --relationship <kind>`

**Synopsis:**
```
planar plan link <plan-id> <to-kind:to-id> --relationship <kind> [--json] [--scope <scope>]
```

**Description:** Create an `entity_links` row linking the plan to another entity.

**Arguments:**

| Argument | Description |
|----------|-------------|
| `<plan-id>` | Source plan id. |
| `<to-kind:to-id>` | Target entity reference, e.g. `artifact:3`, `decision:8`, `task:12`. |

**Options:**

| Flag | Description | Required |
|------|-------------|----------|
| `--relationship <kind>` | One of `derives-from`, `depends-on`, `addresses`, `verifies`, `cites`, `supersedes`. | yes |
| `--scope <scope>` | Declared and never read by the handler. | no |

**Schema effects:** Inserts into `entity_links(from_kind='plan', from_id, to_kind, to_id, relationship)`.

**Capture:** Appends `session_entries` row with `prefix='action'`.

**Exit codes:**
- `1` — source or target entity not found.
- `1` — relationship not in allowed CHECK set.
- `1` — link already exists (UNIQUE constraint).

---

## Domain: `task`

Tasks are the discrete units of work. They may belong to a plan (`plan_id`) or another task (`parent_task_id`), and carry the `next_action` field required by `resume validate`.

---

### `planar task add <title>`

**Synopsis:**
```
planar task add <title> [--plan <plan-id>] [--parent <task-id>] [--scope <scope>] [--priority <n>] [--body <text>] [--due <date>] [--next-action <text>] [--editor] [--slug <slug>]
```

**Description:** Create a new task.

**Arguments:**

| Argument | Description |
|----------|-------------|
| `<title>` | Task title (required). |

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--plan <plan-id>` | Associate with a plan. | none |
| `--parent <task-id>` | Make a sub-task of another task. | none |
| `--scope <scope>` | Override scope. See [scope shorthand](#scope-shorthand). | cwd-derived write scope |
| `--priority <n>` | Integer priority (lower is higher). | `100` |
| `--body <text>` | Task body / description. May be `@<file>`. | none |
| `--due <date>` | Due date in ISO 8601 format (e.g. `2026-05-15`). | none |
| `--next-action <text>` | The immediate next concrete action. Required for `resume validate` to pass. | none |
| `--no-auto-promote` | Skip the [plan-status auto-promotion invariant](concepts.md#plan) (plan 304) for this operation. Escape hatch for scripted migrations that don't intend the plan-level transition. | off |
| `--editor` | Declared default-true. The `$EDITOR` body flow is not implemented: with no `--body` and stdout on a TTY the verb refuses (exit 1) and asks for `--body <text>` or `--editor=false`. | on |
| `--slug <slug>` | Explicit slug for the new task. `tasks.slug` is globally unique. | none |

**Output (human):**
```
task 42: "Implement payment gateway API"  [todo]  (priority: 50, plan: 7, scope: association:3 [from cwd])
```

**Output (`--json`):**
```json
{"ok":true,"id":42,"title":"Implement payment gateway API","status":"todo","plan_id":7,"priority":50,"scope_kind":"association","scope_id":3,"scope_source":"cwd"}
```

**Schema effects:** Inserts into `tasks(scope_kind, scope_id, plan_id, parent_task_id, title, body, status='todo', priority, next_action, due_at)`.

**Capture:** Appends `session_entries` row with `prefix='action'`.

**Exit codes:**
- `1` — plan or parent task not found.
- `1` — scope not resolvable.

---

### `planar task show <task-id>`

**Synopsis:**
```
planar task show <task-id>
```

**Description:** Show full task details including plan position, next action, sub-tasks, and entity links.

**Output (human):**
```
task 42: "Implement payment gateway API"
status:      doing
priority:    50
plan:        7 "Implement billing module"
scope:       association:org:acme
next action: Wire Stripe webhook handler
due:         2026-05-20

body:
  Build the REST adapter for Stripe's payment intent API.

sub-tasks:   none
links:
  cites artifact:3 "Billing Tech Spec"
```

**Output (`--json`):**
```json
{"id":42,"title":"...","body":"...","status":"doing","priority":50,"next_action":"...","plan_id":7,"parent_task_id":null,"scope_kind":"association","scope_id":3,"due_at":"...","created_at":"...","updated_at":"...","links":[{"relationship":"cites","to_kind":"artifact","to_id":3}]}
```

**Schema effects:** Reads `tasks`, `plans`, `entity_links`.

**Capture:** None (read-only).

**Exit codes:**
- `1` — task not found.

---

### `planar task list`

**Synopsis:**
```
planar task list [--scope <scope>] [--status <status>] [--plan <plan-id>] [--priority-max <n>] [--touches <repo-slug>]
```

**Description:** List tasks matching the given filters. Defaults to the cwd-derived scope set and open statuses.

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--scope <scope>` | Filter by scope. | cwd-derived read set |
| `--status <status>` | Filter: `todo`, `doing`, `blocked`, `done`, `cancelled`. Repeatable. | `todo,doing,blocked` |
| `--plan <plan-id>` | Filter to a specific plan. | all |
| `--priority-max <n>` | Only show tasks with priority ≤ n. | none |
| `--touches <repo-slug>` | Return tasks scoped to this repo plus tasks with `entity_links(relationship='touches', to_kind='repo')` for this repo. Composes with `--scope` to narrow the result set. | off |

**Output (human):**
```
id   status   scope            pri  title
42   doing    assoc:billing    50   Implement payment gateway API
43   todo     repo:web-app     100  Write unit tests
```

The `scope` column shows where the task lives: `global`, `repo:<slug>`, or `assoc:<slug>`. Column width auto-sizes to the longest label in the result set.

**Output (`--json`):** One object per task (subset of `task show` fields, without entity_links).

**Schema effects:** Reads `tasks`.

**Capture:** None.

---

### `planar task update <task-id>`

**Synopsis:**
```
planar task update <task-id> [--title <text>] [--body <text>] [--status <status>] [--priority <n>] [--next-action <text>] [--due <date>] [--plan <plan-id>] [--force] [--reason <text>] [--editor] [--slug <slug>]
```

**Description:** Update mutable fields on a task. Status changes are validated against the per-entity transition matrix in `policy.status.check`; illegal moves exit non-zero and leave the task unchanged. Legal status moves: `todo → {doing, blocked, cancelled}`, `doing → {todo, blocked, done, cancelled}`, `blocked → {todo, doing, done, cancelled}` (`blocked → todo` added at task 6441, to requeue an unblocked task without claiming work started). The terminal statuses `done` and `cancelled` block bare `--status` updates; use `task reopen --reason` (the preferred verb-gated path) or `--force` (operator override, records audit row).

**Scope guard:** Refuses when the operator's resolved write scope disagrees with the task's stored scope. See [Cross-scope guard](#cross-scope-guard).

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--title <text>` | New title. | unchanged |
| `--body <text>` | New body. May be `@<file>`. | unchanged |
| `--status <status>` | New status: `todo`, `doing`, `blocked`, `done`, `cancelled`. Must be a legal transition from the current status per the matrix (see description). | unchanged |
| `--priority <n>` | New priority integer. | unchanged |
| `--next-action <text>` | Update the next concrete action. | unchanged |
| `--due <date>` | Update due date. | unchanged |
| `--plan <plan-id>` | Move task to a different plan (or `none` to detach). | unchanged |
| `--force` | Bypass the active-claim guard (see **Claim-atomic guard** below) AND the status-transition matrix. Required to move a `done` / `cancelled` task back to a non-terminal status without going through `task reopen`; the transition is recorded in `task_reopens` with `source='task-update-force'`. Prefer `task reopen <id>` for the documented recovery path. | off |
| `--reason <text>` | Operator-supplied rationale recorded on the `task_reopens` audit row when `--force` triggers a terminal → non-terminal transition. | empty |
| `--no-auto-promote` | Skip the [plan-status auto-promotion invariant](concepts.md#plan) (plan 304) for this operation. Escape hatch for scripted migrations that don't intend the plan-level transition. | off |
| `--editor` | Accepted as a no-op. The editor-driven path is `task edit`; this flag exists so scripts that pass `--editor=false` alongside other flags (e.g. copied from `task add` invocations) are not rejected with `UnknownFlag`. | off |
| `--slug <slug>` | Replace the task's slug. `tasks.slug` is globally unique. | none |

**Claim-atomic guard:** When `--status` is supplied and the task has an active work claim (`agent_work_claims.status='active'` and lease not yet expired), the status flip is refused with exit code `1` and a message identifying the active claim. This prevents an operator `task update --status done` from stranding a live agent lease. Pass `--force` to override and flip the status anyway — use this only when you know the agent is no longer working the task (e.g. it crashed without releasing the claim).

**Schema effects:** Updates `tasks(title, body, status, priority, next_action, due_at, plan_id, updated_at)`. When `--force` triggers a terminal → non-terminal transition, also inserts into `task_reopens(task_id, from_status, to_status, source='task-update-force', reason)`.

**Capture:** Appends `session_entries` row with `prefix='action'`.

**Exit codes:**
- `1` — task not found.
- `1` — invalid status value.
- `1` — terminal-status transition attempted without `--force`.
- `1` — task has an active work claim; re-run with `--force` to override.

---

### `planar task done <task-id>`

**Synopsis:**
```
planar task done <task-id> [--force] [--json]
```

**Description:** Mark a task as done. Legal from `doing` or `blocked` — not from `todo` (the matrix requires `todo → doing` first) or from a terminal status (`done`/`cancelled`). Equivalent to `task update --status done` but spelled explicitly for the common case.

**Scope guard:** NONE. `task done` declares `--scope` and never reads it; the flag is inert here and no comparison is made. See [Cross-scope guard](#cross-scope-guard).

**Claim-atomic guard:** Refuses if the task has an active work claim. The agent path (`planar-agent complete`) is the correct way to close out a claimed task. Pass `--force` to override and mark done anyway — use this only when the agent is known to be no longer active.

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--force` | Override the active-claim guard and mark the task done regardless of any live claim. | off |

**Schema effects:** Updates `tasks(status='done', updated_at)`.

**Capture:** Appends `session_entries` row with `prefix='action'`.

**Exit codes:**
- `1` — task not found.
- `1` — task is not in a legal pre-done status (e.g. `todo` or already `done`/`cancelled`).
- `1` — task has an active work claim; re-run with `--force` to override.

---

### `planar task reopen <task-id>`

**Synopsis:**
```
planar task reopen <task-id> [--status todo|doing|blocked] [--reason <text>] [--scope <slug>] [--force] [--json]
```

**Description:** Reopen a task currently in a terminal status (`done` or `cancelled`). The dedicated recovery path for wrongly-marked tasks — see [`import`](#planar-import-repo-root) for the producer of the most common false-done case. Refuses if the task is not in a terminal status; use `task update --status <s>` for ordinary transitions.

**Scope guard:** NONE. `task reopen` declares `--scope` and never reads it; the flag is inert here and no comparison is made. See [Cross-scope guard](#cross-scope-guard).

**Claim-atomic guard:** Refuses if the task has an active work claim. Pass `--force` to override.

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--status <status>` | Non-terminal status to move the task to: `todo`, `doing`, `blocked`. | `todo` |
| `--reason <text>` | Operator-supplied rationale recorded on the `task_reopens` audit row. | empty |
| `--scope <slug>` | Scope override for the cross-scope guard. | cwd-derived |
| `--force` | Override the active-claim guard. | off |

**Schema effects:**
- Updates `tasks(status, updated_at)`.
- Inserts into `task_reopens(task_id, from_status, to_status, source='task-reopen', reason)`.

**Capture:** Appends `session_entries` row with `prefix='action'`.

**Exit codes:**
- `1` — task not found.
- `1` — task is not currently in a terminal status.
- `1` — task has an active work claim; re-run with `--force` to override.

---

### `planar task block <task-id> --on <task-id>`

**Synopsis:**
```
planar task block <task-id> --on <task-id> [--force] [--json] [--reason <text>]
```

**Description:** Mark a task as blocked and record the blocking relationship in `entity_links`. Sets the blocked task's status to `blocked`.

**Scope guard:** NONE. `task block` declares `--scope` and never reads it; neither endpoint is compared. See [Cross-scope guard](#cross-scope-guard).

**Claim-atomic guard:** Refuses if the task being blocked has an active work claim. Pass `--force` to override.

**Options:**

| Flag | Description | Required |
|------|-------------|----------|
| `--on <task-id>` | The task that is blocking. | yes |
| `--force` | Override the active-claim guard. | no |
| `--reason <text>` | Optional reason for the block. | no |

**Schema effects:**
- Updates `tasks(status='blocked', updated_at)` for the blocked task.
- Inserts into `entity_links(from_kind='task', from_id=<task-id>, to_kind='task', to_id=<on-task-id>, relationship='depends-on')`.

**Capture:** Appends `session_entries` row with `prefix='action'`.

**Exit codes:**
- `1` — either task id not found.
- `1` — task has an active work claim; re-run with `--force` to override.

---

### `planar task link <task-id> <to-kind:to-id> --relationship <kind>`

**Synopsis:**
```
planar task link <task-id> <to-kind:to-id> --relationship <kind> [--json] [--scope <scope>]
```

**Description:** Create an `entity_links` row from a task to another entity.

**Arguments:** Same pattern as `plan link`. Valid `to-kind` values: `plan`, `plan_step`, `task`, `question`, `test_scenario`, `artifact`, `decision`, `session`.

**Options:**

| Flag | Description | Required |
|------|-------------|----------|
| `--relationship <kind>` | One of `derives-from`, `depends-on`, `addresses`, `verifies`, `cites`, `supersedes`. | yes |
| `--scope <scope>` | Declared and never read by the handler. | no |

**Schema effects:** Inserts into `entity_links(from_kind='task', from_id, to_kind, to_id, relationship)`.

**Capture:** Appends `session_entries` row with `prefix='action'`.

---

### `planar task facts stage <task-id>`

**Synopsis:**
```
planar task facts stage <task-id> [--json]
```

**Description:** Rebuild one task's routing facts (`routing_task_facts`) under **operator** provenance, then report the resulting packet.

`planar spec ingest --apply` is the only other writer of routing facts, and it rebuilds every task below an anchor plan. That left two gaps this verb closes (task 6048, decision 1102):

- a hand-filed task — one created with `planar task add` rather than materialized from a roadmap bullet — could never obtain routing facts at all, so `planar task packet` reported `missing_acceptance_fact` / `missing_next_action_fact` and the four `missing_*_spec` reasons permanently;
- editing a task body moves the acceptance section's digest, which strands the facts ingest staged from it. The task becomes `stale_fact` with no operator-reachable way to restage short of re-ingesting the whole anchor plan.

Facts are derived through the **same predicates** the plan-wide pass uses, so an operator-staged fact and an ingest-staged one agree on what they mean. They are stamped `operator-v1` rather than `spec-ingest-v1`, so provenance stays legible in the row and in `task packet --json`.

**Staleness is not traded away.** `packet` accepts both materializer versions, but the digest comparison is unchanged: an operator-staged fact goes stale the moment its source text changes, exactly as an ingest-staged one does. A fact that could never go stale would be a weaker contract, and is deliberately not what this verb produces.

**Citations are resolved, never invented.** A `cited_artifact_section` fact is staged only for an artifact the task *already* cites through an `entity_links` edge **and** references explicitly in its body as `artifact:<id>#Section`. A BARE locator of that form runs to end-of-line but stops at the first `,`, `)` or `]` — each of which ends a real writing pattern (`(see artifact:5#Overview)`, `see artifact:5#Overview, which ...`). To cite a heading that CONTAINS one of those characters, wrap the whole locator in brackets: `[artifact:<id>#File-level tree (operator-approved)]` suppresses the `,` and `)` stops and ends at the matching `]` (nesting counted). An unmatched `[` falls back to the bare scan (task 6760, decision 1124). An artifact cited without a resolvable locator is reported as a diagnostic naming the task, the artifact, the section asked for, and the sections that artifact actually has — and **nothing is staged** in that case.

**Scope:** strictly one task. The delete that precedes the rewrite is keyed on `task_id`, so staging one task never discards a sibling's ingest-materialized facts.

**Arguments:**

| Argument | Description |
|----------|-------------|
| `<task-id>` | Task whose facts are rebuilt (required). |

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--json` | Emit the resulting routing packet as JSON, identical in shape to `task packet --json`. | off |

**Output:** text mode prints the provenance stamp, whether the packet is now ready, and — when it is not — the readiness reasons that remain. Reporting the post-state rather than a bare success is deliberate: the operator's question is whether the task is dispatchable now, and the packet is the only authority on that.

**Exit codes:** `0` on success (including a successful stage that leaves the packet unready — that is a real answer). `1` when the task id does not exist, and `1` for an unresolvable citation, matching `spec ingest --apply` for the identical condition. `2` for a non-integer id.

**Schema effects:** Deletes and reinserts `routing_task_facts` rows for `<task-id>` only, inside one immediate transaction.

---

### `planar task touches add <task-id> <repo-slug> [--path <p>]`

**Synopsis:**
```
planar task touches add <task-id> <repo-slug> [--path <p>] [--scope <scope>]
```

**Description:** Record that a task touches the given repo (and, with `--path`, a specific file). The repo slug must be registered via `planar init` (present in `projects`).

- **Without `--path`** (repo-level): inserts an `entity_links(relationship='touches', from_kind='task', to_kind='repo')` row — the coarse signal used by `task list --touches`. If a repo-level touches link already exists, the command surfaces a clean user error from the UNIQUE constraint — re-adding is not silently no-op'd; remove it with `planar task touches remove` first.
- **With `--path <p>`** (path-level): writes a `task_touch_paths` row `(task_id, repo_id, path)` where `<p>` is a repo-relative file path, **and** writes the coarse repo-level edge (a path-touch implies the repo-touch; a pre-existing repo edge is tolerated in this mode rather than erroring). Path-level rows are idempotent against `unique(task_id, repo_id, path)` — re-declaring the same path is a no-op. Declare touches per file (repeat the verb), not as a list. These declarations are what `plan recommend-strategy` reads for the path-shaped rules 2/3/4; path detail refines the coarse repo signal (two tasks editing different files in the same repo stay parallel-eligible).

**Arguments:**

| Argument | Description |
|----------|-------------|
| `<task-id>` | Task to annotate (required). |
| `<repo-slug>` | Repo slug to link as touched (required). |

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--path <p>` | Repo-relative file path the task touches; writes a path-level `task_touch_paths` row (plus the coarse repo edge). | none (repo-level only) |
| `--scope <scope>` | Declared and never read by the handler. | none |

**Output (`--json`):**
```json
{"ok":true,"task_id":42,"repo_id":7,"repo_slug":"acme/protos","path":"src/foo.cpp"}
```
(`path` is `null` for a repo-level add.)

**Advisory — association-less repo.** If the named repo belongs to **no association**, a warning is printed to stderr (naming a same-basename alternative when one exists) and the write proceeds. This is not a scope check: link verbs are [deliberately unguarded](concepts.md#cross-scope-guard) because a touches edge to a repo in a *different* association is the legitimate polyrepo workflow. A repo in *no* association is different — nothing can reach it, so it is nearly always a stale duplicate slug chosen over the live one. Declarations on such a repo still compute eligibility correctly (rule 2 compares `(repo_id, path)` tuples and never reads the filesystem), but closure extraction reads `projects.root_path`, so seeds resolve against the wrong checkout and unreadable files are skipped silently.

**Schema effects:** Inserts into `entity_links(from_kind='task', from_id=<task-id>, to_kind='repo', to_id=<repo-id>, relationship='touches')`; with `--path`, also inserts `task_touch_paths(task_id=<task-id>, repo_id=<repo-id>, path=<p>)`.

**Capture:** Appends `session_entries` row with `prefix='action'`.

**Exit codes:**
- `1` — task not found.
- `1` — repo slug not found (not registered).
- `1` — repo-level link already exists (duplicate constraint; only without `--path`).

---

### `planar task touches infer <task-id>`

**Synopsis:**
```
planar task touches infer <task-id> [--repo <slug>] [--apply] [--wide] [--json]
```

**Description:** Propose path-level `task_touch_paths` rows by extracting path-shaped tokens from the task's own `title`, `body`, and `next_action`, then resolving them against a repo checkout. **Preview by default — without `--apply` nothing is written.**

<!-- surface-lint-ignore surface-path-missing: illustrative token-stripping example, not a real path -->
Tokens are recovered from the phrasings this codebase actually uses: prose punctuation and backticks are stripped, a trailing `:<line>` or `:<line>-<line>` citation is removed (`docs/cli-reference.md:339` → `docs/cli-reference.md`), and a sentence-ending period is dropped (`Rework src/alpha.cpp.` → `src/alpha.cpp`). URLs, absolute paths, and `../` traversal are rejected.

Each candidate is classified:

| Classification | Meaning | Written by `--apply` |
|----------------|---------|----------------------|
| `resolved` | Exact repo-relative file. | yes |
| `directory` | Token named a directory; expanded recursively to its files. | only with `--wide` |
| `basename` | Bare filename; **every** matching path in the tree. | only with `--wide` |
| `unresolved` | Path-shaped but no match — reported for review. | no |
| `too_broad` | Directory or basename expanding past 64 files. | no |

**Ambiguity always resolves wide when *proposing*** ([decision 906](concepts.md#declaring-what-a-task-touches)): a directory expands, a basename yields every match, and nothing unplaceable is invented. Inference cannot tell an over-declaration from an under-declaration; only the operator can, which is why the default is preview.

**But wide candidates are not *written* by default.** Measured over 46 open tasks in six real plans, applying rule 2 offline:

| Policy | Parallel-eligible |
|--------|-------------------|
| Nothing declared | 0 / 46 |
| `resolved` only (default) | **14 / 46** |
| `resolved` + `--wide` | 13 / 46 |

Wide expansion bought zero additional eligible tasks and cost one. The mechanism is rule 2's drop-both-on-tie: an *undeclared* task removes only itself from the eligible set, but an *over-declared* one removes its peers too. In one plan, four tasks each mentioned a skills source directory in prose; expanding it gave all four the same 35 paths, so they mutually overlapped and also dragged down the one task that had seven genuinely distinct real paths — 1 eligible became 0.

So over-declaration is only the safely-recoverable direction while the declaration stays narrow enough not to intersect everything. Pass `--wide` when you have judged a specific expansion to be right; the preview shows each candidate's expansion size for exactly that decision.

Repo selection: `--repo <slug>` names the checkout. Without it, the repo is derived from the current directory — the project whose `root_path` is the **longest** matching prefix, so a submodule checkout beats its superproject.

**Arguments:**

| Argument | Description |
|----------|-------------|
| `<task-id>` | Task to infer touches for (required). |

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--repo <slug>` | Repo checkout to resolve paths against. | derived from cwd |
| `--apply` | Write the writable candidates. Without it, nothing is written. | off (preview) |
| `--wide` | Also write `directory` and `basename` candidates. Measured to reduce eligibility — see above. | off |
| `--json` | Emit JSON instead of text. | off |

**Output (`--json`):**
```json
{"task_id":42,"repo_id":7,"repo_slug":"acme/protos","applied":true,"written":2,"review":1,
 "candidates":[{"token":"src/foo.cpp","evidence":"body","classification":"resolved","paths":["src/foo.cpp"]},
               {"token":"src/gone.cpp","evidence":"body","classification":"unresolved","paths":[]}]}
```

**Schema effects:** With `--apply`, inserts `task_touch_paths(task_id, repo_id, path)` for every path of every writable candidate, plus the coarse `entity_links(relationship='touches')` repo edge — a path-touch implies the repo-touch. Both are wrapped in a savepoint: a partial commit would leave the repo edge without its path rows, and `plan recommend-strategy` would then fall back to the whole-repo signal and serialize a task that should have been eligible. Writes are idempotent against `unique(task_id, repo_id, path)`, so re-running is a no-op.

**Exit codes:**
- `1` — task id not an integer, or task not found.
- `1` — `--repo` slug not found.
- `1` — no repo matches the current directory and `--repo` was not given.
- `1` — the resolved repo has no `root_path` recorded.

---

### `planar task touches list <task-id>`

**Synopsis:**
```
planar task touches list <task-id> [--json]
```

**Description:** List the touches declared on a task at both granularities: the repo slugs it touches (repo-level `entity_links` edges) and the file paths it touches (`task_touch_paths` rows). Read-only.

**Arguments:**

| Argument | Description |
|----------|-------------|
| `<task-id>` | Task to inspect (required). |

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--json` | Emit JSON instead of text. | off |

**Output (`--json`):**
```json
{"task_id":42,"repos":["acme/protos"],"paths":[{"repo":"acme/protos","path":"src/foo.cpp"}]}
```

**Exit codes:**
- `1` — task id not an integer.

---

### `planar task touches remove <task-id> <repo-slug> [--path <p>]`

**Synopsis:**
```
planar task touches remove <task-id> <repo-slug> [--path <p>] [--json] [--scope <scope>]
```

**Description:** Withdraw a touch declaration.

- **Without `--path`** (repo-level): deletes the `entity_links(relationship='touches')` row. Returns an error if no such link exists.
- **With `--path <p>`** (path-level): deletes one `task_touch_paths` row and **leaves the repo edge in place**.

The two granularities are independent, and removing the repo edge is **not** a way to withdraw path declarations: [rule 2](#planar-plan-recommend-strategy-plan-id) reads `task_touch_paths` directly, so orphaned path rows keep driving eligibility after their edge is gone.

This is deliberately not symmetric with `touches add --path`, where a path-touch implies the repo-touch. Withdrawing one file should not silently drop a repo claim that may still carry other paths, or an intentional whole-repo declaration.

**Withdrawing the last path does not return a task to "undeclared."** Because the repo edge survives, the task is left holding a *whole-repo* claim — and a whole-repo touch collides with any same-repo touch, coarse or path-level. The task therefore becomes **more** restrictive than an undeclared one, not less: it will drop its same-repo peers along with itself. To return a task to fully undeclared, remove the repo edge as well (`touches remove <task> <repo>` with no `--path`). Erring strict is intentional — see [Declaring what a task touches](concepts.md#declaring-what-a-task-touches) for why under-declaration is the dangerous direction.

**Arguments:**

| Argument | Description |
|----------|-------------|
| `<task-id>` | Task to update (required). |
| `<repo-slug>` | Repo slug to remove from touches (required). |

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--path <p>` | Withdraw this path-level declaration only; the repo edge is preserved. | none (repo-level removal) |
| `--scope <scope>` | Declared and never read by the handler. | none |

**Output (`--json`):**
```json
{"ok":true,"task_id":42,"repo_id":7,"repo_slug":"acme/protos"}
```
<!-- surface-lint-ignore surface-path-missing: illustrative JSON output example, not a real path -->
With `--path`, the withdrawn path is echoed as `"path":"src/foo.cpp"`.

**Schema effects:** Without `--path`, deletes from `entity_links(from_kind='task', from_id=<task-id>, to_kind='repo', to_id=<repo-id>, relationship='touches')`. With `--path`, deletes from `task_touch_paths(task_id, repo_id, path)` and leaves `entity_links` untouched.

**Capture:** Appends `session_entries` row with `prefix='action'`.

**Exit codes:**
- `1` — task not found.
- `1` — repo slug not found.
- `1` — link does not exist (repo-level).
- `1` — no such path declaration on that task and repo (with `--path`). A mistyped path fails loudly rather than reporting a silent no-op.

---

## Domain: `question`

Questions represent open uncertainties surfaced during work. They carry a lifecycle (`open` → `answered` / `wontfix`) and can be linked to tasks, plans, and artifacts.

---

### `planar question add <title>`

**Synopsis:**
```
planar question add <title> [--body <text>] [--scope <scope>] [--editor] [--plan <plan-id>]
```

**Description:** Record an open question.

**Arguments:**

| Argument | Description |
|----------|-------------|
| `<title>` | Question title (required). |

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--body <text>` | Expanded question body. May be `@<file>`. | none |
| `--scope <scope>` | Override scope. | cwd-derived write scope |
| `--editor` | Accepted, but not implemented: prints `warning: --editor not yet implemented; falling back to inline create` on stderr and the create proceeds. | off |
| `--plan <plan-id>` | Attach the question to this plan. | none |

**Output (human):**
```
question 3: "What is the Stripe API rate limit?"  [open]  (scope: association:3 [from cwd])
```

**Output (`--json`):**
```json
{"ok":true,"id":3,"title":"What is the Stripe API rate limit?","status":"open","scope_kind":"association","scope_id":3,"scope_source":"cwd"}
```

**Schema effects:** Inserts into `questions(scope_kind, scope_id, title, body, status='open')`.

**Capture:** Appends `session_entries` row with `prefix='question'`.

**Exit codes:**
- `1` — scope not resolvable.

---

### `planar question answer <question-id>`

**Synopsis:**
```
planar question answer <question-id> --answer <text> [--json]
```

**Description:** Provide an answer to an open question, transitioning its status to `answered`. The answer is passed as the `--answer` flag, not as a positional: `planar question answer 12 "text"` fails at parse time with exit 2. The flag takes literal text; there is no `@<file>` expansion (use `--answer "$(cat file)"`).

**Arguments:**

| Argument | Description |
|----------|-------------|
| `<question-id>` | Id of the question to answer. |

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--answer <text>` | The answer text. Required; must be non-empty. | none |
| `--json` | Emit the updated question as JSON. | off |

**Schema effects:** Updates `questions(status='answered', answer_body=<answer>, answered_at=now(), updated_at)`.

**Capture:** Appends `session_entries` row with `prefix='decision'` (answers are resolved decisions).

**Exit codes:**
- `2` — `--answer` omitted (`--answer is required`).
- `1` — `--answer` present but empty.
- `1` — question not found.
- `1` — question is already answered or wontfix.

---

### `planar question wontfix <question-id>`

**Synopsis:**
```
planar question wontfix <question-id> [--reason <text>]
```

**Options:**

| Flag | Description |
|------|-------------|
| `--reason <text>` | Optional reason for the transition. |

**Description:** Mark a question as not going to be answered (wontfix). Use for questions that are no longer relevant or explicitly deferred.

**Schema effects:** Updates `questions(status='wontfix', updated_at)`.

**Capture:** Appends `session_entries` row with `prefix='note'`.

**Exit codes:**
- `1` — question not found.

---

### `planar question list`

**Synopsis:**
```
planar question list [--scope <scope>] [--status <status>] [--touches <repo-slug>] [--plan <id>]
```

**Description:** List questions matching the given filters. Without `--scope`, uses the cwd-derived read set and refuses outside registered scope unless `--scope global` is explicit.

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--scope <scope>` | Filter by scope. | cwd-derived read set |
| `--status <status>` | Filter: `open`, `answered`, `wontfix`. Repeatable. | `open` |
| `--touches <repo-slug>` | Return questions scoped to this repo plus questions with `entity_links(relationship='touches', to_kind='repo')` for this repo. | off |

**Output (human):**
```
id   status    scope            title
3    open      assoc:billing    What is the Stripe API rate limit?
4    answered  repo:web-app     Should we cache webhook receipts?
```

The `scope` column shows where the question lives: `global`, `repo:<slug>`, or `assoc:<slug>`. Column width auto-sizes to the longest label in the result set.

**Output (`--json`):** One object per question.

**Schema effects:** Reads `questions`.

**Capture:** None.

---

### `planar question show <question-id>`

**Synopsis:**
```
planar question show <question-id> [--json]
```

**Description:** Show a question with its body and (if answered) the answer.

**Schema effects:** Reads `questions`.

**Capture:** None.

**Exit codes:**
- `1` — question not found.

---

### `planar question link <question-id> <to-kind:to-id> --relationship <kind>`

**Synopsis:**
```
planar question link <question-id> <to-kind:to-id> --relationship <kind> [--json] [--scope <scope>]
```

**Options:**

| Flag | Description |
|------|-------------|
| `--scope <scope>` | Declared and never read by the handler. |

**Description:** Create an `entity_links` row from a question to another entity.

**Schema effects:** Inserts into `entity_links(from_kind='question', from_id, to_kind, to_id, relationship)`.

**Capture:** Appends `session_entries` row with `prefix='action'`.

---

## Domain: `scenario`

Test scenarios are verification artifacts tied to specs, plans, or tasks. Planar records them and their outcomes; it does not execute them.

---

### `planar scenario add <title>`

**Synopsis:**
```
planar scenario add <title> [--body <text>] [--related <artifact-id>] [--scope <scope>] [--editor] [--plan <plan-id>]
```

**Description:** Create a new test scenario.

**Arguments:**

| Argument | Description |
|----------|-------------|
| `<title>` | Scenario title (required). |

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--body <text>` | Full scenario description. May be `@<file>`. | none |
| `--related <artifact-id>` | Associate with a specific artifact (spec or ADR). Sets `test_scenarios.related_artifact_id`. | none |
| `--scope <scope>` | Override scope. | cwd-derived write scope |
| `--editor` | Accepted, but not implemented: prints `warning: --editor not yet implemented; falling back to inline create` on stderr and the create proceeds. | off |
| `--plan <plan-id>` | Attach the scenario to this plan. | none |

**Output (human):**
```
scenario 9: "Stripe webhook idempotency"  [draft]  (scope: association:3 [from cwd])
```

**Output (`--json`):**
```json
{"ok":true,"id":9,"title":"Stripe webhook idempotency","status":"draft","scope_kind":"association","scope_id":3,"scope_source":"cwd"}
```

**Schema effects:** Inserts into `test_scenarios(scope_kind, scope_id, title, body, status='draft', related_artifact_id)`.

**Capture:** Appends `session_entries` row with `prefix='action'`.

**Exit codes:**
- `1` — artifact id not found.

---

### `planar scenario verify <scenario-id>`

**Synopsis:**
```
planar scenario verify <scenario-id> [--outcome <outcome>] [--summary <text>] [--json]
```

**Description:** Record the outcome of running a scenario. When `--outcome pass` (the default), transitions status to `verified`. Non-passing outcomes (`fail`, `error`, `skipped`) record `last_outcome` and `last_run_at` but leave the status unchanged. Does not execute the scenario — execution is the agent's job.

**Auto-transition from draft:** When the scenario is in `draft` status and `--outcome pass`, `verify` internally walks `draft → ready → verified` (two policy-checked hops) so the operator workflow `scenario add → scenario verify` works with no intermediate step required. There is no `scenario ready` CLI verb.

**Status matrix:** `ready → verified` (on pass). When source is `draft`, auto-walks `draft → ready → verified` first. Non-passing outcomes leave status unchanged regardless of source status.

**Arguments:**

| Argument | Description |
|----------|-------------|
| `<scenario-id>` | Scenario id. |

**Options:**

| Flag | Description | Required |
|------|-------------|----------|
| `--outcome <outcome>` | One of `pass`, `fail`, `error`, `skipped`. Defaults to `pass`. | no |
| `--summary <text>` | Optional summary of the run. Included in the audit row. | no |
| `--json` | Emit the updated scenario as JSON. | no |

**Schema effects:**
- Updates `test_scenarios(last_run_at=now(), last_outcome=<outcome>, updated_at)`.
- On `pass`: additionally sets `status='verified'`.

**Capture:** Appends `session_entries` row with `prefix='observation'`.

**Exit codes:**
- `1` — scenario not found.
- `1` — outcome not in allowed values.
- `1` — illegal transition (source status does not permit `→ verified`; e.g. calling `verify` from `retired`).

---

### `planar scenario list`

**Synopsis:**
```
planar scenario list [--scope <scope>] [--status <status>] [--related <artifact-id>] [--touches <repo-slug>]
```

**Description:** List test scenarios. Without `--scope`, uses the cwd-derived read set and refuses outside registered scope unless `--scope global` is explicit.

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--scope <scope>` | Filter by scope. | cwd-derived read set |
| `--status <status>` | Filter: `draft`, `ready`, `verified`, `failing`, `retired`. Repeatable. | all |
| `--related <artifact-id>` | Filter to scenarios related to a specific artifact. | all |
| `--touches <repo-slug>` | Return scenarios scoped to this repo plus scenarios with `entity_links(relationship='touches', to_kind='repo')` for this repo. | off |

**Output (human):**
```
id   status    scope            title
9    verified  assoc:billing    Stripe webhook idempotency
10   failing   repo:web-app     Payment retry logic
```

The `scope` column shows where the scenario lives: `global`, `repo:<slug>`, or `assoc:<slug>`. Column width auto-sizes to the longest label in the result set.

**Output (`--json`):** One object per scenario.

**Schema effects:** Reads `test_scenarios`.

**Capture:** None.

---

### `planar scenario show <scenario-id>`

**Synopsis:**
```
planar scenario show <scenario-id> [--json]
```

**Description:** Show a scenario's full details including last run outcome.

**Schema effects:** Reads `test_scenarios`, `artifacts` (for related artifact title).

**Capture:** None.

**Exit codes:**
- `1` — scenario not found.

---

### `planar scenario retire <scenario-id>`

**Synopsis:**
```
planar scenario retire <scenario-id> [--reason <text>] [--json]
```

**Description:** Mark a scenario as retired (no longer relevant). Legal from any non-terminal status (`draft`, `ready`, `verified`, `failing`). `retired` is terminal — there is no reopen verb.

**Status matrix:** any `{draft, ready, verified, failing} → retired`.

**Schema effects:** Updates `test_scenarios(status='retired', updated_at)`.

**Capture:** Appends `session_entries` row with `prefix='action'`.

**Exit codes:**
- `1` — scenario not found.

---

## Domain: `decision`

Decisions record rationale for choices made during work. They are scope-aware and linked to the session that produced them.

---

### `planar decision add <title>`

**Synopsis:**
```
planar decision add <title> --body <text> [--rationale <text>] [--scope <scope>] [--editor] [--plan <plan-id>]
```

**Description:** Record a decision with its body and optional rationale.

**Arguments:**

| Argument | Description |
|----------|-------------|
| `<title>` | Decision title (required). |

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--body <text>` | Decision statement. May be `@<file>`. Required. | — |
| `--rationale <text>` | Rationale text. May be `@<file>`. | none |
| `--scope <scope>` | Override scope. | cwd-derived write scope |
| `--editor` | Accepted, but the interactive editor flow is not implemented: without `--body` it prints a warning on stderr and the verb still refuses with `--body is required` (exit 2). | off |
| `--plan <plan-id>` | Attach the decision to this plan. | none |

**Output (human):**
```
decision 5: "Use Stripe as payment processor"  [proposed]  (scope: association:3 [from cwd])
```

**Output (`--json`):**
```json
{"ok":true,"id":5,"title":"Use Stripe as payment processor","status":"proposed","scope_kind":"association","scope_id":3,"scope_source":"cwd"}
```

**Schema effects:** Inserts into `decisions(scope_kind, scope_id, title, body, rationale, status='proposed', session_id=<current session id>)`. If no active session exists when this command runs, one is auto-created per the [Capture Behavior](#capture-behavior) rule before insertion, so `decisions.session_id` is always non-null on entries created via the CLI.

**Capture:** Appends `session_entries` row with `prefix='decision'`.

---

### `planar decision accept <decision-id>`

**Synopsis:**
```
planar decision accept <decision-id> [--scope <scope>] [--json]
```

**Description:** Mark a decision as accepted.

**Scope guard:** Refuses before changing the decision when the operator's
resolved write scope disagrees with the decision's stored scope. Pass the
decision's scope explicitly with `--scope` when invoking from elsewhere. See
[Cross-scope guard](#cross-scope-guard).

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--scope <scope>` | Override scope for the cross-scope guard. | cwd-derived write scope |

**Schema effects:** Updates `decisions(status='accepted', decided_at=now(), updated_at)`.

**Capture:** Appends `session_entries` row with `prefix='decision'`.

**Exit codes:**
- `1` — decision not found.
- `5` — the resolved write scope does not admit the decision's scope.

---

### `planar decision supersede <decision-id> --by <decision-id>`

**Synopsis:**
```
planar decision supersede <decision-id> --by <decision-id> [--json] [--scope <scope>]
```

**Description:** Mark a decision as superseded by a newer decision.

**Scope guard:** NONE. Neither the old decision nor the `--by` decision is compared against the operator's scope. See [Cross-scope guard](#cross-scope-guard).

**Options:**

| Flag | Description | Required |
|------|-------------|----------|
| `--by <decision-id>` | The newer decision that supersedes this one. | yes |
| `--scope <scope>` | Declared and never read by the handler. | no |

**Schema effects:**
- Updates `decisions(status='superseded', updated_at)` on the old decision.
- Inserts into `entity_links(from_kind='decision', from_id=<new>, to_kind='decision', to_id=<old>, relationship='supersedes')`.

**Capture:** Appends `session_entries` row with `prefix='decision'`.

**Exit codes:**
- `1` — either decision id not found.

---

### `planar decision withdraw <decision-id>`

**Synopsis:**
```
planar decision withdraw <decision-id> [--scope <scope>] [--json]
```

**Description:** Mark a decision as withdrawn.

**Scope guard:** Refuses before changing the decision when the operator's
resolved write scope disagrees with the decision's stored scope. Pass the
decision's scope explicitly with `--scope` when invoking from elsewhere. See
[Cross-scope guard](#cross-scope-guard).

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--scope <scope>` | Override scope for the cross-scope guard. | cwd-derived write scope |

**Schema effects:** Updates `decisions(status='withdrawn', updated_at)`.

**Capture:** Appends `session_entries` row with `prefix='decision'`.

**Exit codes:**
- `1` — decision not found.
- `5` — the resolved write scope does not admit the decision's scope.

---

### `planar decision list`

**Synopsis:**
```
planar decision list [--scope <scope>] [--status <status>] [--plan <plan-id>]
```

**Description:** List decisions. Without `--scope`, uses the cwd-derived read set and refuses outside registered scope unless `--scope global` is explicit.

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--scope <scope>` | Filter by scope. | cwd-derived read set |
| `--status <status>` | Filter: `proposed`, `accepted`, `superseded`, `withdrawn`. Repeatable. | `proposed,accepted` |
| `--plan <plan-id>` | Filter to decisions attached to this plan. | none |

**Output (human):**
```
id   status     scope            title
5    proposed   assoc:billing    Use Stripe Checkout instead of Elements
6    accepted   repo:web-app     Adopt cookie-based session auth
```

The `scope` column shows where the decision lives: `global`, `repo:<slug>`, or `assoc:<slug>`. Column width auto-sizes to the longest label in the result set.

**Output (`--json`):** One object per decision.

**Schema effects:** Reads `decisions`.

**Capture:** None.

---

### `planar decision show <decision-id>`

**Synopsis:**
```
planar decision show <decision-id> [--json]
```

**Description:** Show a decision with body, rationale, status, and linked session.

**Schema effects:** Reads `decisions`, `sessions` (for session vendor/timestamp).

**Capture:** None.

**Exit codes:**
- `1` — decision not found.

---

## Domain: `artifact`

Artifacts are durable documents: tech specs, ADRs, design notes, summaries, and READMEs. They are scope-aware and can be imported from an existing file path or authored in-place.

---

### `planar artifact add <title>`

**Synopsis:**
```
planar artifact add <title> --kind <kind> [--body <text>] [--from-file <path>] [--source-path <path>] [--scope <scope>] [--plan <plan-id>] [--editor]
```

**Description:** Register a new artifact. The body may be specified inline via `--body`, read from a file via `--from-file`, or left empty. `--from-file` and `--body` are mutually exclusive. `--plan` attaches the artifact to the given plan via a `derives-from` entity link in the same transaction, for use by the planner agent when registering spec files against their anchor plan.

**Arguments:**

| Argument | Description |
|----------|-------------|
| `<title>` | Artifact title (required). |

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--kind <kind>` | One of `tech_spec`, `adr`, `design_note`, `summary`, `readme`, `generated`, `other`, `product_spec`, `roadmap`, `test_spec`, `research`, `getting_started`, `changelog_entry`, `glossary_term`. | Required. |
| `--body <text>` | Body text. May be `@<file>`. Mutually exclusive with `--from-file`. | none |
| `--from-file <path>` | Read artifact body from this file; also sets `--source-path` to the same path unless `--source-path` is specified explicitly. Mutually exclusive with `--body`. | none |
| `--source-path <path>` | Path to the source file (e.g. `docs/architecture.md`). | none |
| `--scope <scope>` | Override scope. | cwd-derived write scope |
| `--status <status>` | Initial status: `draft`, `active`. | `draft` |
| `--plan <plan-id>` | Attach the artifact to this plan via a `derives-from` entity link. Applied atomically in the same transaction as the artifact insert. | none |
| `--editor` | Declared and never read by the handler. | off |

**Output (human):**
```
artifact 3: "Billing Tech Spec"  [tech_spec, draft]  (scope: association:3 [from cwd])
```

**Output (`--json`):**
```json
{"ok":true,"id":3,"title":"Billing Tech Spec","kind":"tech_spec","status":"draft","scope_kind":"association","scope_id":3,"scope_source":"cwd"}
```

**Schema effects:** Inserts into `artifacts(scope_kind, scope_id, kind, title, body, source_path, status)`. When `--plan` is set, also inserts into `entity_links(from_kind='artifact', from_id=<new>, to_kind='plan', to_id=<plan-id>, relationship='derives-from')`.

**Capture:** Appends `session_entries` row with `prefix='action'`.

**Exit codes:**
- `1` — invalid kind value, `--from-file` and `--body` both specified, or plan not found.

---

### `planar artifact show <artifact-id>`

**Synopsis:**
```
planar artifact show <artifact-id> [--json]
```

**Description:** Show an artifact's metadata and (if present) body.

**Schema effects:** Reads `artifacts`.

**Capture:** None.

**Exit codes:**
- `1` — artifact not found.

---

### `planar artifact list`

**Synopsis:**
```
planar artifact list [--scope <scope>] [--kind <kind>] [--status <status>] [--plan <plan-id>]
```

**Description:** List artifacts. Without `--scope`, uses the cwd-derived read set and refuses outside registered scope unless `--scope global` is explicit.

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--scope <scope>` | Filter by scope. | cwd-derived read set |
| `--kind <kind>` | Filter by kind. Repeatable. | all |
| `--status <status>` | Filter: `draft`, `active`, `superseded`, `retired`. Repeatable. | `draft,active` |
| `--plan <plan-id>` | Filter to artifacts attached to this plan. | none |

**Output (human):** One sentence per artifact, with kind, status, and scope:
```
artifact 1: "Founding tech spec"  [tech_spec, active]  scope:assoc:project:planar
artifact 5: "Billing module roadmap"  [roadmap, draft]  scope:assoc:billing
```

The trailing `scope:<label>` is `global`, `repo:<slug>`, or `assoc:<slug>`.

**Output (`--json`):** One object per artifact (same fields as `artifact show`).

**Schema effects:** Reads `artifacts`.

**Capture:** None.

---

### `planar artifact update <artifact-id>`

**Synopsis:**
```
planar artifact update <artifact-id> [--title <text>] [--body <text>] [--status <status>] [--source-path <path>] [--json]
```

**Description:** Update mutable fields on an artifact.

**Scope guard:** NONE. `artifact update` performs no scope comparison. Note also that `--scope` on this verb is a **patch field** that reassigns the artifact's scope, not a write-scope override. See [Cross-scope guard](#cross-scope-guard).

**Schema effects:** Updates `artifacts(title, body, status, source_path, updated_at)`.

**Capture:** Appends `session_entries` row with `prefix='action'`.

**Exit codes:**
- `1` — artifact not found.
- `1` — invalid status value.

---

### `planar artifact link <artifact-id> <to-kind:to-id> --relationship <kind>`

**Synopsis:**
```
planar artifact link <artifact-id> <to-kind:to-id> --relationship <kind> [--json] [--scope <scope>]
```

**Options:**

| Flag | Description |
|------|-------------|
| `--scope <scope>` | Declared and never read by the handler. |

**Description:** Create an `entity_links` row from an artifact to another entity.

**Schema effects:** Inserts into `entity_links(from_kind='artifact', from_id, to_kind, to_id, relationship)`.

**Capture:** Appends `session_entries` row with `prefix='action'`.

---

## Domain: `document`

Read-only, revision-bound projections of plan and artifact content. Both
leaves open the database read-only and verify the schema version before
reading; they never create, migrate, or write the database, and write no
session entry or workbench file. The one row that can appear is the
binary-wide opt-in `cli_invocations` log: when `[introspection].cli_log` is
on, every `planar` invocation, these included, appends one row to an
*existing* database (it never creates or migrates one). Every flag takes literal
text — none accepts the `@<file>` grammar described under
[Conventions](#conventions).

**Output is always JSON.** `--json` is accepted on both leaves for symmetry
with the rest of the CLI and changes nothing; there is no human-text
rendering.

### `planar document project`

```bash
planar document project --kind plan|artifact --id <id> --json
```

Emits Planar's authoritative `block-document-v1` projection. Each passage has
a stable key, authoritative text, and source mapping. `content_revision` binds
the complete ordered projection to the source state.

**Output:**
```json
{"contract_version":"block-document-v1","source_uuid":"<db-uuid>","document_id":"<db-uuid>:artifact:1","content_revision":"<sha256>","passages":[{"key":"<db-uuid>:...","kind":"heading","text":"...","source":{"kind":"artifact","id":"1","path":"body","start_line":1,"end_line":1}}]}
```

### `planar document validate-range`

```bash
planar document validate-range --kind plan|artifact --id <id> \
  --content-revision <revision> --start-key <key> --start-offset <bytes> \
  --end-key <key> --end-offset <bytes> \
  --covered-key <key>... --segment-quote <text>... --json
```

Validates a complete adjacent range at one content revision. Offsets are UTF-8
byte offsets and must lie on code-point boundaries. Covered keys and segment
quotes are evidence, not authority: Planar recomputes and compares them before
returning the canonical keys, segment quotes, and normalized full quote.

**Output (`block-range-validation-v1`):**
```json
{"contract_version":"block-range-validation-v1","source_uuid":"<db-uuid>","document_id":"<db-uuid>:artifact:1","content_revision":"<sha256>","covered_keys":["<key>","..."],"segment_quotes":["<text>","..."],"normalized_quote":"<segments joined by \\n>"}
```

**Rejection reasons.** A rejected range exits `2` with
`error: document range rejected: <reason>` on stderr. Checks run in this order
and the first failure is reported:

| Reason | Meaning |
|--------|---------|
| `stale_revision` | `--content-revision` is not the revision just projected: the source changed, or the evidence came from another database. |
| `missing_boundary` | A boundary key or offset is absent. All four are required flags, so the parser normally refuses first (exit `2`). |
| `foreign_key` | `--start-key` or `--end-key` is not a passage of this projection. |
| `reversed_range` | The end passage precedes the start passage. |
| `invalid_utf8_boundary` | An offset is negative, past the passage end, before the start offset in a single-passage range, or splits a UTF-8 code point. |
| `noncontiguous_covered_keys` | The `--covered-key` list is not exactly the ordered passages from start to end. |
| `forged_quote` | The `--segment-quote` list is not exactly the projected text of each covered segment. |

The recovery for every reason is the same: re-run `document project`, rebuild
the evidence from that output, and resend. Retrying unchanged evidence fails
the same way.

**Exit codes (both leaves):**
- `0` — success.
- `1` — the source row is absent; the database file cannot be opened read-only (including when it does not exist); a projection query failed; the schema is **behind** this binary or has a gap.
- `2` — an unsupported `--kind` (anything but `plan` / `artifact`), a parse failure, or (on `validate-range`) a rejected range.
- `7` — the schema is **newer** than this binary supports.

---

## Domain: `annotate`

Annotations are either file-anchored review notes or entity-anchored notes on a plan or task. File annotations can carry a `commit_sha` and `text_hash` for later verification; entity annotations have no invented file path. Annotations have a lifecycle (`active` → `resolved` / `dismissed` → `archived`; or `active` → `archived` directly), free-form tags, and bulk operations over a filter. They are scope-aware like every other planning entity. `archived` is the single final retention state (plan 692): `resolved` and `dismissed` are outcome states that may still progress to `archived` via `annotate sweep` or `annotate archive`.

---

### `planar annotate add`

**Synopsis:**
```
planar annotate add --text <note> [--anchor-path <path>] [--line-start <n>] [--line-end <n>] [--commit-sha <sha>] [--text-hash <hash>] [--title <t>] [--slug <s>] [--body <text>] [--vendor <v>] [--plan <id>] [--task <id>] [--tags <csv>] [--scope <scope>] [--json]
```

**Description:** Create a new annotation. `--anchor-path` (+ optional `--line-start`/`--line-end`) locates the note in the tree; `--commit-sha` and `--text-hash` record what the anchored content looked like at capture time so `annotate verify` can detect drift. `--plan` / `--task` associate the note with a planning entity; `--tags` is a comma-separated list.

**Options:** `--text` is the note body (the load-bearing field). `--anchor-path`, `--line-start`, `--line-end`, `--commit-sha`, `--text-hash` form the anchor. `--title`, `--slug`, `--body`, `--vendor`, `--plan`, `--task`, `--tags`, `--scope` are optional metadata.

**Schema effects:** Inserts into `annotations(...)` (and `annotation_tags` for each tag).

---

### `planar annotate show <annotation-id>` / `planar annotate remove <annotation-id>`

`show` prints one annotation (add `--json`). `remove` hard-deletes it (and its `annotation_tags`).

`planar annotate remove <annotation-id> --expected-revision <n> [--json]` requires `--expected-revision`: omitting it refuses with `error: --expected-revision is required` (exit 2), and a value that differs from the annotation's stored `revision` refuses as a revision conflict without deleting.

`show --json` and `list --json` emit the stable consumer representation described in [Annotation consumer contract](features/annotation-consumer-contract.md). In particular, `anchor.kind` distinguishes `file` and `entity`, `target` is either `null` or an object with a `kind` and numeric `id`, and `revision` is the optimistic-concurrency value. Reads do not resolve, process, or otherwise mutate an annotation.

---

### `planar annotate list`

**Synopsis:**
```
planar annotate list [--anchor-path <path>] [--anchor-kind file|entity] [--target-kind plan|task] [--target-id <id>] [--status <status>] [--plan <id>] [--task <id>] [--vendor <v>] [--tag <tag>] [--scope <scope>] [--json]
```

**Description:** List annotations in scope, filtered by any combination of anchor path or kind, exact entity target kind/id, `--status` (`active`/`resolved`/`dismissed`/`archived`), associated `--plan`/`--task`, `--vendor`, or a single `--tag`. `--target-kind` and `--target-id` apply to entity anchors, while `--plan` / `--task` retain their legacy association meaning.

---

### `planar annotate capabilities`

**Synopsis:**
```
planar annotate capabilities [--json]
```

**Description:** Return a source-bound capability handshake for a local annotation consumer. JSON includes the immutable `source_uuid`, supported read/filter fields, entity-anchor/revision support, the allowed structured command operations, and receipt lookup support. It is an observation command; it does not process annotations or enable writing.

---

### `planar annotate update <annotation-id>`

**Synopsis:**
```
planar annotate update <annotation-id> [--title <t>] [--slug <s>] [--body <text>] [--status <status>] [--plan <id>] [--task <id>] [--scope <scope>] [--json]
```

**Description:** Rewrite an annotation's fields. Only the flags you pass are changed.

---

### `planar annotate tag <annotation-id> <tag> [--remove]`

Add a tag to an annotation, or remove it with `--remove`. Writes/deletes an `annotation_tags` row. Accepts `--json`.

---

### `planar annotate resolve|dismiss|archive <annotation-id>`

Lifecycle transitions on a single annotation: `resolve` marks it handled, `dismiss` marks it won't-fix, `archive` retires it. Each takes the annotation id and an optional `--json`.

---

### `planar annotate bulk-resolve|bulk-dismiss|bulk-archive`

**Synopsis:**
```
planar annotate bulk-resolve [--anchor-path <path>] [--plan <id>] [--task <id>] [--vendor <v>] [--tag <tag>] [--scope <scope>] [--json] [--operation-id <uuid>]
```

**Options:**

| Flag | Description |
|------|-------------|
| `--operation-id <uuid>` | Operation UUID. When supplied, the bulk transition runs as a receipt-backed command and records a durable aggregate receipt (see `annotate receipt`); omitted, the one-shot behaviour is unchanged. |

**Description:** Apply the lifecycle transition to **every** annotation matching the filter. `bulk-resolve` and `bulk-dismiss` act on `active` annotations only. `bulk-archive` includes `active`, `resolved`, and `dismissed` annotations (under the retention-tier model, resolved→archived and dismissed→archived are legal; already-`archived` rows are skipped as idempotent). The filter flags mirror `annotate list`. Use these to clear a whole review pass at once.

---

### `planar annotate verify [--anchor-path <path>] [--scope <scope>] [--json]`

**Description:** Verify annotation anchors against current workspace state — re-reads each anchored path/line range and compares against the stored `text_hash` / `commit_sha`, reporting which annotations are still anchored cleanly and which have drifted (the underlying content moved or changed). Restrict to one path with `--anchor-path`.

---

### `planar annotate sweep [--since-days <n>] [--scope <scope>] [--json]`

**Description:** Sweep stale annotations — archive `resolved` / `dismissed` annotations older than `--since-days`. Under the retention-tier model (plan 692), resolved→archived and dismissed→archived are legal, so sweep successfully archives all qualifying rows. Housekeeping for a scope whose review notes have accumulated over time.

---

## Domain: `promote`

Promotion moves an entity from one scope to another — typically from `global` (personal) to an association, or from one association to another. `demote` is the reverse.

---

### `planar promote <kind:id> --to <association-slug>`

**Synopsis:**
```
planar promote <kind:id> --to <association-slug>
```

**Description:** Change an entity's scope from its current scope to the target association. If the target association is workbench-enabled, the entity will be included in the next `workbench export`.

Promotion targets are associations only; promotion to `repo` scope is not supported. The valid transitions are `global → assoc:<slug>` and `assoc:<other> → assoc:<slug>`.

Valid entity kinds: `plan`, `task`, `question`, `test_scenario`, `artifact`, `decision`.

**Arguments:**

| Argument | Description |
|----------|-------------|
| `<kind:id>` | Typed entity reference, e.g. `task:42`, `plan:7`. |

**Options:**

| Flag | Description | Required |
|------|-------------|----------|
| `--to <association-slug>` | Target association slug. | yes |

**Output (human):**
```
task:42 promoted to association org:acme  (was: global)
```

**Output (`--json`):**
```json
{"ok":true,"kind":"task","id":42,"scope_kind":"association","scope_id":3,"previous_scope_kind":"global","previous_scope_id":null}
```

**Schema effects:** Updates `<entity_table>(scope_kind='association', scope_id=<association_id>, updated_at)`.

**Capture:** Appends `session_entries` row with `prefix='action'`.

**Exit codes:**
- `1` — entity not found.
- `1` — target association not found.
- `1` — entity kind not promotable.

---

### `planar demote <kind:id>`

**Synopsis:**
```
planar demote <kind:id> [--from <association-slug>] [--json]
```

**Description:** Reverse a promotion — move an entity back to global personal scope. Allowed before workbench export; after export, the exported file is left for the user to remove manually.

Demotion always targets `global`. Association-to-association transitions go through `promote`.

**Options:**

| Flag | Description | Required |
|------|-------------|----------|
| `--from <slug>` | Optional source-association slug recorded on the audit row. The engine demotes unconditionally regardless of this flag. | no |

**Schema effects:** Updates `<entity_table>(scope_kind='global', scope_id=NULL, updated_at)`.

**Capture:** Appends `session_entries` row with `prefix='action'`.

**Exit codes:**
- `1` — entity not found.
- `1` — entity already at global scope.

---

## Domain: `workbench`

The workbench is a bidirectionally synced drafting filesystem under `$PLANAR_WORKBENCH_ROOT`
(default `~/.planar/workbench/`). Each active feature occupies a directory named
`<association-slug>/<plan-key>-<plan-slug>/` and contains deterministic Markdown files for
every entity linked to the anchor plan. Sync is explicit — no daemon, no watcher.

Files are written atomically via temp-file + `os.Rename`. The `.sync` manifest records what
was last synced; its successful write commits a push. YAML front matter in every file
carries `entity_kind`, `entity_id`, `anchor_plan_id`, `title`, and `status` as the
bidirectional contract between filesystem and database.

**Exit codes (all workbench verbs):**
- `0` — success.
- `1` — operation failure (plan not found, malformed workbench file, lint issue, workbench root not writable).
- `2` — system error (DB, I/O).
- `3` — conflict(s) detected (status / sync / pull only; resolve to continue).

When one operation finds both malformed files and conflicts, exit code `1` takes precedence;
the summary still reports both counts so resolving the malformation does not hide the conflict.

---

### `planar workbench lint [<plan> | --all | --path <file-or-directory>]`

**Synopsis:**
```
planar workbench lint <plan> [--json]
planar workbench lint --all [--json]
planar workbench lint --path <file-or-directory> [--json]
```

**Description:** Validate Markdown frontmatter without changing the filesystem or database.
The linter calls the same `FrontMatter` parser used by pull, push, status, and sync. The
shared parser requires every rendered entity kind to carry non-empty `title` and `status`,
validates status against that kind's database enum, and requires artifacts to carry a valid
`artifact_kind`. Lint then checks that `anchor_plan_id` identifies an existing plan. Exactly
one target is required: one plan tree, every tree under the workbench root, or one explicit
file/directory.

Text output lists each issue as `path:line`, followed by a stable severity/code, message,
and repair hint, then prints the files/errors/warnings totals. `--json` emits one NDJSON
object per issue with `path`, `line`, `severity`, `code`, `message`, and `hint`. A clean JSON
run emits no issue rows. Errors and warnings both produce exit code 1, making the command
suitable for pre-commit hooks and CI.

**Exit codes:**
- `0` — every scanned file is valid.
- `1` — one or more errors or warnings, or an unreadable workbench tree.
- `2` — invalid or missing target selection, or a non-Markdown `--path` file.

---

### `planar workbench push <plan> [--filter-mode {failures,all}] [--apply-cleanup]`

**Synopsis:**
```
planar workbench push <plan> [--filter-mode failures|all] [--apply-cleanup] [--verbose]
```

**Options:**

| Flag | Description |
|------|-------------|
| `--verbose` | Verbose text rendering of the sync result. |

**Description:** Apply DB→FS changes for the named anchor plan. Each entity linked to the
plan is rendered as a Markdown file with YAML front matter. Files that match the last-synced
hash are skipped (no-op). Files are written atomically. Malformed files are counted
separately from pending drift and are never overwritten; a nonzero count appears in the
default summary and exits 1. JSON always includes `malformed` and `malformed_files` details.

**Terminal-status filter (plan 439).** Entities whose status falls in the active filter set
are excluded from the FS write set. The default mode is `failures` — cancelled tasks,
abandoned plans, superseded/withdrawn decisions, wontfix questions, retired test_scenarios,
and superseded/retired artifacts are filtered. Success terminals (`done` tasks, `answered`
questions, `verified` test_scenarios) stay visible. Pass `--filter-mode all` to extend the
filter to success terminals.

**Surprise-free upgrade path.** If files for entities the filter would have dropped already
exist on disk (typically because they were written before this feature shipped), push reports
them as `pre-existing terminal file(s)` in its summary but does NOT remove them by default.
Pass `--apply-cleanup` to remove them in the same pass — narrow-scope: only files this
specific push enumerated, not every terminal-backed file in the workbench tree (use
`planar workbench gc` for plan-wide cleanup). `--apply-cleanup` and `--filter-mode all` are
mutually exclusive (the modes express opposite intents).

**Arguments:**

| Argument | Description |
|----------|-------------|
| `<plan>` | Anchor plan ID (numeric) or slug. |
| `--filter-mode failures` | (default) Filter only failure-terminal statuses. |
| `--filter-mode all` | Extend the filter to success-terminal statuses too. |
| `--apply-cleanup` | Remove pre-existing FS files for entities this push would have filtered. |

**Output (human):**
```
workbench push: project_checkout-app/p1-checkout-revamp
  DB→FS  README.md
  DB→FS  tasks/1-update-checkout-ui.md
  DB→FS  tasks/2-refactor-payment-form.md
  no-op  plans/frontend-changes.md
```

**Schema effects (reads):** `plans`, `tasks`, `artifacts`, `entity_links`, `associations`, `workbench_sync_state`.

**Schema effects (writes):** Upserts `workbench_sync_state` rows; rewrites `.sync` manifest file.

**Capture:** Appends `session_entries` row with `prefix='action'`.

**Exit codes:**
- `1` — plan not found, not a top-level plan, or one or more malformed files detected.
- `2` — workbench root not writable or I/O error.

---

### `planar workbench pull <plan>`

**Synopsis:**
```
planar workbench pull <plan> [--verbose]
```

**Options:**

| Flag | Description |
|------|-------------|
| `--verbose` | Verbose text rendering of the sync result. |

**Description:** Apply FS→DB changes for the named anchor plan. Each Markdown file in the
feature directory is parsed; if its content hash differs from the manifest the entity is
updated in the DB. New files with valid front matter are inserted as tasks and linked to the
anchor plan via `derives-from`. Deleted files (present in manifest, absent on disk) mark the
entity soft-deleted. Malformed files are reported but not auto-inserted or auto-deleted.
They are counted separately from pending changes, included in the default summary only
when nonzero, and make the command exit 1. JSON always includes `malformed` and
`malformed_files`; each detail has `path` and `parse_error`.

Conflicts (both FS and DB changed since last sync) are surfaced; they are not resolved
automatically. Use `workbench resolve` to settle them.

**Non-body field edits are refused, not silently dropped (task 6910).** `pull` writes only
`body` (plus `status` for `task`/`plan`) — a decision's `## Rationale` section and a
question's `**Answer:**` line are rendered TO disk on `push` but never read back FROM it on
`pull`. Before task 6910 an operator edit there was silently discarded on every pull. Now,
before applying a file whose entity kind is `decision` or `question`, the extracted
`## Rationale` / `**Answer:**` text on disk is compared against what the database currently
holds; a difference refuses that ONE entity's pull entirely (not just the field — the whole
row, including `body`, is left untouched) and is reported as a named `field_edit_refusal`
(`path`, `entity_kind`, `entity_id`, `field` — `"rationale"` or `"answer"`) rather than an
undifferentiated `pending` count. This is a PER-ENTITY refusal, like `conflict` — it does not
abort the rest of the run. The fix is to make the edit through the CLI instead of the file:
`planar decision edit <id>` (the editor-first flow — the rationale is part of what it opens)
or `planar question answer <id> ...`, then pull
again. `--json` always includes `field_edit_refused` (a count) and `field_edit_refusals` (the
list); the default text summary prints nothing extra when the count is zero.

**Arguments:**

| Argument | Description |
|----------|-------------|
| `<plan>` | Anchor plan ID (numeric) or slug. |

**Output (human):**
```
workbench pull: project_checkout-app/p1-checkout-revamp
  FS→DB  tasks/1-update-checkout-ui.md
  no-op  README.md
  conflict  tasks/2-refactor-payment-form.md  CONFLICT [7]
```

**Schema effects (reads):** `workbench_sync_state`, filesystem.

**Schema effects (writes):** `tasks`, `plans`, `artifacts`, `decisions`, `questions`, `test_scenarios`,
`entity_links` (new derives-from links for discovered tasks), `workbench_sync_state`.

**Capture:** Appends `session_entries` row with `prefix='action'`.

**Exit codes:**
- `1` — plan not found or one or more malformed files detected.
- `2` — I/O error.
- `3` — one or more conflicts detected.

---

### `planar workbench status [<plan>]`

**Synopsis:**
```
planar workbench status [<plan>] [--json] [--verbose]
```

**Options:**

| Flag | Description |
|------|-------------|
| `--verbose` | Verbose text rendering of the status report. |

**Description:** Classify all (file, entity) pairs for one plan (or all plans with FS trees
if no plan argument is given) without applying any changes. Reports each file as one of:
`no-op`, `FS→DB`, `DB→FS`, `conflict`, `new-on-FS`, `deleted-on-FS`, `malformed`.
Malformed files are distinct from pending drift. A nonzero count appears as
`, N MALFORMED` in the text summary; JSON always includes `malformed` and
`malformed_files` details. Any malformed file makes status exit 1.

**Arguments:**

| Argument | Description |
|----------|-------------|
| `<plan>` | Optional. Anchor plan ID or slug. Omit to show all active features. |

**Output (human):**
```
workbench status: project_checkout-app/p1-checkout-revamp
  no-op      README.md
  FS→DB      tasks/1-update-checkout-ui.md
  DB→FS      tasks/3-add-cart-summary.md
  conflict   tasks/2-refactor-payment-form.md  CONFLICT [7]
```

**Schema effects (reads):** `workbench_sync_state`, `plans`, `tasks`, `artifacts`, `entity_links`, filesystem.

**Schema effects (writes):** None.

**Capture:** None.

**Exit codes:**
- `1` — plan not found or one or more malformed files detected.
- `3` — one or more conflicts detected.

---

### `planar workbench resolve <event-id> [--prefer fs|db]`

**Synopsis:**
```
planar workbench resolve <event-id> [--prefer fs|db] [--json]
```

**Description:** Settle a conflict surfaced by `workbench status` or `workbench sync`. The
`<event-id>` is the `sync_events.id` integer printed when a conflict is detected; it identifies
the durable conflict row written to `sync_events(scope='workbench', outcome='conflict')` during
the sync run. Applies the chosen side:

- `--prefer fs`: applies the FS file content to the DB entity.
- `--prefer db`: overwrites the FS file with the DB-rendered content.

After resolution the manifest row is updated so that subsequent status reports are clean, and
the `sync_events` row outcome is updated to `'resolved-fs'` or `'resolved-db'`.

**Arguments:**

| Argument | Description |
|----------|-------------|
| `<event-id>` | The `sync_events.id` integer from the conflict row written during `workbench sync`. |

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--prefer fs\|db` | Which side wins. | (required) |

**Output (human):**
```
resolved conflict event 7  (prefer db)
```

**Schema effects (reads):** `sync_events`, `workbench_sync_state`, filesystem.

**Schema effects (writes):** `sync_events` (outcome update); entity table (on `--prefer fs`); filesystem (on `--prefer db`); `workbench_sync_state`.

**Capture:** Appends `session_entries` row with `prefix='action'`.

**Exit codes:**
- `1` — plan not found or conflict index out of range.
- `2` — I/O or DB error during resolution.

---

### `planar workbench sync <plan>`

**Synopsis:**
```
planar workbench sync <plan> [--json] [--verbose]
```

**Options:**

| Flag | Description |
|------|-------------|
| `--verbose` | Verbose text rendering of the sync result. |

**Description:** Full bidirectional reconciliation for the named anchor plan. Applies
non-conflicting FS→DB and DB→FS changes in a single pass. Conflicts are surfaced (exit 3)
and must be resolved with `workbench resolve` before the next sync will be clean. Malformed
files are counted separately, included in JSON as `malformed` / `malformed_files`, and make
sync exit 1. When both classes are present, malformed exit precedence applies.

**Arguments:**

| Argument | Description |
|----------|-------------|
| `<plan>` | Anchor plan ID or slug. |

**Output (human):**
```
workbench sync: project_checkout-app/p1-checkout-revamp
  FS→DB  tasks/1-update-checkout-ui.md
  DB→FS  tasks/3-add-cart-summary.md
  conflict  tasks/2-refactor-payment-form.md  CONFLICT [7]
```

**Schema effects (reads):** `workbench_sync_state`, all entity tables, filesystem.

**Schema effects (writes):** Entity tables (FS→DB side); filesystem (DB→FS side); `workbench_sync_state`.

**Capture:** Appends `session_entries` row with `prefix='action'`.

**Exit codes:**
- `1` — plan not found or one or more malformed files detected.
- `2` — I/O or DB error.
- `3` — one or more conflicts detected.

---

### `planar workbench archive <plan> [--filter-mode {failures,all}]`

**Synopsis:**
```
planar workbench archive <plan> [--filter-mode failures|all] [--json]
```

**Description:** Remove the FS tree for the named anchor plan's feature directory. The
database is not touched — all entity rows and manifest rows are retained. If the parent
association directory becomes empty after removal, it is also removed (best-effort).

**`--filter-mode`** is accepted for API symmetry with push/restore. The current archive
implementation deletes the on-disk tree without packaging it into a separate archive store,
so the flag is currently a no-op. Reserved for a future archive store (tarball, git stash,
etc.) that would honor the same filter semantics push uses.

**Arguments:**

| Argument | Description |
|----------|-------------|
| `<plan>` | Anchor plan ID or slug. |

**Output (human):**
```
archived: project_checkout-app/p1-checkout-revamp  (directory removed)
```

**Schema effects (reads):** `plans`, `associations`, `external_links`.

**Schema effects (writes):** None (DB unchanged). Removes the feature directory from filesystem.

**Capture:** Appends `session_entries` row with `prefix='action'`.

**Exit codes:**
- `1` — plan not found.
- `2` — I/O error removing directory.

---

### `planar workbench restore <plan> [--filter-mode {failures,all}]`

**Synopsis:**
```
planar workbench restore <plan> [--filter-mode failures|all] [--json]
```

**Description:** Recreate the FS tree from the DB for the named anchor plan. Idempotent
against existing trees — existing files are overwritten atomically if they differ from the
DB render. After restore the manifest and `.sync` file are fully up to date; a subsequent
`workbench status` reports all no-ops.

**Terminal-status filter.** Restore honors the same `--filter-mode` flag as push. Default
mode is `failures`: re-materialized FS files mirror what a fresh `workbench push` would write.
Pass `--filter-mode all` to extend the filter to success terminals as well. The anchor plan
itself is always written regardless of mode so feature-tree navigation remains intact.

**Arguments:**

| Argument | Description |
|----------|-------------|
| `<plan>` | Anchor plan ID or slug. |

**Output (human):**
```
restored: project_checkout-app/p1-checkout-revamp  (7 files written)
```

**Schema effects (reads):** All entity tables, `entity_links`, `associations`.

**Schema effects (writes):** Filesystem (Markdown files + `.sync` manifest); upserts `workbench_sync_state` rows.

**Capture:** Appends `session_entries` row with `prefix='action'`.

**Exit codes:**
- `1` — plan not found.
- `2` — I/O error.

---

### `planar workbench gc [<plan>] [--filter-mode {failures,all}] [--dry-run] [--yes] [--all-scopes] [--json]`

**Synopsis:**
```
planar workbench gc <plan> [--filter-mode failures|all] [--dry-run] [--yes]
planar workbench gc --all-scopes [--filter-mode failures|all] [--dry-run] [--yes]
```

**Description:** Walk the workbench tree for the named plan (or all plans with `--all-scopes`),
classify each `.md` file by its backing entity's status, and remove files for entities that
fall inside the active filter mode. FS-only — never mutates entity rows. The `workbench_sync_state`
row for each removed file is also dropped so subsequent pushes do not see the missing file as
drift.

**Drift refusal.** If any to-be-removed file has FS-content drift from its DB-stored hash
(i.e. the operator hand-edited the cancelled-task file and forgot to push), gc exits 1 with
the drift list and a hint to either `workbench pull` first or re-run with `--yes` to discard.

**Arguments:**

| Argument | Description |
|----------|-------------|
| `<plan>` | Anchor plan ID or slug. Required unless `--all-scopes` is set. |
| `--filter-mode failures` | (default) Remove only files for failure-terminal entities. |
| `--filter-mode all` | Also remove files for success-terminal entities (`done` tasks, etc.). |
| `--dry-run` | Preview only; print which files would be removed without touching disk. |
| `--yes` | Discard FS-content drift; remove drifted files anyway. |
| `--all-scopes` | Walk every top-level plan's workbench tree. |
| `--json` | Emit a machine-readable summary instead of human text. |

**Output (human):**
```
workbench gc: removed 3, kept 7, drifted-skipped 0, errors 0 (mode=failures)
```

**Exit codes:**
- `0` — success.
- `1` — drift refusal (non-empty drifted set without `--yes`), or invalid arguments.
- `2` — filesystem I/O error.

**See also:** `planar workbench push --apply-cleanup` (in-the-moment narrow-scope cleanup); `docs/concepts.md § Terminal-status filter` for the model.

---

### `planar workbench list`

**Synopsis:**
```
planar workbench list [--json]
```

**Description:** List all top-level plans that have a workbench directory under
`$PLANAR_WORKBENCH_ROOT`. Shows plan ID, title, status, and whether the FS tree is present.

**Output (human):**
```
id   title                  status   dir
1    Checkout Revamp        active   project_checkout-app/p1-checkout-revamp  [present]
2    Add Checkout RPC       done     project_checkout-platform/p2-add-checkout-rpc  [archived]
```

**Schema effects (reads):** `plans`, `associations`, `external_links`, filesystem.

**Schema effects (writes):** None.

**Capture:** None.

**Exit codes:**
- `1` — workbench root does not exist or is not a directory.

---

### `planar workbench publish <plan-id>`

**Synopsis:**
```
planar workbench publish <plan-id> --system <slug> [--json]
```

**Description:** Render the workbench files for the named anchor plan and push the rendered content to a registered external operational system (Jira, GitHub Issues) via the adapter layer. The destination system, credentials, and per-entity projection template come from the system registration (see `planar-ext ext list` / `planar-ext ext create`).

For richer per-entity counterpart creation (epics, issues, sub-issues with parent/child links) walking the full plan tree, use [`planar-ext ext propagate <plan-id> --system <slug>`](#planar-ext-ext-propagate-plan) (implemented for every strategy except the cut `github-projects-v2`), or `planar-ext ext propagate-one <system> --from <kind:id>` for a single entity. `workbench publish` pushes the rendered Markdown body; `ext propagate` creates one external counterpart per entity in the plan subtree.

**Arguments:**

| Argument | Description |
|----------|-------------|
| `<plan-id>` | Anchor plan ID (numeric) or slug. |

**Options:**

| Flag | Default | Description |
|------|---------|-------------|
| `--system <slug>` | _(required)_ | External system slug (must already be registered via `planar-ext ext create`). |
| `--json` | `false` | Emit a JSON result envelope on stdout. |

**Schema effects:** Renders the workbench tree, creates one remote mirror, then inserts the resulting `external_links` row and its initial successful `sync_events` row atomically. Refuses to publish when the plan already has a link on the named system.

**Capture:** None.

**Exit codes:**
- `0` — success.
- `1` — user error: `--system` missing, plan not found, system slug not registered.
- `2` — system error: adapter / HTTP failure, authentication failure.

---

### `planar workbench extract-questions <plan-ref>`

**Synopsis:**
```
planar workbench extract-questions <plan-ref> [--json]
```

**Description:** Scan every artifact in the named anchor plan's workbench directory for `## Open questions` sections. Each H3 heading (`### …`) that is a direct child of an `## Open questions` H2 is treated as one open question item. Returns a JSON array describing which artifact file each question item came from and the text of each heading. Artifact files with no `## Open questions` section are omitted from the output.

This helper is consumed by the `planar-planner` and `planar-ingestor` agents to auto-register question entities during spec authoring and re-ingestion. Calling it directly is useful for auditing which open questions a spec body currently declares before running the ingest cycle.

**Arguments:**

| Argument | Description |
|----------|-------------|
| `<plan-ref>` | Anchor plan ID or slug. Must be an anchor plan (no `parent_plan_id`). |

**Options:**

| Flag | Default | Description |
|------|---------|-------------|
| `--json` | false | Emit JSON to stdout instead of a human-readable table. Always implied when the output is not a terminal (pipe / redirect). |

**Output (JSON):**

Each element of the returned array corresponds to one artifact file that contained at least one open question item:

```json
[
  {
    "artifact_id": 17,
    "file": "tech-spec.md",
    "questions": [
      "Which date format for export timestamps?",
      "Should partial rows be silently skipped or cause an error?"
    ]
  },
  {
    "artifact_id": 18,
    "file": "product-spec.md",
    "questions": [
      "Do we need a download-progress indicator?"
    ]
  }
]
```

**Human-readable output (default):**

```
tech-spec.md (artifact:17)
  1. Which date format for export timestamps?
  2. Should partial rows be silently skipped or cause an error?

product-spec.md (artifact:18)
  1. Do we need a download-progress indicator?

3 question(s) across 2 file(s)
```

**Schema effects:** Read-only. Reads `plans`, `artifacts`, and the workbench filesystem. No writes.

**Capture:** None.

**Exit codes:**
- `0` — success (zero question items is a valid successful result).
- `1` — plan not found, or plan is a child plan rather than an anchor.

---

## Domain: `workspace`

Manages workspace state directories — the per-org canonical AGENTS.md surface, the structured routing table, and the symlink lifecycle at the workspace root. A workspace is identified by an `associations` row of `kind=org`; its state directory is `${PLANAR_HOME:-~/.planar}/workspaces/<org_id>/`. See [concepts.md § Workspace](concepts.md#workspace) and [architecture.md § Workspace State Directory Model](architecture.md#workspace-state-directory-model).

**Workspace resolution:** the `[<workspace>]` positional argument on `routing build`, `routing show`, and `regenerate` accepts a numeric org id, an `org:<slug>` form, or a bare slug. When omitted, the command resolves the workspace from the only `kind=org` row in the database; multiple candidates produce exit 1 with an "explicit workspace required" message.

---

### `planar workspace init`

**Synopsis:**
```
planar workspace init [--name <text>] [--slug <text>] [--scan <N>] [--meta-repo] [--no-scan] [--enrich]
```

**Description:** Create an org association for the current working directory and register every immediate child directory containing a `.git` as a project member of that org. Refuses if cwd has its own `.git` unless `--meta-repo` is passed (use `planar init` instead for ordinary single repos) or if cwd contains zero `.git` children (a workspace must contain repos). In meta-repo mode, cwd must be a git repository; the root repo itself is registered as a project member, nested `.git` directories and submodule-style `.git` files are scanned, and the org config records `"workspace_shape":"meta-repo"`. Idempotent on re-run from the same root: an existing org with the same slug and root is reused; existing projects keep their state; missing memberships are added. In meta-repo mode, an existing org slug whose recorded `root_path` points at a different directory is refused and its `config_json` is left unchanged.

After the org + projects are committed, init runs a pipeline pass that (1) builds the static routing table, (2) regenerates AGENTS.md from the template, and (3) installs `AGENTS.md` / `CLAUDE.md` symlinks at the workspace root for sibling workspaces (copy fallback on filesystems that reject symlinks). In meta-repo mode, step 3 is skipped: root-level instruction files are repo-owned and are not created, copied, symlinked, overwritten, or repaired. Pipeline failures do not roll back the DB writes; they surface as a stderr warning with a hint to run `planar workspace doctor`, `planar workspace routing build`, and `planar workspace regenerate`.

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--name <text>` | Human-readable org name. | cwd basename |
| `--slug <text>` | Org slug. | Derived from cwd basename. |
| `--scan <N>` | Walk N levels deep when scanning for child repos. | `1` (immediate children only) |
| `--meta-repo` | Treat the cwd git repository as a workspace container and member project, scanning nested repos/submodules. | off |
| `--no-scan` | Skip the post-init pipeline (routing build + regenerate + symlinks). | off |
| `--enrich` | Merge cached LLM enrichment results into the routing table (equivalent to `routing build --enrich`). Cannot combine with `--no-scan`. | off |

**Output (human):**
```
created org:work (Work)
  ├─ project:repo-a   [/home/user/work/repo-a]   (auto-created, member-of org:work)
  ├─ project:repo-b   [/home/user/work/repo-b]   (auto-created, member-of org:work)
  └─ project:repo-c   [/home/user/work/repo-c]   (auto-created, member-of org:work)

3 repos initialized as projects, all members of org:work.
Routing table refreshed (3 projects, 1 cross-repo deps).
AGENTS.md regenerated (1842 bytes).
Symlinks installed: AGENTS.md, CLAUDE.md (strategy: symlink).
Run `planar assoc tree` to view the hierarchy.
```

**Output (`--json`):**
```json
{
  "org":      {"id":1,"slug":"work","name":"Work","created":true},
  "projects": [{"slug":"repo-a","path":"/home/user/work/repo-a","created":true,"membership_created":true}],
  "pipeline": {
    "skipped":     false,
    "routing":    {"project_count":3,"cross_repo_deps":1,"enrich_enabled":false,"enrich_misses":0},
    "regenerate": {"agents_path":"/home/user/.planar/workspaces/1/AGENTS.md","bytes":1842},
    "symlinks":   {"strategy":"symlink","installed":["AGENTS.md","CLAUDE.md"]}
  }
}
```

**Schema effects:** Inserts/preserves `associations(kind='org')`, `projects`, `project_associations`. The association `config_json` stores `root_path`, plus `workspace_shape: "meta-repo"` when `--meta-repo` is used. Writes `~/.planar/workspaces/<org_id>/AGENTS.md` and `routing-table.json`. Sibling workspaces install symlinks (or copies) at the workspace root; meta workspaces leave root `AGENTS.md` / `CLAUDE.md` untouched.

**Exit codes:**
- `0` — success.
- `1` — cwd has its own `.git` without `--meta-repo`; or `--meta-repo` was used outside a git repo; or cwd contains zero `.git` children; or `--no-scan` was combined with `--enrich`; or `--meta-repo` reused an existing org slug whose recorded root points elsewhere.
- `2` — migration, DB, or pipeline I/O failure.

---

### `planar workspace doctor`

**Synopsis:**
```
planar workspace doctor
```

**Description:** Walk every `associations` row with `kind=org` and reconcile its on-disk state. For each org, doctor verifies the state directory exists (creating it if missing) and, for sibling workspaces, verifies that the `AGENTS.md` / `CLAUDE.md` symlinks at the workspace root point at the canonical target (reinstalling them if not). Doctor only repairs root guidance files when `config_json` is valid and positively describes a non-meta workspace. For meta workspaces (`config_json.workspace_shape == "meta-repo"`), root instruction files are skipped entirely because they are repo-owned. If the workspace config is missing, malformed, or has an unknown shape, doctor reports a deterministic `error` issue and skips root guidance repair rather than guessing. Missing canonical `AGENTS.md` or `routing-table.json` under the state directory are reported as `missing` with a hint to run `planar workspace regenerate` — doctor does not regenerate content itself. Idempotent: a second invocation against the same fleet produces an `ok` summary per org whose state is complete.

**Output (human):**
```
fix: created state dir /home/user/.planar/workspaces/1
fix: reinstalled symlink /home/user/work/AGENTS.md → /home/user/.planar/workspaces/1/AGENTS.md
org:work repaired 2 issues
org:side ok
```

**Output (`--json`):**
```json
{"orgs":[{"slug":"work","org_id":1,"issues_found":2,"issues_repaired":[
  {"kind":"fix","detail":"created state dir /home/user/.planar/workspaces/1"},
  {"kind":"fix","detail":"reinstalled symlink /home/user/work/AGENTS.md → ..."}
]}]}
```

**Schema effects:** None. Reads `associations`; writes the filesystem under `~/.planar/workspaces/<org_id>/` and at each org's workspace root.

**Exit codes:**
- `0` — success (including the "no orgs registered" case).
- `2` — DB open failure.

---

### `planar workspace routing build [<workspace>]`

**Synopsis:**
```
planar workspace routing build [<workspace>] [--enrich]
```

**Description:** Scan the workspace's member projects and rewrite `<state-dir>/routing-table.json`. Deterministic and in-process by default (no LLM): reads each project's README first paragraph (becomes `summary`), runs manifest detection for capability tags (loaded from `~/.planar/templates/workspace-capabilities.toml` when present), infers cross-repo dependencies from `go.mod` replace directives and `package.json` workspace deps, walks the repo for a language census and entry-point detection, and refreshes the `planar_focus` block via live DB queries (open tasks, open questions, active plans). Operator overrides from `<state-dir>/routing-table-overrides.json` are merged on every build and always win.

With `--enrich`, the builder additionally consults the workspace-enrichment cache at `~/.planar/cache/workspace-enrichment/<org_id>/` and merges any LLM result whose fingerprint matches the project's current content. Cache misses emit a per-project hint to run the workspace-scan flow. If the workspace's `config.toml` declares an `enrich_command`, the builder shells out to it on cache miss (stdin = Request JSON, stdout = Result JSON, bounded by `enrich_timeout_seconds`, default 30); validation failures are warnings, never build failures. Merge precedence (highest first): manual overrides → LLM enrichment → static signals.

**Output (human):**
```
built /home/user/.planar/workspaces/1/routing-table.json (3 projects, 1 cross-repo deps)
```

**Output (`--json`):**
```json
{"path":"/home/user/.planar/workspaces/1/routing-table.json","projects":3,"dependency_edges":1,"enrich_enabled":false,"enrich_misses":0}
```

**Schema effects:** Reads `associations`, `projects`, `project_associations`, `plans`, `tasks`, `questions`, `entity_links`. Writes `<state-dir>/routing-table.json` atomically (temp + rename).

**Exit codes:**
- `0` — success.
- `1` — workspace cannot be resolved (no org registered, or `[<workspace>]` omitted with multiple candidates), or the named workspace does not exist.
- `2` — DB or filesystem I/O failure.

---

### `planar workspace routing show [<workspace>]`

**Synopsis:**
```
planar workspace routing show [<workspace>] [--json]
```

**Description:** Print `<state-dir>/routing-table.json`. Without `--json`, formats one block per project (slug, path, capabilities, summary, depends-on, open task / question counts) followed by a cross-repo edge list. With `--json` (or the global `--json` flag), the raw routing-table JSON is echoed verbatim for downstream tooling. Errors when `routing-table.json` does not exist with a hint to run `routing build` first.

**Output (human):**
```
workspace: org:work (id 1)
generated: 2026-05-18T... (static-v1)
projects:  3

- repo-a
    path:         /home/user/work/repo-a
    capabilities: go-service, grpc
    summary:      Customer-facing API service
    depends_on:   repo-b
    open tasks:   14
    open Qs:      3
- repo-b
    ...

cross-repo edges:
  repo-a -> repo-b (go.mod replace)
```

**Schema effects:** None — read-only.

**Exit codes:**
- `0` — success.
- `1` — workspace not resolvable, or routing table missing.
- `2` — I/O or JSON decode failure.

---

### `planar workspace regenerate [<workspace>]`

**Synopsis:**
```
planar workspace regenerate [<workspace>]
```

**Description:** Render the canonical `AGENTS.md` for a workspace. Reads `<state-dir>/routing-table.json` (produced by `routing build`), re-fetches live cross-repo plans and open questions from the database (these go stale fast, so the regenerator queries them every run rather than trusting cached counts), renders the AGENTS.md template (operator-installed at `~/.planar/templates/doc-prompts/agents.md` or the embedded fallback), always includes the host build and test queue rule exactly once (the text `planar-agent queue rule` prints: the default templates place it with `{{.QueueRule}}`, and a template that does not place it gets it appended after the rendered text), and atomically writes the result to `<state-dir>/AGENTS.md`. Errors with an explicit "run routing build first" hint when `routing-table.json` is missing.

**Output (human):**
```
regenerated AGENTS.md for org:work (3 projects, 1842 bytes)
```

**Output (`--json`):**
```json
{"agents_path":"/home/user/.planar/workspaces/1/AGENTS.md","project_count":3,"bytes_written":1842}
```

**Schema effects:** Reads `associations`, `plans`, `tasks`, `questions`. Writes `<state-dir>/AGENTS.md` atomically. The symlinks at the workspace root are not touched (they already point at the canonical target).

**Exit codes:**
- `0` — success.
- `1` — workspace not resolvable; or `routing-table.json` missing.
- `2` — DB, template, or filesystem error.

---

## Domain: `ext`

Commands for registering and interacting with external operational plane systems (Jira, GitHub Issues, etc.).

---

### `planar-ext ext register jira <slug>`

**Synopsis:**
```
planar-ext ext register jira <slug> --base-url <url> --project <key> --auth-env <var>
```

**Description:** Register a Jira instance as an external system.

**Arguments:**

| Argument | Description |
|----------|-------------|
| `<slug>` | User-defined unique slug, e.g. `acme-jira`. |

**Options:**

| Flag | Description | Required |
|------|-------------|----------|
| `--base-url <url>` | Jira base URL, e.g. `https://acme.atlassian.net`. | yes |
| `--project <key>` | Default Jira project key, e.g. `PROJ`. | yes |
| `--auth-env <var>` | Environment variable name holding the API token. Sets `auth_method='token-env'`, `auth_ref=<var>`. | yes |

**Output (`--json`):**
```json
{"ok":true,"id":1,"slug":"acme-jira","kind":"jira"}
```

**Schema effects:** Inserts into `external_systems(kind='jira', slug, base_url, default_project, auth_method='token-env', auth_ref)`.

**Capture:** None.

**Exit codes:**
- `1` — slug already exists.

---

### `planar-ext ext register github <slug>`

**Synopsis:**
```
planar-ext ext register github <slug> --project <owner/repo> [--auth-env <var>] [--json]
```

**Description:** Register a GitHub Issues instance as an external system.

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--project <owner/repo>` | GitHub repository, e.g. `acme/widgets`. | Required. |
| `--auth-env <var>` | Use an environment variable token. Sets `auth_method='token-env'`. When omitted the adapter shells out to `gh auth token` (`auth_method='gh-cli'`). | — |

**Schema effects:** Inserts into `external_systems(kind='github-issues', slug, base_url='https://api.github.com', default_project, auth_method, auth_ref)`.

**Capture:** None.

---

### `planar-ext ext list`

**Synopsis:**
```
planar-ext ext list
```

**Description:** List all registered external systems.

**Output (human):**
```
slug        kind             base-url                      project
acme-jira   jira             https://acme.atlassian.net    PROJ
side-gh     github-issues    https://api.github.com        acme/widgets
```

**Output (`--json`):** One object per system (auth_ref omitted from output for safety; auth_method included).

**Schema effects:** Reads `external_systems`.

**Capture:** None.

---

### `planar-ext ext test <slug>`

**Synopsis:**
```
planar-ext ext test <slug>
```

**Description:** Verify authentication and network reachability for the named external system. Makes a lightweight read-only API call (e.g. fetch a single issue or project metadata).

**Output (human):**
```
acme-jira: ok  (Jira 9.4.2, project PROJ found, auth valid)
```

**Output (`--json`):**
```json
{"slug":"acme-jira","ok":true,"system_version":"9.4.2","latency_ms":142}
```

**Schema effects:** Reads `external_systems`. No writes on success. On failure, does not create a `sync_events` row.

**Capture:** None.

**Exit codes:**
- `1` — system not found.
- `2` — authentication failed or system unreachable.

---

### `planar-ext ext create <system-slug> --from <kind:id>`

**Synopsis:**
```
planar-ext ext create <system-slug> --from <kind:id> [--type <issue-type>] [--role <link-role>] [--sync <direction>] [--scope <scope>]
```

**Description:** Create a counterpart for an existing local entity on the named external system, then record the link. This is the automation entry point for surfacing local work to the operational plane.

**Scope guard:** NONE. `ext create --from` performs no scope comparison against the `--from` entity. See [Cross-scope guard](#cross-scope-guard).

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--from <kind:id>` | Source local entity. | Required. |
| `--type <issue-type>` | External issue type (e.g. `Epic`, `Story` for Jira). | System default. |
| `--role <link-role>` | Link role: `mirror`, `parent`, `child`, `reference`. | `mirror` |
| `--sync <direction>` | Sync direction: `read-only`, `write-back`, `two-way`. | `two-way` |
| `--scope <scope>` | Declared and never read by the handler. | none |

**Output (human):**
```
created PROJ-1234 on acme-jira for task:42
link id: 7  (two-way mirror)
```

**Output (`--json`):**
```json
{"ok":true,"link_id":7,"external_id":"PROJ-1234","external_url":"https://acme.atlassian.net/browse/PROJ-1234","sync_direction":"two-way"}
```

**Schema effects:**
- Calls adapter `create(local_entity, options)` → `external_id`, `external_url`.
- Inserts into `external_links(entity_kind, entity_id, system_id, external_id, external_url, link_role, sync_direction, last_sync_status='ok', last_synced_at=now())`.
- Inserts into `sync_events(link_id, direction='push', outcome='ok', fields_changed=<all fields>, at=now())`.

**Capture:** Appends `session_entries` row with `prefix='action'`.

**Exit codes:**
- `1` — system or source entity not found.
- `2` — adapter `create` call failed.

---

### `planar-ext ext propagate <plan>`

> **Every reachable strategy is implemented except `github-projects-v2`.**
> The `ext`/`sync` verb family moved to `planar-ext` at task 6419; task 6421
> ported the single-repo `github-parent-issue` engine, and task 6451 added
> the generic per-entity tree walk that `jira-epic`, `github-zero-repo` and
> `github-tracking-issue` run through (the same `propagate_one_entity` core
> `ext propagate-one` uses, applied to every entry of the feature tree). The
> only refusal is `github-projects-v2` — the multi-repo GitHub strategy was
> cut from the C++ rewrite by decision 1001 — which exits `1` with a message
> naming that cut, whether it was auto-detected for a multi-repo feature or
> requested via `--github-strategy projects-v2`.

**Synopsis:**
```
planar-ext ext propagate <plan> [--system <slug>] [--dry-run] [--github-strategy <s>]
                            [--restrategize [--yes]]
                            [--verify-counterparts [--unlink | --recreate]]
                            [--scope <slug>] [--sync <direction>] [--json]
```

**Description:** Push a feature tree to the operational plane: create external counterparts for the anchor plan and all descendant child plans and tasks that do not yet have a `mirror` link. The propagation strategy is selected per [ADR-0006](adrs.md): a Jira system always uses the epic hierarchy (`jira-epic`); a GitHub system uses `github-parent-issue` (parent issue plus sub-issues) when the feature touches exactly one repo, `github-zero-repo` when it touches none, and `github-projects-v2` when it touches several — the last of which refuses (decision 1001). `--github-strategy` overrides the auto-detected GitHub strategy outright.

**Strategy stickiness (Phase C):** The chosen strategy is cached on `external_links.config_json` of the anchor plan at first propagation. Subsequent reruns honor the cached strategy even if the repo count later changes. Strategy is not re-evaluated automatically; use `--restrategize` to rebuild.

**Scope guard:** UNGUARDED BY DESIGN — `--scope` is accepted and discarded; `external_links` carries no scope column. See [Cross-scope guard](#cross-scope-guard).

Idempotent: entities that already have an `external_links(link_role='mirror')` row for the target system are skipped without error.

After propagation, run `planar workbench push <plan>` separately to update workbench front matter with the newly captured external keys.

**Arguments:**

| Argument | Description |
|----------|-------------|
| `<plan>` | Anchor plan id (integer) or slug. |

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--system <slug>` | External system slug. | First registered system. |
| `--dry-run` | Print what would be created without contacting the remote. | `false` |
| `--github-strategy <s>` | Override the auto-detected GitHub strategy: `parent-issue`, `projects-v2` (always refused, decision 1001) or `tracking-issue`. Bypasses the repo-count detection entirely. Only valid against a `github-issues` system (exit 1 otherwise) and mutually exclusive with `--restrategize`. | auto-detected |
| `--restrategize` | Force fresh strategy detection; prompts for confirmation if the strategy changes. On confirmation, prior counterparts are abandoned (NOT deleted from the remote) and `sync_events(outcome='strategy-abandoned')` rows are written for audit. Fresh propagation then proceeds under the new strategy. | `false` |
| `--yes` | Auto-confirm the `--restrategize` prompt without interactive input. No effect without `--restrategize`. | `false` |
| `--verify-counterparts` | Probe the remote to confirm every already-linked entity still exists. Missing counterparts (404) are reported as `Missing` and `sync_events(outcome='counterpart-missing')` rows are written. Off by default — probing on every run is expensive on large features. | `false` |
| `--unlink` | Remove `external_links` rows for missing counterparts (requires `--verify-counterparts`). The entity is then treated as "to create" on the next propagation. Mutually exclusive with `--recreate`. | `false` |
| `--recreate` | Remove the `external_links` row for missing counterparts and immediately re-create them (requires `--verify-counterparts`). Mutually exclusive with `--unlink`. | `false` |
| `--sync <direction>` | Sync direction applied to every `external_links` row created by this propagation. Accepted values: `read-only`, `write-back`, `two-way`. **Behavior change from prior versions:** the propagate flow previously defaulted to `two-way`; the new default is `read-only`. Users with downstream tooling that depended on the implicit two-way write must pass `--sync two-way` explicitly going forward. | `read-only` |
| `--scope <slug>` | Declared for parity with every other write verb but discarded — `external_links` carries no scope column and this verb is UNGUARDED BY DESIGN (see [Cross-scope guard](#cross-scope-guard)). | (off) |

**Output (human):**
```
propagate plan:7 → my-gh (github-parent-issue): 9 created, 0 skipped, 0 failed
  created   plan:7 "Add Checkout RPC" → acme/checkout#1
  created   plan:8 "Protos Changes" → acme/checkout#2
  ...
```

With `--verify-counterparts`:
```
propagate plan:7 → my-gh (github-parent-issue): 0 created, 9 skipped, 0 failed, 9 verified
  verified  plan:7 "Add Checkout RPC"  (acme/checkout#1 still present)
  ...
```

**Output (`--json`):**
```json
{"ok":true,"plan_id":7,"system":"my-gh","strategy":"github-parent-issue","created":9,"skipped":0,"failed":0}
```

The JSON shape gains `verified`, `abandoned`, `partial`, `missing`, and `warnings` fields (all zero/empty on a clean propagation).

**Schema effects:** Inserts into `external_links(link_role='mirror')` and `sync_events(outcome='ok')` per created entity. Each entity's writes are in a separate transaction. `--restrategize` writes `sync_events(outcome='strategy-abandoned')` per abandoned counterpart and deletes the corresponding `external_links` rows. `--verify-counterparts` writes `sync_events(outcome='counterpart-missing')` for missing entities. Partial entity failures write `sync_events(outcome='partial')` for resumability.

**Capture:** Session entries not appended (propagation is a bulk operation; `sync_events` rows serve as the audit trail).

**Exit codes:**
- `0` — success (including a fully-skipped rerun or fully-verified rerun).
- `1` — plan not found, no registered system, entity-level failures, or missing counterparts reported without `--unlink` / `--recreate`.
- `2` — adapter or database failure.

---

### `planar-ext ext propagate-one <system> --from <kind:id>`

**Synopsis:**
```
planar-ext ext propagate-one <system> --from <kind:id> [--strategy <value>] [--sync <direction>] [--dry-run] [--json]
```

**Description:** Render one entity's propagation template, POST the counterpart to the named external system, and record the resulting `external_links(link_role='mirror')` row in a single transaction. Designed for targeted one-off propagation (e.g. a missing entity after a bulk `ext propagate` run) and for orchestrator dispatch that creates counterparts one task at a time.

Idempotent: if a mirror link already exists for the `(entity, system)` pair the call is a no-op and returns `op=skipped`. No duplicate counterparts are created on repeated invocations.

**Arguments:**

| Argument | Description |
|----------|-------------|
| `<system>` | External system slug (must be registered via `planar-ext ext register`). |

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--from <kind:id>` | Source local entity. Accepts `plan:N` or `task:N`. | Required. |
| `--strategy <value>` | Override the per-entity strategy for GitHub systems. `tracking-issue` is the only accepted value — it is for an entity with no determinable repo. `parent-issue` and `projects-v2` are recognized only to be refused, pointing at the whole-tree `ext propagate` instead. GitHub-only; rejected for non-GitHub systems. | (auto-detect: `parent-issue`) |
| `--sync <direction>` | Sync direction for the created `external_links` row. Accepted values: `read-only`, `write-back`, `two-way`. | `read-only` |
| `--dry-run` | Preview: render the template and report what would be POSTed without contacting the remote system. | off |
| `--json` | Emit a JSON result object. | off |

**Output (human):**
```
created PROJ-42 on acme-jira for task:7
link id: 11  (read-only mirror)
```

On a skipped (already-linked) invocation:
```
skipped task:7 on acme-jira: mirror link already exists (link id: 11)
```

**Output (`--json`):**
```json
{"ok":true,"op":"created","link_id":11,"entity_kind":"task","entity_id":7,"system":"acme-jira","external_id":"PROJ-42","external_url":"https://acme.atlassian.net/browse/PROJ-42"}
```

On skip: `{"ok":true,"op":"skipped","link_id":11}`.

**Schema effects:**
- On create: calls adapter → inserts `external_links(entity_kind, entity_id, system_id, external_id, external_url, link_role='mirror', sync_direction, last_sync_status='ok')` and `sync_events(direction='push', outcome='ok')`.
- On skip or `--dry-run`: no writes.

**Capture:** None.

**Exit codes:**
- `0` — success (created or skipped).
- `1` — `--from` entity or `<system>` not found; `--strategy` value invalid for the target system.
- `2` — adapter failure (remote HTTP error).

---

## Domain: `link` / `unlink`

Links record relationships between local entities and external tickets. These are top-level commands matching the tech spec's literal CLI examples, not nested under `ext`.

---

### `planar link <kind:id> --to <system-slug>:<external-id>`

**Synopsis:**
```
planar link <kind:id> --to <system-slug>:<external-id> [--role <link-role>] [--sync <direction>] [--scope <scope>]
```

**Description:** Manually record an `external_links` row linking a local entity to an already-existing external ticket. Use this when the external ticket was created outside of `ext create`. Does not push any data to the external system.

**Scope guard:** NONE — and unguarded **by design**: `entity_links` edges legitimately cross scopes (the polyrepo `touches`/`derives-from` workflow). See [Cross-scope guard](#cross-scope-guard).

**Arguments:**

| Argument | Description |
|----------|-------------|
| `<kind:id>` | Local entity reference, e.g. `task:42`, `plan:7`. |

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--to <system-slug>:<external-id>` | External reference, e.g. `acme-jira:PROJ-1234`. | Required. |
| `--role <link-role>` | One of `mirror`, `parent`, `child`, `reference`. | `reference` |
| `--sync <direction>` | One of `read-only`, `write-back`, `two-way`. | `read-only` |
| `--propagate` | After creating the link, propagate the anchor plan of the linked entity to its registered external system. Runs the equivalent of `planar-ext ext propagate` against the top-level plan and shares its one refusal (`github-projects-v2`, decision 1001). | `false` |
| `--scope <scope>` | Declared and never read by the handler. | none |

**Output (`--json`):**
```json
{"ok":true,"link_id":8,"entity_kind":"task","entity_id":42,"external_id":"PROJ-1234","system_id":1}
```

**Schema effects:** Inserts into `external_links(entity_kind, entity_id, system_id, external_id, external_url=null, link_role, sync_direction, last_sync_status='never')`. If `--propagate` is set, also inserts into `external_links(link_role='mirror')` and `sync_events` for propagated entities.

**Capture:** Appends `session_entries` row with `prefix='action'`.

**Exit codes:**
- `1` — entity or external system not found.
- `1` — link already exists (UNIQUE constraint: `entity_kind`, `entity_id`, `system_id`, `external_id`).
- `1` — propagation failed (when `--propagate` is set and propagation encounters an error).

---

### `planar unlink <link-id>`

**Synopsis:**
```
planar unlink <link-id> [--scope <scope>]
```

**Options:**

| Flag | Description |
|------|-------------|
| `--scope <scope>` | Scope for the cross-scope guard (currently informational). |

**Description:** Remove an `external_links` row by link id. Associated
`sync_events` rows are retained with `link_id=null` by the foreign key's `ON
DELETE SET NULL` action, but they are detached from the deleted link and no
longer reachable through `audit trail --link`.

This is not a lossless way to change sync direction. Recreating the binding
with `planar link` gives it a new row id and resets `external_url` and
`config_json` (including any cached propagation strategy) to null,
`last_synced_at` to null, and `last_sync_status` to `never`; the old sync-event
history is not attached to the replacement. The public CLI cannot export or
restore the exact old `external_url`, `config_json`, link role, or sync
direction, so do not unlink unless those losses are acceptable and the role
and desired direction are known independently.

**Scope guard:** NONE — unguarded by design, for the same reason as `link`. See [Cross-scope guard](#cross-scope-guard).

**Schema effects:**
- Deletes from `external_links(id)`.
- Sets matching `sync_events.link_id` values to null via FK.

**Capture:** Appends `session_entries` row with `prefix='action'`.

**Exit codes:**
- `1` — link id not found.

---

## Domain: `sync`

Sync commands pull and push data between the local plane and registered external systems. Sync is always on-demand; no background daemon.

---

### `planar-ext sync pull <target>`

**Synopsis:**
```
planar-ext sync pull <link-id | kind:id | --all> [--system <slug>] [--scope <slug>]
```

**Description:** Pull remote state for one or more links and report it. Updates `external_links.last_synced_at`. **Does not write to the local entity (decision 996).** `planar-ext` fetches and emits remote state; it never mirrors a field onto a task, plan, question, or artifact. When a link's `remote_title` and/or `remote_status` differ from the current local value, the result row carries the emitted value(s) — evidence for the caller to review, not a change already made. The intended flow is three steps: `planar-ext` fetches and emits (this command), an agent verifies/synthesizes/validates the emitted values, then the agent calls `planar` to create or update the planning entity if warranted. Records a `sync_events` row per link touched.

**Scope guard:** Single-target invocations (`<link-id>` or `<kind:id>`) refuse when the operator's resolved write scope disagrees with the local entity referenced by any resolved link. `--all` invocations are not guarded (bulk fan-out is opt-in). See [Cross-scope guard](#cross-scope-guard).

**Arguments / Options:**

| Form | Description |
|------|-------------|
| `<link-id>` | Pull a specific link by id. |
| `<kind:id>` | Pull all links for the given local entity. |
| `--all` | Pull all links with `sync_direction` of `two-way` or `read-only`. |
| `--system <slug>` | Filter to links via a specific system (when used with `--all` or `kind:id`). |
| `--scope <slug>` | Explicit write-scope override for the target entity guard. |

**Output (human):**
```
pulled 3 links
  link 7: ok — title status — remote: title="Fix checkout race" status="In Progress"
  link 8: noop
  link 9: conflict — status — status diverged (local: done, remote: In Progress)
```

**Output (`--json`):** One object per link. `remote_title`/`remote_status` are omitted entirely when the corresponding field did not differ from local:
```json
{"link_id":7,"outcome":"ok","fields_changed":["title","status"],"remote_title":"Fix checkout race","remote_status":"In Progress"}
{"link_id":8,"outcome":"noop","fields_changed":[]}
{"link_id":9,"outcome":"conflict","fields_changed":["status"],"detail":"status: local=done remote=in-progress"}
```

**Schema effects:**
- Updates `external_links(last_synced_at, last_sync_status)` per link.
- Does NOT update the local entity's fields. `remote_title`/`remote_status` are emitted in the result only.
- Inserts into `sync_events(link_id, direction='pull', outcome, fields_changed, detail, at)` per link.

**Capture:** Appends `session_entries` row with `prefix='observation'`.

**Exit codes:**
- `0` — all links pulled (even if some were `noop`).
- `3` — at least one conflict detected.
- `2` — adapter error on one or more links.

---

### `planar-ext sync push <target>`

**Synopsis:**
```
planar-ext sync push <link-id | kind:id | --all> [--system <slug>] [--scope <slug>] [--json]
```

**Description:** Push selected local fields to the remote system for one or more links. For comment and decision posts, appends rather than replaces. Includes the correlation footer on every push.

**Scope guard:** Single-target invocations (`<link-id>` or `<kind:id>`) refuse when the operator's resolved write scope disagrees with the local entity referenced by any resolved link. `--all` invocations are not guarded (bulk fan-out is opt-in). See [Cross-scope guard](#cross-scope-guard).

`--scope <slug>` explicitly selects the write scope used by that guard.

**Schema effects:**
- Calls adapter `update(external_id, fields)` or `comment(external_id, body)`.
- Inserts into `sync_events(link_id, direction='push', outcome, fields_changed, detail, at)`.
- Updates `external_links(last_synced_at, last_sync_status)`.

**Capture:** Appends `session_entries` row with `prefix='action'`.

**Exit codes:**
- `0` — all pushes succeeded.
- `2` — adapter error.

---

### `planar-ext sync status`

**Synopsis:**
```
planar-ext sync status [--entity <kind:id>] [--system <slug>]
```

**Description:** Show the sync status of all links in scope. Highlights conflicts and errors.

**Output (human):**
```
link  entity    external-id    system      last-sync           status
7     task:42   PROJ-1234      acme-jira   2026-05-10T09:00Z   ok
8     plan:7    PROJ-100       acme-jira   2026-05-09T18:00Z   conflict
9     task:43   PROJ-1235      acme-jira   never               never
```

**Output (`--json`):** One object per link.

**Schema effects:** Reads `external_links`, `external_systems`, scoped by scope filter.

**Capture:** None.

---

### `planar-ext sync resolve <event-id> --keep <side>`

**Synopsis:**
```
planar-ext sync resolve <event-id> --keep <side>
  --evidence-token <sha256> --expected-local-updated-at <timestamp>
  [--scope <slug>]
```

**Description:** Resolve a sync conflict recorded in `sync_events`. `--keep local` keeps the local value and pushes it to the remote. `--keep remote` overwrites the local value with the remote value. Resolution is whole-entity; per-field resolution is not supported. Read the event through `planar audit trail --link <id> --json`: conflict rows expose an `evidence` object with exact local/remote field values, provenance, observation time, local `updated_at`, provider remote version (`updated`/`updated_at`), and the evidence token.

**Scope guard:** Refuses when the operator's resolved write scope disagrees with the local entity referenced by the link the event belongs to. See [Cross-scope guard](#cross-scope-guard).

**Arguments:**

| Argument | Description |
|----------|-------------|
| `<event-id>` | The `sync_events.id` of the conflict event to resolve. |

**Options:**

| Flag | Description | Required |
|------|-------------|----------|
| `--keep <side>` | `local` or `remote`. | yes |
| `--evidence-token <sha256>` | Exact token from the operator-approved conflict event evidence. | yes |
| `--expected-local-updated-at <timestamp>` | Exact approved local entity version. For manual merge, use the reviewed post-edit `updated_at`, not the original conflict value. | yes |
| `--scope <slug>` | Explicit write-scope override for the target entity guard. | no |

**Output (human):**
```
conflict resolved: event 15 — kept local value for status
```

**Schema effects:**
- Reads `sync_events(outcome='conflict')` and associated `external_links`.
- Rejects unless the event is the latest event for the link, the link remains conflicted, the stored evidence token matches, the local `updated_at` matches, the approved and freshly read provider versions are both non-empty, and the fresh adapter read still matches the recorded remote values and version. These checks run before either resolution mutation; missing or changed evidence requires defer, a fresh preview, and new approval. The local compare-and-swap plus fresh remote read narrows but cannot eliminate the provider GET-to-write race without a provider conditional-write primitive. After an ambiguous failure, inspect `audit trail --link`, `sync status --entity`, and the local entity before retrying.
- On `--keep local`: calls adapter `update` with the local value; inserts a fresh `sync_events` row with `direction='push'`, `outcome='ok'`, and `detail='resolved=local; from sync_event=<id>'`.
- On `--keep remote`: updates the local entity field; inserts a fresh `sync_events` row with `direction='pull'`, `outcome='ok'`, and `detail='resolved=remote; from sync_event=<id>'`.
- The `direction` value matches the originating action: `pull` for "remote-overwrites-local", `push` for "local-overwrites-remote". The `detail` field records which side won and which conflict event was resolved.
- Updates `external_links(last_sync_status='ok', last_synced_at=now())`.

Note: `sync_events.direction` accepts only `pull` or `push` per the schema CHECK constraint; no other direction values are used.

**Capture:** Appends `session_entries` row with `prefix='decision'`.

**Exit codes:**
- `1` — event not found.
- `1` — event is not a conflict.
- `2` — adapter push failed.

---

## Domain: `resume`

Resume commands implement the from-zero resumption contract: a new agent process, with no prior conversation history, can resume any captured task using a single command.

---

### `planar resume [<task-id>]`

**Synopsis:**
```
planar resume [<task-id> | <plan-id>]
```

**Description:** Produce a structured resume packet for the specified task or plan. If no id is given, targets the most recently active task in the cwd-derived scope set. Before producing the packet, pulls operational plane state for linked external systems if the last sync is outside the freshness window.

The resume packet contains:
1. **Identity** — task id, plan id (if any), title, goal, scope.
2. **State** — current status, exact next action, last action taken, timestamp.
3. **Plan position** — parent plan, completed steps, current step, remaining steps.
4. **Operational plane** — linked Jira/GitHub Issues with current remote state, conflict warnings.
5. **Recent activity** — session entries from the last N sessions, ranked by relevance, with structured prefixes.
6. **Decisions and questions** — decisions and open questions tied to this task or plan.
7. **Linked artifacts** — specs and ADRs related via `entity_links`.
8. **Audit footer** — previous agent session, vendor, and timestamp.

**Arguments:**

| Argument | Description |
|----------|-------------|
| `<task-id>` or `<plan-id>` | Id of the task or plan to resume. Optional; defaults to most recently active. |

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--json` | Emit the resume packet as a single JSON object instead of structured text. | off |

**Output:** The resume packet, formatted as structured text (human mode) or a single JSON object (`--json`). The JSON shape is:

```json
{
  "task": {"id":42,"title":"...","status":"doing","next_action":"...","scope_kind":"...","scope_id":...},
  "plan": {"id":7,"title":"...","steps":[...]},
  "operational": [{"link_id":8,"external_id":"PROJ-1234","status":"In Progress","conflicts":[]}],
  "recent_entries": [{"session_id":101,"prefix":"action","body":"...","created_at":"..."},...],
  "decisions": [{"id":5,"title":"...","status":"accepted"},...],
  "questions": [{"id":3,"title":"...","status":"open","answer_body":null},...],
  "artifacts": [{"id":3,"title":"...","kind":"tech_spec","relationship":"cites"},...],
  "audit": {"session_id":101,"vendor":"claude","started_at":"..."}
}
```

**Schema effects (reads):** `tasks`, `plans`, `plan_steps`, `sessions`, `session_entries`, `context_snapshots`, `decisions`, `questions`, `entity_links`, `artifacts`, `external_links`.
**Schema effects (writes):** If an operational pull is performed, updates `external_links(last_synced_at)` and inserts `sync_events` rows.

**Capture:** Appends `session_entries` row with `prefix='action'` in the current session.

**Exit codes:**
- `0` — packet produced.
- `1` — task not found; or no active task in scope (when called without an id).
- `1` — `resume validate` fails (e.g. a capture failure is detected).
- `3` — conflicts detected on operational pull; conflicts are included in the packet but exit code signals the condition.

---

### `planar resume validate <task-id>`

**Synopsis:**
```
planar resume validate <task-id>
```

**Description:** Check whether the specified task is resumable. Validates: task identity present, current status set, `next_action` not null, at least one recent `context_snapshots` row, operational plane state pulled within the freshness window.

Failure produces concrete remediation guidance, not a generic warning. Example:
```
FAIL task:42 is not resumable:
  - next_action is null → run: planar task update 42 --next-action "<text>"
  - no context snapshot → run: planar capture snapshot 42
  - operational sync stale (last pull: 3 days ago) → run: planar-ext sync pull task:42
```

**Output (`--json`):**
```json
{"task_id":42,"resumable":false,"failures":[
  {"check":"next_action","message":"next_action is null","remediation":"planar task update 42 --next-action \"<text>\""},
  {"check":"snapshot","message":"no context snapshot found","remediation":"planar capture snapshot 42"}
]}
```

**Schema effects:** Reads `tasks`, `context_snapshots`, `external_links`. No writes.

**Capture:** None (validation is read-only).

**Exit codes:**
- `0` — task is resumable.
- `1` — task fails one or more checks.

---

## Domain: `handoff`

Handoff commands support the end-of-session ritual: capturing a snapshot, validating it, and preparing the transition record.

---

### `planar handoff [<task-id>]`

**Synopsis:**
```
planar handoff [<task-id>] [--vendor <to-vendor>] [--note <text>]
```

**Description:** Capture a context snapshot for the current session (or the named task's session), then create a `handoffs` row in `pending` status. Designed to be run before terminating an agent process. The snapshot records the narrative, the exact `next_action`, and the vendor identity.

After capturing, automatically runs the equivalent of `handoff validate` and reports whether the handoff is resume-ready.

**Arguments:**

| Argument | Description |
|----------|-------------|
| `<task-id>` | Task to capture for. Defaults to the currently active task. |

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--vendor <to-vendor>` | Expected destination vendor (free-form string). Stored in `handoffs.to_vendor`. | none |
| `--note <text>` | Narrative note to include in the snapshot body. May be `@<file>`. | none |

**Output (human):**
```
handoff captured for task:42
  snapshot: 15  vendor: claude  next_action: Wire Stripe webhook handler
  handoff:  3   status: pending
  validate: PASS — task is resume-ready
```

**Output (`--json`):**
```json
{"ok":true,"snapshot_id":15,"handoff_id":3,"status":"pending","resumable":true,"failures":[]}
```

**Schema effects:**
- Inserts into `context_snapshots(session_id, task_id, vendor, vendor_session_id, body, next_action)`.
- Inserts into `handoffs(from_snapshot_id, from_vendor, to_vendor, status='pending')`.

**Capture:** Appends `session_entries` row with `prefix='note'`.

**Exit codes:**
- `1` — task not found or no active session.

---

### `planar handoff validate <handoff-id>`

**Synopsis:**
```
planar handoff validate <handoff-id> [--json] [--note <text>] [--vendor <v>]
```

**Options:**

| Flag | Description |
|------|-------------|
| `--note <text>` | Declared and never read by the handler. |
| `--vendor <v>` | Declared and never read by the handler. |

**Description:** Transition the `pending` handoff with the given id to `validated`, making it eligible for `handoff consume`. Checks the same readiness criteria as `resume validate` plus snapshot presence and handoff status. On success, writes the `validated` state to the handoff row.

**Output:** Same structure as `resume validate` output (add `--json` for a single JSON object).

**Schema effects:**
- Reads `context_snapshots`, `handoffs`, `tasks`.
- On success: updates `handoffs(status='validated', validated_at=strftime('%Y-%m-%dT%H:%M:%fZ','now'))` for the handoff with the given `<handoff-id>`.

**Capture:** None.

**Exit codes:**
- `0` — handoff is resume-ready; status updated to `validated`.
- `1` — `<handoff-id>` not found.
- `1` — handoff cannot transition to `validated` (not `pending`).
- `1` — no pending handoff found for this snapshot.

---

### `planar handoff list`

**Synopsis:**
```
planar handoff list [--status <status>] [--json] [--note <text>] [--vendor <v>]
```

**Description:** List handoffs by status.

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--status <status>` | Filter: `pending`, `validated`, `consumed`, `abandoned`. Repeatable. | `pending` |
| `--note <text>` | Declared and never read by the handler. | none |
| `--vendor <v>` | Declared and never read by the handler. | none |

There is no `--task` filter on `handoff list` (passing it exits 2); filter the output by the `task` column instead.

**Output (human):**
```
id  snapshot  task  from-vendor  to-vendor  status   created-at
3   15        42    claude       codex      pending  2026-05-10T09:00Z
```

**Schema effects:** Reads `handoffs`, `context_snapshots`, `tasks`.

**Capture:** None.

---

### `planar handoff create <snapshot-id> [--vendor <v>] [--note <text>] [--json]`

**Description:** Create a `pending` handoff anchored to an existing context snapshot (see `planar capture snapshot`). `--note` attaches a free-form handoff message; `--vendor` records the originating vendor. The handoff must then pass `handoff validate` before it is eligible for `handoff consume`.

**Schema effects:** Inserts a `handoffs` row with `status='pending'`.

---

### `planar handoff show <handoff-id> [--json]`

**Description:** Show one handoff's details — its status, anchoring snapshot, note, and timestamps. `--vendor <v>` and `--note <text>` are declared on this leaf and never read by the handler.

---

### `planar handoff abandon <handoff-id> [--reason <text>] [--vendor <v>] [--note <text>] [--json]`

**Description:** Abandon a non-terminal handoff (`pending` or `validated`), recording an optional `--reason`. Terminal handoffs (already `consumed`/`abandoned`) cannot be abandoned again.

**Schema effects:** Updates the `handoffs` row to `status='abandoned'`.

**Exit codes:**
- `1` — handoff not found, or already terminal.

---

### `planar handoff consume <handoff-id>`

**Synopsis:**
```
planar handoff consume <handoff-id> [--session <session-id>] [--json] [--note <text>] [--vendor <v>]
```

**Description:** Mark a handoff as consumed by the resuming session. Transitions `handoffs.status` from `pending` (or `validated`) to `consumed`. Typically called automatically by `resume` when it picks up a pending handoff, but exposed as an explicit command for agent scripts.

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--session <session-id>` | The session id of the resuming session. | Current session. |
| `--note <text>` | Declared and never read by the handler. | none |
| `--vendor <v>` | Declared and never read by the handler. | none |

**Schema effects:** Updates `handoffs(status='consumed', to_session_id=<session-id>, consumed_at=now())`.

**Capture:** Appends `session_entries` row with `prefix='action'`.

**Exit codes:**
- `1` — handoff not found or already consumed.

---

## Domain: `capture`

Capture commands manage explicit session management and context capture for cases where automatic capture is insufficient — typically narrative summaries, blocker explanations, or reasoning the agent wants to preserve verbatim.

The `context-history` term is vestigial from an earlier framing and is dropped. Equivalent narrative-history capture happens through `planar capture note` (see below).

---

### `planar capture session`

**Synopsis:**
```
planar capture session [--task <task-id>] [--vendor <vendor>] [--vendor-session-id <id>] [--model <model>] [--json]
```

**Description:** Explicitly open a new session row and make it the current active session for subsequent commands. Useful when automatic session creation behavior needs to be overridden (e.g. when starting a new agent process mid-task). When cwd is inside a git repo, the first successful open for that session also records `sessions.repo_root` and `sessions.head_sha_at_start`; reused opens are first-open-wins for those columns.

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--task <task-id>` | Associate the session with a task. | none |
| `--vendor <vendor>` | Vendor identity string. | `$PLANAR_VENDOR` or `"cli"`. |
| `--vendor-session-id <id>` | Vendor-specific session id. | `$PLANAR_VENDOR_SESSION_ID` or none. |
| `--model <model>` | Model identifier (free-form string). | none |

**Output (human):**
```
session 101 opened (vendor: claude, task: 42)
```

**Schema effects:** Inserts into `sessions(task_id, project_id, agent_id, vendor, vendor_session_id, model, started_at)`. On git-backed opens, also sets `sessions.repo_root` and `sessions.head_sha_at_start` if those columns are still `NULL`.

**Capture:** Appends `session_entries` row with `prefix='action'` marking session start.

---

### `planar capture end [<session-id>]`

**Synopsis:**
```
planar capture end [<session-id>] [--summary <text>] [--json] [--session <session-id>]
```

**Description:** Close the current (or specified) session by setting `sessions.ended_at`. Before the end timestamp is written, Planar attempts a fail-soft git walk over the operator session window `sessions.head_sha_at_start..HEAD` in `sessions.repo_root` and records any discovered commits into `session_commits`. Does not capture a snapshot; use `handoff` for end-of-session snapshot capture. If `--summary` is supplied, also writes a human-readable summary of the session to `sessions.summary`.

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--summary <text>` | Human-readable session summary. May be `@<file>`. | none |
| `--session <session-id>` | Session id to end. The `<session-id>` positional wins when both are given; with neither, the vendor tuple's active session is ended. | none |

**Schema effects:** Inserts zero or more rows into `session_commits(session_id, sha, repo_root, branch, subject, author, committed_at, recorded_at)` from the session's git window, then updates `sessions(ended_at=now())`. If `--summary` is given, also updates `sessions(summary=<text>)`.

**Capture:** Appends final `session_entries` row with `prefix='note'` marking session end.

**Exit codes:**
- `1` — session not found or already ended.

---

### `planar capture commits [<sha>...]`

**Synopsis:**
```
planar capture commits [--session <session-id>] [--repo <dir>] [--since <ref>] [<sha>...] [--json]
```

**Description:** Record explicit git commits into a session. This is the loud-fail operator correction path for anything the automatic claim/session windows miss: post-session attribution, multi-repo sessions, or recovery after a best-effort automatic window recorded nothing. The target session defaults to the active vendor session; `--session` can point at an ended session on purpose.

Exactly one input mode is allowed:
- `--since <ref>` walks `<ref>..HEAD` in the target repo.
- Positional SHAs record those exact commits in the order given.

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--session <session-id>` | Record into the named session instead of the active vendor session. Ended sessions are allowed. | active vendor session |
| `--repo <dir>` | Read commits from a repo other than cwd. | `.` |
| `--since <ref>` | Walk `<ref>..HEAD` and record every commit in that range. Mutually exclusive with positional SHAs. | none |
| `--json` | Emit a summary object instead of the human confirmation line. | off |

**Arguments:**

| Argument | Description |
|----------|-------------|
| `<sha>` | One or more explicit commit SHAs to attribute. Mutually exclusive with `--since`. |

**Output (human):**
```
session 101: processed 3 commits (2 new)
```

**Output (`--json`):**
```json
{"ok":true,"session_id":101,"repo_root":"/path/to/repo","commit_count":3,"inserted_count":2}
```

**Schema effects:** Reads `sessions`; inserts or ignores into `session_commits(session_id, sha, repo_root, branch, subject, author, committed_at, recorded_at)`. The unique key is `(session_id, sha)`, so rerunning an overlapping window is idempotent within one session.

**Capture:** None.

**Exit codes:**
- `1` — unknown session id; no active session when `--session` is omitted; non-git cwd/`--repo`; unresolvable `--since` ref; or one or more explicit SHAs could not be resolved.

---

### `planar capture note <body>`

**Synopsis:**
```
planar capture note <body> [--session <session-id>] [--json]
```

**Description:** Append a `note` entry to the current session. Use for narrative observations, reasoning, or context the agent wants to preserve verbatim.

**Arguments:**

| Argument | Description |
|----------|-------------|
| `<body>` | Note text. May be `@<file>`. |

**Schema effects:** Inserts into `session_entries(session_id, ordinal, prefix='note', body)`.

**Exit codes:**
- `1` — no active session.

---

### `planar capture command <command> [--outcome <text>]`

**Synopsis:**
```
planar capture command <command> [--outcome <text>] [--session <session-id>] [--json]
```

**Description:** Record a command that was run and (optionally) its outcome, as a structured `command` entry in the session timeline. Agents use this to preserve the record of shell commands and their results.

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--outcome <text>` | Summary of the command outcome. May be `@<file>`. | none |

**Schema effects:** Inserts into `session_entries(session_id, ordinal, prefix='command', body=<command + outcome>)`.

---

### `planar capture file <path> [--role <description>]`

**Synopsis:**
```
planar capture file <path> [--role <description>] [--session <session-id>] [--json]
```

**Description:** Record that a file was read, written, or otherwise touched during the session, with an optional role description (e.g. "implementation target", "read for context", "avoided — too large").

**Schema effects:** Inserts into `session_entries(session_id, ordinal, prefix='file', body=<path + role>)`.

---

### `planar capture snapshot [<body>]`

**Synopsis:**
```
planar capture snapshot [<body>] [--note <text>] [--next-action <text>] [--session <session-id>] [--task <task-id>] [--json]
```

**Options:**

| Flag | Description |
|------|-------------|
| `--session <session-id>` | Session id to snapshot against; otherwise the vendor tuple's active session (created when absent). |
| `--task <task-id>` | Task id for the snapshot; otherwise the session's own bound task. |
| `--note <text>` | Snapshot body. May be `@<file>`. Takes precedence over the `<body>` positional. |

The positional is the snapshot **body** (it may be `@<file>`), not a task id; name the task with `--task`.

**Description:** Produce a context snapshot for the current or named task mid-session, without creating a full handoff record. Useful for checkpointing state at meaningful points during long sessions.

**Schema effects:** Inserts into `context_snapshots(session_id, task_id, vendor, vendor_session_id, body, next_action)`. Reads `tasks.next_action` for the snapshot's `next_action` field.

**Capture:** Appends `session_entries` row with `prefix='note'`.

---

## Domain: `audit`

Audit commands produce cross-plane audit trails linking local sessions, decisions, and sync events to external tickets.

---

### `planar audit trail --link <link-id>`

**Synopsis:**
```
planar audit trail --link <link-id> [--json]
```

**Description:** Show every local session, decision, sync event, and attributed commit tied to the given external link, with timestamps and vendor identity. This is the inverse view: starting from an external ticket's link id, reconstruct the full history of local work that produced changes to it. The commits leg is sourced from `session_commits` through the link's local entity sessions and is omitted entirely when no commits were recorded.

The positional form, `planar audit trail [--kind <kind>] [--grep <pattern>] <entity-id>`, is entity-scoped; use `--link <link-id>` for the external-link trail shown here. `--kind` defaults to `task`. `--grep` applies to the positional form only, and switches to a separate query that does not widen through `entity_links` rather than filtering the default result. When `--link` is given the positional is ignored.

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--link <link-id>` | Switch to the external-link trail form for an `external_links.id`. | required for this form |
| `--json` | Emit one JSON object instead of human text. | off |

**Output (human):**
```
audit trail for link 7  (task:42 ↔ acme-jira:PROJ-1234)

sessions:
  2026-05-10T09:00Z  claude  session:101  "Implement payment gateway API"
  2026-05-09T14:00Z  claude  session:98   "Design data model"

decisions:
  2026-05-09T14:22Z  "Use Stripe as payment processor"  [accepted]

sync events:
  2026-05-10T09:15Z  push  ok  title, status
  2026-05-09T18:00Z  pull  ok  status

commits:
  2026-05-10T09:12:44Z  5f4dcc3b5aa765d61d8327deb882cf99  Implement payment gateway API
```

**Output (`--json`):**
```json
{
  "link_id":7,
  "entity_kind":"task","entity_id":42,
  "external_id":"PROJ-1234","system_slug":"acme-jira",
  "sessions":[{"id":101,"vendor":"claude","started_at":"...","summary":"..."},...],
  "decisions":[{"id":5,"title":"...","status":"accepted","decided_at":"..."},...],
  "commits":[{"session_id":101,"claim_id":44,"sha":"...","repo_root":"/repo","branch":"main","subject":"...","author":"...","committed_at":"...","recorded_at":"..."}],
  "sync_events":[{"id":15,"direction":"push","outcome":"ok","at":"..."},...]
}
```

`commits` is omitted from the JSON object when the trail has none.

**Schema effects:** Reads `external_links`, `sync_events`, `sessions`, `session_entries`, `decisions`, `entity_links`, `session_commits`.

**Capture:** None.

**Exit codes:**
- `1` — link id not found.

---

### `planar audit session <session-id>`

**Synopsis:**
```
planar audit session <session-id> [--json]
```

**Description:** Show the full timeline of a session: all `session_entries` in ordinal order, with the task context and vendor identity.

**Output (human):**
```
session 101  vendor: claude  task: 42  2026-05-10T09:00Z → 2026-05-10T11:30Z

  1  [action]       Opened task: Implement payment gateway API
  2  [file]         docs/billing-spec.md  (read for context)
  3  [command]      go test ./...   outcome: 3 passed, 0 failed
  4  [decision]     Use Stripe as payment processor
  5  [observation]  Stripe rate limit is 100 req/s per endpoint
```

**Schema effects:** Reads `sessions`, `session_entries`, `tasks`.

**Capture:** None.

**Exit codes:**
- `1` — session not found.

---

### `planar audit commits`

**Synopsis:**
```
planar audit commits [--session <session-id>] [--task <task-id>] [--json | --shas]
```

**Description:** List commits attributed to sessions and claims. Filters are optional and composable: `--session` narrows to one session, `--task` traverses claim ownership to show commits recorded for claims on that task, and no filter lists every recorded commit newest-first. Human output is a fixed table; `--shas` emits bare SHAs for piping.

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--session <session-id>` | Restrict results to one session. | all sessions |
| `--task <task-id>` | Restrict results to commits recorded through claims on one task. | all tasks |
| `--json` | Emit the canonical row objects. Mutually exclusive with `--shas`. | off |
| `--shas` | Emit exactly one SHA per line, no header or decoration. Mutually exclusive with `--json`. | off |

**Output (human):**
```
SHA                                       session  claim  committed_at               subject
5f4dcc3b5aa765d61d8327deb882cf99              101     44  2026-05-10T09:12:44Z      Implement payment gateway API
```

**Output (`--json`):** One JSON array:
```json
[{"session_id":101,"claim_id":44,"sha":"...","repo_root":"/repo","branch":"main","subject":"...","author":"...","committed_at":"...","recorded_at":"..."}]
```

**Output (`--shas`):**
```
5f4dcc3b5aa765d61d8327deb882cf99
```

**Schema effects:** Reads `session_commits`; validates `--session` against `sessions(id)` and `--task` against `tasks(id)` before querying.

**Capture:** None.

**Exit codes:**
- `1` — session id or task id not found; or `--json` and `--shas` were combined.

---

### `planar audit publish-decision <decision-id> [--scope <slug>] [--json]`

**Description:** Post a decision's body to every operational-plane target reachable from that decision's external links — the bridge that pushes a recorded local decision out to the linked Jira issue / GitHub issue as a comment. Subject to the cross-scope guard (the decision's scope must agree with the operator's, or pass `--scope`).

**Schema effects:** Reads `decisions`, `entity_links`, `external_links`, and `external_systems`; updates each attempted link's last-sync state and inserts a `sync_events` row. Performs outbound HTTP to each linked system.

---

### `planar audit handoff-readiness [--threshold <n>] [--json]`

**Description:** Check resume-readiness across all in-flight tasks at once — the fleet-wide companion to `resume validate`. Reports which tasks would resume cleanly and which are missing a next action or capture context. `--threshold` tunes the staleness cutoff.

---

## Domain: `health`

Health commands report the operational status of the Planar installation, the
handoff readiness of in-flight tasks, and installed projection freshness.

---

### `planar health`

**Synopsis:**
```
planar health [--json]
```

**Description:** Report database, handoff-readiness, and installed-projection
health. In addition to the database and handoff checks, health consumes the
same read-only manifest-owned projection classification (presence/validity
plus byte/symlink freshness); it never repairs or rewrites an installed
projection.

**Output (human):**
```
db:               ok (~/.planar/planar.db)
schema:           v28 of v28 (current)
integrity:        ok
in-flight tasks:  5 (3 resumable, 2 NOT resumable)
pending handoffs: 1 (0 stale > 24h)
projection freshness: fresh (1 managed: 1 fresh, 0 stale, 0 missing; 1 unmanaged; 2 unselected vendors)
projection manifest:  current
overall:          degraded
```

**Output (`--json`):**
```json
{
  "db_path":"~/.planar/planar.db",
  "db_ok":true,
  "schema_version":28,
  "schema_target":28,
  "schema_current":true,
  "migration_count":28,
  "integrity_ok":true,
  "inflight_tasks":5,
  "resumable_tasks":3,
  "not_resumable_tasks":2,
  "pending_handoffs":1,
  "stale_handoffs":0,
  "projection_freshness": {
    "state":"fresh",
    "manifest_status":"current",
    "managed":1,
    "fresh":1,
    "stale":0,
    "missing":0,
    "unmanaged":1,
    "unselected_vendors":2,
    "evidence":null,
    "repair_command":null
  },
  "overall":"degraded"
}
```

The nested field order is stable. `state` is `fresh`, `degraded`, or
`not_installed`; `evidence` and `repair_command` are present as `null` when
unused. Text emits optional `projection evidence` and `projection repair`
lines, in that order, between the manifest and overall lines.

Stale or missing manifest-owned rows degrade health. A legacy, invalid, or
unsupported manifest also degrades once and reports the classifier's exact
reinstall command. An absent manifest without the legacy ownership stamp is
`not_installed` and stays healthy. Unmanaged extensions and vendors omitted by
the manifest are counted but never degrade health.

**Schema effects:** Reads `schema_migrations`, `tasks`, `context_snapshots`,
`handoffs`, the install manifest, and installed projection paths. Writes
nothing.

**Capture:** None.

**Exit codes:**
- `0` — all checks pass.
- `1` — degraded (some tasks are not resumable, handoffs are stale, managed
  projections are stale/missing, the install manifest needs recovery, SQLite
  integrity fails, or the health check itself cannot complete).

### `planar health hygiene`

```text
planar health hygiene [--scope <association>] [--stale-doing <days>]
                      [--stale-open <days>] [--json]
```

Reports lifecycle drift without changing any entity. Draft plans are reported
when they have no tasks (suggesting `planar plan update <id> --status
abandoned`) or when every attached task is `done` or `cancelled` (suggesting
`--status done`). Tasks in `doing` are reported after 7 days by default, and
open questions after 30 days. `--stale-doing` and `--stale-open` override those
non-negative day thresholds. `--scope` limits all three sections to one
association; without it, the reporter scans all scopes.

Text output contains one actionable section per signal class. JSON output has
this stable shape:

```json
{
  "thresholds": { "stale_doing_days": 7, "stale_open_days": 30 },
  "stale_draft_plans": [{
    "id": 110,
    "parent_plan_id": 85,
    "title": "M1 — Schema and core store",
    "reason": "all_tasks_terminal",
    "task_counts": { "todo": 0, "doing": 0, "blocked": 0, "done": 4, "cancelled": 0 },
    "suggestion": "planar plan update 110 --status done"
  }],
  "stale_doing_tasks": [
    {
      "id": 384,
      "plan_id": 76,
      "scope": "assoc:org:acme",
      "title": "Implement parser",
      "age_days": 14,
      "suggestion": "planar task update 384 --scope assoc:org:acme --status done OR planar task update 384 --scope assoc:org:acme --status blocked"
    }
  ],
  "stale_open_questions": [
    {
      "id": 12,
      "title": "Regression backfill?",
      "age_days": 45,
      "suggestion": "planar question answer 12 --answer \"<resolution>\" OR planar question wontfix 12"
    }
  ]
}
```

Findings do not affect process status: a successfully produced hygiene report
always exits `0`. Invalid flags, thresholds, scopes, or database failures remain
ordinary command errors.

---

### `planar dashboard`

**Synopsis:**
```
planar dashboard [--scope <slug>] [--agents] [--json]
```

**Description:** Operator situational-awareness view. Without `--agents`, a lightweight roll-up of in-flight plans (status in `draft` / `active` / `paused`). With `--agents`, folds in live claim state from `agent_work_claims` plus a per-plan "next available work" tally — the operator's read surface on agent coordination state.

The `--agents` fold-in is the planar binary's view of agent activity. The `planar agent` subcommand namespace does not exist by design — agent observability is split between this fold-in, the `planar-watch` viewer binary (M8), and the `planar-agent` ritual binary (M2).

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--scope <slug>` | Limit to a single scope slug. | resolved scope |
| `--agents` | Fold in live claim state and `next_available_by_plan`. | off |
| `--json` | Emit JSON instead of text. | off |

**JSON shape (without `--agents`):**

```json
{ "active_plans": [Plan, ...] }
```

**JSON shape (with `--agents`):**

```json
{
  "active_plans": [Plan, ...],
  "claims": {
    "active": [ClaimRow, ...],
    "stale":  [ClaimRow, ...]
  },
  "next_available_by_plan": {
    "12": [Task, ...],
    "13": [Task, ...]
  }
}
```

`ClaimRow` includes the locality columns (`repo_root`, `branch`, `head_sha_at_claim`, `dirty_at_claim`) and the worktree columns (`worktree_id`, `worktree_path`). The text-mode renderer surfaces `branch:<branch>  sha:<8-char>  dirty:<state>  repo:<repo_root>` for every active claim so an operator can spot mismatched checkouts at a glance.

**M3 addition:** each `ClaimRow` in the `--agents` fold-in carries a `latest_action` field matching the shape emitted by `planar-watch ps --json` — see [M3 addition — `ClaimRow.latest_action`](#binary-planar-watch) above. No new CLI flags on `planar dashboard`.

The `stale` bucket aggregates both `status='stale'` rows and `status='active'` rows whose lease has expired without a reconcile pass.

**Schema effects:** Reads `plans`, `agent_work_claims`, `tasks`.

**Exit codes:**
- `0` — always (an empty dashboard is not an error).

---

## Domain: `links`

Internal cross-cutting entity relationships. The `entity_links` table stores typed relationships between any two Planar entities (e.g. a task cites an artifact, a plan depends on another plan). This domain is distinct from the top-level `link` / `unlink` commands, which operate on `external_links` (operational plane bindings to Jira, GitHub Issues, etc.).

| Table | Purpose |
|-------|---------|
| `entity_links` | Internal cross-cutting relationships between Planar entities. |
| `external_links` | Bindings between local entities and external-system tickets. |

---

### `planar links add <from-kind:from-id> <to-kind:to-id>`

**Synopsis:**
```
planar links add <from-kind:from-id> <to-kind:to-id> --relationship <rel>
```

**Description:** Create an `entity_links` row between two Planar entities. Both arguments are typed entity references of the form `<kind>:<id>` (e.g. `artifact:30`, `task:42`). The referenced entities must exist in the database. The `(from_kind, from_id, to_kind, to_id, relationship)` tuple must be unique — re-creating an identical link is an error.

**Arguments:**

| Argument | Description |
|----------|-------------|
| `<from-kind:from-id>` | Source entity reference, e.g. `task:42`, `artifact:3`. |
| `<to-kind:to-id>` | Target entity reference, e.g. `plan:7`, `question:2`. |

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--relationship <rel>` | Relationship type. Accepted values: `derives-from`, `depends-on`, `addresses`, `verifies`, `cites`, `supersedes`, `touches`. | Required. |

**Valid kinds:** `plan`, `plan_step`, `task`, `question`, `test_scenario`, `artifact`, `decision`, `session`, `repo`.

**Output (human):**
```
created entity_link: task:42 --[cites]--> artifact:3  (link id: 22)
```

**Output (`--json`):**
```json
{"ok":true,"id":22,"from_kind":"task","from_id":42,"to_kind":"artifact","to_id":3,"relationship":"cites"}
```

**Schema effects:** Inserts into `entity_links(from_kind, from_id, to_kind, to_id, relationship)`. Appends a `session_entries` row with `prefix='action'`.

**Capture:** Appends `session_entries` row with `prefix='action'`.

**Exit codes:**
- `0` — link created.
- `1` — invalid `<kind:id>` format, unknown kind, unknown relationship, entity not found, or duplicate link.

---

### `planar links list <kind:id>`

**Synopsis:**
```
planar links list <kind:id>
```

**Description:** List all `entity_links` rows where the given entity appears as either the source (`from_kind`/`from_id`) or target (`to_kind`/`to_id`).

**Arguments:**

| Argument | Description |
|----------|-------------|
| `<kind:id>` | Typed entity reference, e.g. `task:42`, `artifact:3`, `plan:7`. |

**Output (human):**
```
id   direction  relationship    peer
22   from       cites           artifact:3 "Billing Tech Spec"
31   to         depends-on      task:38 "Implement retry logic"
```

**Output (`--json`):** One object per link:
```json
{"id":22,"from_kind":"task","from_id":42,"to_kind":"artifact","to_id":3,"relationship":"cites"}
{"id":31,"from_kind":"task","from_id":38,"to_kind":"task","to_id":42,"relationship":"depends-on"}
```

**Schema effects:** Reads `entity_links`.

**Capture:** None (read-only).

**Exit codes:**
- `1` — entity kind is not a valid `entity_links` kind.

---

### `planar links remove <link-id>`

**Synopsis:**
```
planar links remove <link-id>
```

**Description:** Delete an `entity_links` row by its id. Use this to remove internal cross-cutting relationships previously created by `task link`, `plan link`, `artifact link`, `question link`, etc.

**Arguments:**

| Argument | Description |
|----------|-------------|
| `<link-id>` | The `entity_links.id` to delete. |

**Output (human):**
```
entity link 22 removed
```

**Output (`--json`):**
```json
{"ok":true,"id":22}
```

**Schema effects:** Deletes from `entity_links(id)`.

**Capture:** Appends `session_entries` row with `prefix='action'`.

**Exit codes:**
- `1` — link id not found.

---

### `planar links trail <link-id> [--json]`

**Description:** Show the audit trail for an `entity_links` row — the sequence of `sync_events` and related activity recorded against that link over its lifetime. The read-side companion to the link-mutation verbs.

---

### `planar links update <link-id>`

> **This verb does not exist.** Measured at task 6075 (2026-09-11):
> `planar schema` declares `links add`, `links list`, `links remove` and
> `links trail` — there is no `links update`. Invoking it fails at PARSE TIME
> with exit 2:
>
> ```
> error: links: The following arguments were not expected: update 7 --sync write-back
> ```
>
> It does not print `links update is deferred to M11`; no such message exists
> in the binary. An earlier edition of this page claimed that deferral, and
> also claimed a cross-scope guard for a verb that cannot be invoked.
>
> `links remove` / `links add` manage internal `entity_links` and cannot
> change an external link. **No lossless external-link update exists in the
> CLI today.** Everything below is a design sketch for a verb that has never
> shipped — not a contract. The unlink-and-recreate sequence documented above
> is the actual workaround.

The only current recovery is destructive top-level `unlink` / `link`, or
`unlink` followed by a fresh `ext propagate`. Before proceeding, capture the
CLI-visible evidence:

```sh
planar audit trail --link <link-id> --json > external-link-<link-id>-audit.json
planar-ext sync status --entity <kind:id> --system <system-slug> --json \
  > external-link-<link-id>-status.json
```

The audit output captures the local entity, system slug, external id, and
sync-event history; status captures the last-sync timestamp and status. Neither
command exposes `external_url`, `config_json`, `link_role`, or
`sync_direction`. If the intended role and direction are not known from an
independent record, stop: the CLI cannot reconstruct them exactly.

For a record-only binding where retaining the same remote external id matters,
write down and review the complete replacement command before deleting:

```sh
planar unlink <link-id>
planar link <kind:id> --to <system-slug>:<external-id> \
  --role <role> --sync <read-only|write-back|two-way>
```

This preserves only the identifiers and explicitly re-entered role/direction.
The replacement has `external_url=null`, `config_json=null`, no prior
last-sync state, and no attached prior event history. The old event rows remain
detached with `link_id=null`; the replacement link cannot query them.

For a propagation-owned mirror, first verify that the plan and system resolve
while the old row still exists. After capturing evidence and unlinking, preview
the now-unlinked entity before allowing a new remote counterpart:

```sh
planar-ext ext propagate <plan-id> --system <system-slug> --dry-run \
  --sync <read-only|write-back|two-way>
planar unlink <link-id>
planar-ext ext propagate <plan-id> --system <system-slug> --dry-run \
  --sync <read-only|write-back|two-way>
planar-ext ext propagate <plan-id> --system <system-slug> \
  --sync <read-only|write-back|two-way>
```

The first dry run is a pre-delete resolution check; it normally reports the
existing row as skipped. The second previews fresh creation after unlink. The
final command creates a new remote counterpart, URL, config, sync state, and
history; it does not restore the old values. Strategy is selected from
current state and may differ from the deleted `config_json`; for GitHub
systems the strategy is re-detected from the current repo count (or forced
with `--github-strategy`).

The design sketch for a `links update --sync <direction>` verb that earlier
editions of this page carried has been removed; nothing of that shape exists
in the binary.

---

## Domain: `help`

### `planar help`

**Synopsis:**
```
planar help
```

**Description:** Print the root help page and exit 0. The output is byte-identical to `planar --help`: the same
`CLI::App::help()` rendering of the root node, so the two cannot drift apart. The root page leads with the
binary's write surface (the planning entities and manual `tasks.status` transitions `planar` writes, and the
tables `planar-agent` and `planar-ext` write) and then lists every top-level command.

`planar help` takes no arguments. Per-node help stays on the flag: `planar <subcommand> [<sub-subcommand>] --help`
for a group or leaf. Trailing words are not interpreted: `planar help task` prints the root page, not `task`'s.
`help` is a leaf in the `planar schema`
catalog. The text is rendered by Planar's own help renderer in `src/lib/cliapp/` (decision 948: CLI11 handles
tokenization and value coercion only, because the help text is a pinned surface).

**Output:** Plain text to stdout, exit 0. Not affected by `--json`.

**Exit codes:**
- `2` — the node the flag was attached to does not exist (`planar plan nosuch --help` prints `plan`'s help and exits 0, because CLI11 stops at the last valid node; `planar plan nosuch` without `--help` exits 2).

---


## Domain: `spec`

Planning pipeline spec commands for decomposing workbench planning documents into a task graph.

---

### `planar spec ingest <plan>`

**Synopsis:**
```
planar spec ingest <plan> [--apply] [--apply-removals] [--format text|json] [--json] [--scope <scope>] [--strict]
```

**Description:** Read `tech-spec.md`, `roadmap.md`, and (when present) `test-spec.md` from the anchor plan's workbench directory, compute the proposed diff against the current database state, and (optionally) commit additions and updates.

Default mode is **preview**: prints a tree-shaped diff and exits 0 without applying graph changes. `--apply` is required to commit changes.

Apply mode is atomic per anchor plan. All derived rows for one anchor plan run inside one SQLite savepoint: child plans, tasks, decisions, test scenarios, links, optional removals, the anchor `draft` -> `active` flip, and the successful action audit either all commit or all roll back. A failed apply leaves no partial derived graph for that anchor and should not require workbench cleanup. When multiple `<plan>` arguments are supplied, each anchor has its own atomic boundary; one plan may apply successfully while another rolls back, and the command exits non-zero if any plan fails.

A `coverage:` line follows the totals on every run. It reports how many tasks carry a `[slug:]`, how many slug-bearing tasks are verified by at least one test-spec scenario, and any orphan scenarios whose `**Verifies:**` line failed to parse. Slug collisions are reported separately. `--strict` promotes uncovered tasks, orphan scenarios, and slug collisions into a non-zero exit.

`<plan>` may be a numeric plan id or a plan slug.

**Scope guard:** Preview is read-only, so a numeric plan id may locate and inspect an anchor outside the cwd-derived scope. `--apply` refuses before writing when the operator's resolved write scope disagrees with the anchor plan's stored scope; pass the anchor's scope explicitly with `--scope` (or run from its owning cwd) to apply. Derived rows always retain the anchor's stored scope. See [Cross-scope guard](#cross-scope-guard).

**Arguments:**

| Argument | Description |
|----------|-------------|
| `<plan>` | Anchor plan id (integer) or plan slug. Required. |

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--apply` | Commit additions and updates to the database. | off |
| `--apply-removals` | Also commit proposed removals (cancel orphan tasks, abandon orphan plans). Must be combined with `--apply`. | off |
| `--format text\|json` | Output format. `text` prints a tree-shaped diff; `json` emits a machine-readable JSON object. The JSON object carries a `coverage` field with the same data the text mode prints. | `text` |
| `--json` | Shorthand for `--format json`. | off |
| `--scope <scope>` | Select operator write scope for `--apply`. Required when cwd does not resolve to a scope that covers the anchor plan. | cwd-derived |
| `--strict` | Reject the ingest (exit 1) when any slug-bearing task has no verifying scenario, any scenario has no parseable `**Verifies:**` line, or any proposed task slug collides with a live task. | off |

**Output (human, `--format text`):**

```
<assoc-slug>/<plan-slug>/
  + plan       <milestone-name>          (N tasks)
  +   task     <task-title>
  +   task     <task-title>              touches=acme/protos
  ~ task       <existing-task-title>     body changed
  - task       <orphan-task-title>       not in current roadmap  [needs --apply-removals]
  + decision   <decision-title>

N additions, M updates, P proposed removals.
Run with --apply to commit; add --apply-removals to cancel proposed removals.
```

**Output (`--format json`):**
```json
{
  "anchor_plan_id": 42,
  "assoc_slug": "project:checkout-platform",
  "anchor_slug": "add-checkout-rpc",
  "entities": [
    {"op": "add", "kind": "plan", "title": "Protos Changes", "derives_from": "plan:42"},
    {"op": "add", "kind": "task", "title": "Define CheckoutRequest proto", "derives_from": "plan:Protos Changes", "touches": ["acme/protos"]},
    {"op": "add", "kind": "decision", "title": "Use Protocol Buffers", "derives_from": "plan:42"}
  ],
  "summary": {"additions": 11, "updates": 0, "removals": 0},
  "coverage": {
    "total_tasks": 5,
    "tasks_with_slug": 5,
    "tasks_without_slug": 0,
    "uncovered_task_slugs": [],
    "orphan_scenarios": []
  },
  "slug_collisions": []
}
```

For pre-ingest review, this JSON is authoritative even when no live task or
scenario rows exist. A non-empty `uncovered_task_slugs`, `orphan_scenarios`, or
`slug_collisions` array is an ingest-readiness failure.

**Schema effects:**

Reads:
- `plans` — anchor plan and existing child plans.
- `tasks` — existing tasks linked via `entity_links(relationship='derives-from')`.
- `decisions` — existing decisions linked via `entity_links(relationship='derives-from')`.
- `questions` — existing anchor-linked questions for title-based reconciliation.
- `test_scenarios` — existing scenarios linked to the anchor for title-based reconciliation.
- `entity_links` — existing link rows for reconciliation.

Writes (only with `--apply`, atomically per anchor plan):
- `plans` — inserts child plans; updates anchor plan status (`draft` → `active` on first apply).
- `tasks` — inserts or updates tasks; cancels orphan tasks (only with `--apply-removals`).
- `decisions` — inserts or updates decisions.
- `questions` — inserts H3 items under tech-spec `## Open Questions`; when the first non-blank body line begins with the case-sensitive `Resolution:` token, answers the new or existing question during the same apply.
- `test_scenarios` — inserts or updates scenarios parsed from `test-spec.md`; also auto-drafts `Verify: <task title>` scenarios for non-trivial newly added tasks (task body contains at least two bullet lines).
- `entity_links` — inserts `derives-from` links (plan→anchor, task→plan, decision→anchor, scenario→anchor), `touches` links from roadmap `[touches: ...]` annotations (task→repo), plus `task_touch_paths` rows for entries in `<repo-slug>:<path>` or bare-`<path>` form, and `verifies` links (scenario→task).

**Capture:**
- Preview mode: one `session_entries` row with `prefix='read'` appended.
- Apply mode: one `session_entries` row with `prefix='action'` appended inside the same savepoint as the derived graph writes, so failed applies do not emit a success audit row.

**Exit codes:**
- `0` — success (preview or apply).
- `1` — user error: plan not found, `--apply-removals` without `--apply`, malformed workbench files, `--strict` refused due to coverage gap.
- `2` — system error: database failure, workbench filesystem I/O failure.

---

## Domain: `test-spec`

Read-only inspectors for post-ingest test-spec coverage. Before apply, the
authoritative draft check is `spec ingest <plan> --strict --json`, whose
workbench-derived `coverage` object exists even when there are no live task or
scenario rows. After apply, `test-spec status` is the human-facing live-row
oracle used by test-coder and reviewer cycles.

### `planar test-spec status <plan>`

**Synopsis:**
```
planar test-spec status <plan> [--json]
```

**Description:** Print per-milestone test-spec coverage for an anchor plan. Each row reports the milestone's child plan, total tasks, slug-bearing tasks, covered tasks, and a four-bucket breakdown (happy / empty / error / edge) classified by scenario-title prefix. A summary line totals across milestones. A numeric plan id is an unambiguous read locator and may resolve an anchor outside the cwd-derived scope.

This command is read-only. It does not consult the workbench filesystem — it queries `tasks`, `test_scenarios`, and `entity_links` directly.

**Arguments:**

| Argument | Description |
|----------|-------------|
| `<plan>` | Anchor plan id (integer) or plan slug. Required. |

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--json` | Emit line-oriented JSON: one object per milestone, followed by a final summary object. Streamable. | off |

**Output (human, default):**

```
test-spec status for plan 42 (billing-export-csv)
  milestone                        tasks  slug   cov   happy empty error edge  other
  Phase 1 — Schema                     3     3     2       1     1     0    0      0
  Phase 2 — Implementation             2     2     2       2     0     0    0      0

  4 scenarios total; 4 of 5 slug-bearing tasks covered (5 total tasks).
```

**Exit codes:**
- `0` — always; this verb never refuses.
- `1` — plan not found.
- `2` — database failure.

---

## Domain: `import`

Import an existing repository's planning artefacts (tech specs, roadmaps, ADRs, backlog files, GitHub issues) into Planar's data model. A translator, not a generator: imports what is already there rather than drafting new documents from a goal statement.

**Sibling verb.** `synthesize` is the synthesis counterpart to import's transcription. Use `import` for clean structured docs; use `synthesize` for messy / docs-only / mid-evolution repos. See [Domain: synthesize](#domain-synthesize) for the full surface, and [Transcription vs Synthesis](./concepts.md#transcription-vs-synthesis) for the conceptual split.

See also: `spec ingest` (decompose workbench planning docs), `ext propagate` (push the imported tree to an external system).

---

### `planar import <repo-root>`

**Synopsis:**
```
planar import <repo-root> [--from-github] [--dry-run] [--strict]
                            [--roadmap <path>]
                            [--apply] [--apply-removals] [--scope <slug>]
                            [--no-status-inference] [--interpret]
                            [--accept-spec <slugs>] [--no-forward-specs] [--json]
```

**Description:** Walk the repository at `<repo-root>`, parse planning artefacts from the filesystem and git history, infer task completion status, build an ImportPlan, and (optionally) commit it to the database.

Default mode is **preview**: prints a tree-shaped diff and exits 0 without writing anything. `--apply` is required to commit.

`--dry-run` emits the ImportPlan as versioned JSON (schema version 1.0) instead of the human tree and never writes, regardless of `--apply`.

Idempotency: items already present in the database (matched by title and source-path fingerprint) are reported as **Skipped** and never double-imported. Safe to re-run after new commits.

**Arguments:**

| Argument | Description |
|----------|-------------|
| `<repo-root>` | Path to the repository root to import. Required. Must be a directory. |

**Options:**

| Flag | Default | Description |
|------|---------|-------------|
| `--from-github` | off | Also import open GitHub issues as tasks. Requires the `gh` CLI on PATH and a GitHub remote at the `origin`. If `gh` is not found, a warning is emitted and the flag is silently skipped. |
| `--dry-run` | off | Emit JSON report instead of human preview. Never writes, regardless of `--apply`. |
| `--strict` | off | Refuse to import ambiguous items (below the confidence threshold). Default (lenient) mode imports them as `status='todo'` with a warning annotation. |
| `--roadmap <path>` | auto | Explicit path to the roadmap file. Overrides auto-discovery. Useful when the roadmap has a non-standard name or location. |
| `--apply` | off | Commit the import to the database. Without this flag, preview only. |
| `--apply-removals` | off | When applying, also remove entities that are gone from the source. |
| `--scope <slug>` | cwd-derived | Scope for all created entities. Overrides cwd derivation for this invocation. |
| `--no-status-inference` | off | Default every imported task to `status=todo`, `signal=no-inference`, `confidence=0`. Skips layers 2-3 (branch + git-log correlation) of status inference; the checkbox layer still runs because checkbox state is operator-explicit. Use for greenfield, docs-only, or fresh-fork repos where status correlation is unreliable by construction. |
| `--interpret` | off | Run the optional LLM interpretation pass after the deterministic classifier (see the `planar-importer` agent). |
| `--accept-spec <slugs>` | none | Non-interactive forward-spec selection — slug, comma-separated slugs, or `all`. |
| `--no-forward-specs` | off | Skip forward-spec processing entirely. |

**No >25% auto-done refusal.** This binary does not refuse an import in
which a large share of inferred tasks would land as `status=done`, and
neither `--trust-status-inference`, `--threshold` nor `--no-interpret`
exists on it; passing any of them fails at parse time with exit 2. For an
unreliable correlation, reach for
`--no-status-inference` (which IS implemented) to default every task to
`status=todo`, and `task reopen <id>` to recover any individually
wrongly-marked tasks from an earlier import.

**Output (human, preview mode):**

```
<repo-root>/
  + plan       Phase 1 — Foundation         (4 tasks)
  +   task     Add migrations               [done — git-log match]
  +   task     Wire CLI parser              [todo]
  + plan       Phase 2 — Adapters           (2 tasks)
  +   task     Jira adapter                 [todo]
  +   task     GitHub adapter               [todo]
  + artifact   docs/tech-spec.md            [kind=tech_spec]
  + decision   Vendor SQLite amalgamation   [from: docs/adrs.md#adr-0009]
  ~ task       Add workbench archive        [skipped — already imported]

7 additions, 1 skipped.
Run with --apply to commit.
```

**Output (`--dry-run`, JSON schema version 1.0):**

```json
{
  "schema_version": "1.0",
  "anchor": {"title": "<repo-name>", "slug": "<slug>"},
  "child_plans": [
    {"title": "Phase 1 — Foundation", "slug": "phase-1-foundation"}
  ],
  "tasks": [
    {"title": "Add migrations", "status": "done", "inferred_from": "git-log", "parent_plan": "phase-1-foundation"}
  ],
  "artifacts": [
    {"title": "Tech Spec", "source_path": "docs/tech-spec.md", "kind": "tech_spec"}
  ],
  "decisions": [
    {"title": "Vendor SQLite amalgamation", "source_path": "docs/adrs.md"}
  ],
  "ambiguous": [],
  "warnings": []
}
```

**Output (`--apply`):**

```
applied: 3 plans, 12 tasks, 4 artifacts, 2 decisions, 1 skipped
anchor plan id: 87
github links created: 5
```

The `github links created` line appears only when `--from-github` is set and issues were linked.

**Schema effects:**

Reads:
- `associations`, `project_associations`, `projects` — cwd-derived scope resolution.
- `plans`, `tasks`, `artifacts`, `decisions` — fingerprint queries to detect already-imported items.
- Git log and branch list via shell (`git log`, `git branch --merged`).
- GitHub Issues via `gh issue list` (only when `--from-github`).

Writes (only with `--apply`):
- `plans` — inserts the anchor plan and child plans (one per milestone).
- `tasks` — inserts tasks with inferred status.
- `artifacts` — inserts artifact rows for discovered planning documents.
- `decisions` — inserts decision rows for ADRs and `## Decisions` headings.
- `entity_links` — inserts `derives-from` links (child plan→anchor, task→plan, decision→anchor).
- `external_links` — inserts `link_role='mirror'` rows for GitHub issues (only when `--from-github`).

**Exit codes:**
- `0` — success (preview, dry-run, or apply).
- `1` — user error: `<repo-root>` not found or not a directory; `--strict` mode rejected ambiguous items.
- `2` — system error: database failure, git shell failure, filesystem I/O failure.

**Related:**
- `planar spec ingest <plan>` — decompose workbench planning documents into a task graph for a feature already in Planar.
- `planar-ext ext propagate <plan>` — propagate the imported tree to Jira or GitHub Issues.
- `planar import <repo-root> --dry-run` — emit the ImportPlan as JSON without writing.
- `planar synthesize <repo-root>` — sibling verb. Synthesizes fresh planning material from docs + code via an LLM pass instead of transcribing.

---

## Domain: `synthesize`

`planar synthesize <repo-root>` — synthesize fresh planning artifacts for a repo from existing docs + git log + source code via an LLM pass. Sibling of `import` (which transcribes existing docs verbatim).

Use `synthesize` when:
- Repo docs are scattered, mid-evolution, or contradicted by reality.
- The repo is docs-only (no source code yet — greenfield).
- You inherited a repo and want the LLM to assess what's actually there.
- Multiple roadmaps of different eras exist.

Use `import` instead when docs are clean and structured. See [Transcription vs Synthesis](./concepts.md#transcription-vs-synthesis).

---

### `planar synthesize <repo-root>`

**Synopsis:**
```
planar synthesize <repo-root> [--apply] [--apply-removals] [--scope <slug>]
                                 [--code-layout <name>]
                                 [--treat-as-greenfield] [--treat-as-nongreenfield]
                                 [--accept-spec <slug>] [--no-forward-specs]
                                 [--literal] [--dry-run] [--json]
```

**Description:** Walk the repo at `<repo-root>`, run the deterministic floor (artifact discovery + `codeprobe.Probe` over the source tree), hand a fingerprinted `synthesis.Request` to the vendor skill via the on-disk cache, then on a cache-hit re-invocation validate and merge the LLM `synthesis.Result` against the deterministic baseline.

Default mode is **preview**: prints a tree-shaped diff and exits 0 without writing. `--apply` is required to commit additions and updates; `--apply --apply-removals` additionally soft-cancels removed entities.

`--dry-run` narrows that further: there are no planning writes on the non-apply path to suppress, but a first run does stage `_pending.json` under `$PLANAR_HOME/cache/bootstrap-synthesis/<repo-slug>/`. `--dry-run` suppresses that one filesystem side effect and names the paths it would have used (task 6273, decision 1125).

The LLM never runs in the CLI. The `planar` binary writes a synthesis Request to `$PLANAR_HOME/cache/bootstrap-synthesis/<repo-slug>/_pending.json` and exits 0 with an "Awaiting LLM synthesis" notice. The vendor skill reads the Request, runs the LLM at temperature 0, and writes the Result to `<cache-dir>/<fingerprint>.json`. The operator re-runs `planar synthesize <repo-root>`; the CLI finds the cached Result, validates it, merges it with the deterministic baseline, and emits the preview.

**Arguments:**

| Argument | Description |
|----------|-------------|
| `<repo-root>` | Path to the repository root to synthesize. Required. Must be a directory. |

**Options:**

| Flag | Default | Description |
|------|---------|-------------|
| `--apply` | off | Commit additions and updates. |
| `--apply-removals` | off | Commit removals (soft-cancel). Requires `--apply`. |
| `--scope <slug>` | cwd-derived | Override scope resolution. |
| `--code-layout <name>` | auto | Override layout detection. One of `swift`, `go`, `node`, `python`, `mixed`. |
| `--treat-as-greenfield` | off | Force greenfield mode even when code is detected. Forces every task to land `status=todo`. |
| `--treat-as-nongreenfield` | off | Bypass greenfield auto-detection. Use for non-conventional layouts where codeprobe under-detects. |
| `--accept-spec <slug>` | interactive | Non-interactive forward-spec selection; `all` accepts every proposed forward spec. |
| `--no-forward-specs` | off | Skip the forward-spec phase entirely. |
| `--literal` | off | Delegate to `import` (transcription). Useful when you started with `synthesize` but realize the repo is clean enough for transcription. |
| `--dry-run` | off | Stage nothing: skip the `_pending.json` write (and the cache directory that holds it) and report the paths that would have been written. Exit 0. Before task 6273 this flag was declared and never read, so it silently did the normal thing. |
| `--json` | off | Machine-readable output. |

**Workflow:**

1. Operator runs `planar synthesize <repo-root>`.
2. The CLI runs deterministic artifact discovery, corpus parsing, and code-evidence probing.
3. The CLI writes the synthesis Request to `$PLANAR_HOME/cache/bootstrap-synthesis/<repo-slug>/_pending.json`.
4. The CLI exits 0 with the "Awaiting LLM synthesis" message.
5. Vendor skill reads the Request, runs the LLM at temperature 0, and writes the Result to `<cache-dir>/<fingerprint>.json`.
6. Operator re-runs `planar synthesize <repo-root>`.
7. The CLI reads and validates the cached Result, then merges it with the deterministic baseline.
8. Operator reviews the preview; `--apply` commits.

**Schema effects:**

Reads:
- `associations`, `project_associations`, `projects` — cwd-derived scope resolution.
- `plans`, `tasks`, `artifacts`, `decisions` — fingerprint queries to detect already-synthesized items.
- Source tree + git log (via `codeprobe`); no shell to `gh` (`synthesize` does not pull GitHub issues).

Writes (only with `--apply`):
- `plans` — inserts the anchor plan and per-phase child plans synthesized by the LLM.
- `tasks` — inserts tasks with status grounded in code-evidence citations.
- `artifacts` — inserts the synthesized `product_spec` / `tech_spec` / `roadmap` as primary artifacts. The existing planning docs are preserved on the same anchor plan as `kind=research` reference artifacts.
- `decisions` — inserts decisions extracted from tech specs and LLM-inferred decisions (citation required).
- `entity_links` — inserts `derives-from` links (child plan→anchor, task→plan, decision→anchor).

**Legacy source schemes.** Artifacts created by `synthesize` (and by `import` for forward-spec seeds) carry a stored `source` of `pl-synthesize://<kind>` or `pl-forward-spec://<kind>`. These are provenance identifiers kept for compatibility with existing databases; they name no skill and are not command references. The synthesize handler branches on the `pl-synthesize://` prefix when it decides which artifacts survive `--apply-removals`, so the literals must not be renamed. The generated artifact bodies and the operator messages name `planar synthesize` instead.

**Apply layer.** The Apply path is **shared with `import`** (the apply + diff helpers in `src/engine/importer/importer.cpp`). Both verbs converge on the same downstream pipeline.

**Exit codes:**
- `0` — success (preview, dry-run, apply, or cache-miss "awaiting synthesis").
- `1` — user error: `<repo-root>` not found, conflicting flags, cached Result fails `synthesis.Validate`, confidence-floor refusal.
- `2` — system error: database failure, filesystem I/O failure, cache I/O failure.

**Related:**
- [`agents/planar-synthesizer.md`](../agents/planar-synthesizer.md) — vendor-neutral role spec.
- [Domain: `import`](#domain-import) — sibling transcription verb.
- [Transcription vs Synthesis](./concepts.md#transcription-vs-synthesis) — conceptual split.

---

## Domain: `config`

Manages the Planar configuration file at `~/.planar/config.toml` (overrideable
via `$PLANAR_CONFIG_PATH`). The configuration plane is file-based; no new DB
tables or schema changes are involved. Settings are re-parsed on every CLI
invocation.

Resolution order (highest to lowest priority):

1. Environment variable (e.g. `$PLANAR_WORKBENCH_ROOT`, `$PLANAR_VENDOR`).
2. Per-association section (`[associations."<slug>"]` in the config file).
3. Top-level config file values.
4. Embedded defaults (compiled into the binary from the engine config module).

**Sensitive-data invariant:** keys whose names match `*_token`, `*_password`,
`*_secret`, `*_key`, or the bare names `token`, `password`, `secret` must not
carry literal values in the config file. Use the `*_env` convention to name the
environment variable instead.

**Model routing (plan 540, extended plan 899).** The config carries per-vendor
model **tier maps** (each tier a scalar or an ordered candidate list), a
vendor+tier **work-type routing map**, and a vendor-independent **role→tier**
map. The canonical tiers are `small`, `medium`, and `large`; the canonical
work types are `schema`, `engine`, `architectural`, `cli`, `feature`, and
`mechanical`:

```toml
[models.claude]
small  = "claude-haiku-4-5"
medium = "claude-sonnet-5-5"
large  = ["claude-opus-5-5", "claude-fable-5-1"]

[models.codex]
small  = "gpt-5.6-luna"
medium = "gpt-5.6-terra"
# A tier value may be an ordered candidate list instead of a scalar;
# list[0] is the tier default. Every other reader is scalar-compatible.
large  = ["gpt-5.6-sol", "gpt-5.5"]

[routing.codex.large]
# work-type → candidate id (must be a member of the tier's candidate list
# above). Unmapped work types fall back to the tier default (list[0]).
schema        = "gpt-5.6-sol"
architectural = "gpt-5.6-sol"
feature       = "gpt-5.5"

[roles]
coder      = "medium"   # coder resolves to the active vendor's `medium` model
reviewer   = "large"
test-coder = "medium"
sync-reconciler = "large"
```

Override any tier to re-route every role at that tier for that vendor, or any
role to move it to a different tier. A `[role_vendors]` section (role→vendor,
override-only) routes individual roles to a different vendor; unset roles use
`[defaults].vendor`. `planar config validate` rejects a `[routing.<vendor>.<tier>]`
entry naming a model id absent from that tier's candidate list — a stale or
typo'd routing target is a configuration error, not a silent fall-through.
`planar config show --effective` shows each resolved
`models.<vendor>.<tier>` / `routing.<vendor>.<tier>.<work-type>` /
`roles.<role>` / `role_vendors.<role>` key with its provenance. (There is
no `planar models routing` / `planar models candidates` / `planar models
list` — that discovery family was removed; `planar models resolve --role
<role>` answers what tier a role gets and whether the packet or the static
fallback produced it.) This is the **single authoritative routing source** — the
skill-render Tier Table (hand-maintained in `agents/models.md`), the
orchestrator's Phase 3 dispatch-preview routed-model column (`resolve(role,
work_type)`; see the `planar-orchestrator` agent's Dispatch preview and model
tiers), and other workflow callers all resolve through
it; there is no separate `execute-config.toml`.

#### The `[queue]` table

The host-wide build and test queue (`planar-agent queue run`) reads its
settings from an optional `[queue]` table. Every key is optional; the values
shown are the defaults.

```toml
[queue]
slots         = 1       # integer, 1..1024: commands that may run at once
poll_interval = "1s"    # duration, above zero: how often a submitter polls
stale_after   = "30s"   # duration, above zero, not below poll_interval
grace         = "10s"   # duration, zero or more: SIGTERM-to-SIGKILL grace
history_days  = 30      # integer, 1..36500: days history rows and logs are kept
```

A duration is an integer followed by a unit: `ms`, `s`, `m` or `h` (at most
`24h`). A bare integer is refused: the unit of a bare number is not guessed.
The queue reads the file at each poll, so an edit takes effect without
restarting a waiting submitter; lowering `slots` stops no running command, and
no new one starts until the running count is below the new value. The queue
reads only `config.toml`: it never opens `planar.db`, so it keeps working when
the main database is schema-locked. `planar config show --effective` reports
each `queue.*` key with its provenance.

`planar config validate` refuses a `[queue]` value outside those ranges, a key
of a wrong type, and an unknown `[queue]` key, and names the key on each
finding (`error: queue.stale_after: stale_after must not be negative, got
"-5s"`).

---

### `planar config show`

**Synopsis:**
```
planar config show [--effective] [--raw] [--defaults] [--scope <slug>] [--format human|json]
```

**Description:** Print the resolved configuration. By default, prints every
key with its resolved value (env + file + embedded defaults, merged). All output
is sorted for snapshot-friendliness.

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--effective` | Show each key with its provenance (`[env: VAR]`, `[config file]`, `[embedded default]`, `[per-association override]`). | off |
| `--raw` | Print the user's config file contents verbatim. Empty output if the file is absent. | off |
| `--defaults` | Print the embedded defaults file verbatim. Doubles as the "what keys exist" reference. | off |
| `--scope <slug>` | Resolve as if the named association slug were active. | (none) |
| `--format <fmt>` | Output format: `human` or `json`. | `human` |

**Output (`--effective`, human):**
```
defaults.scope = global                  [embedded default]
defaults.vendor = claude                 [config file]
external.jira.base_url = https://...     [config file]
external.jira.token_env = JIRA_TOKEN     [embedded default]
workbench.root = /tmp/wb                 [env: PLANAR_WORKBENCH_ROOT]
```

Sensitive-named keys (or keys resolved via a sensitive env var) display `***`
instead of their value:
```
external.jira.token_env = JIRA_TOKEN     [embedded default]
```
(The token value itself — read at runtime from `$JIRA_TOKEN` — is redacted.)

**Output (`--format json`, one object per key):**
```json
{"key":"defaults.vendor","value":"claude","provenance":"config file"}
```

**Schema effects:** None (read-only).

**Capture:** None (read-only).

**Exit codes:**
- `0` — success.
- `1` — config file present but unparseable.
- `2` — filesystem error (file unreadable).

---

### `planar config edit`

**Synopsis:**
```
planar config edit
```

**Description:** Open the config file in `$EDITOR` (falling back to `$VISUAL`,
then `vi`). If the file does not exist, a starter file is written first
(equivalent to `planar config init`).

**Schema effects:** None.

**Capture:** None.

**Exit codes:**
- `0` — editor exited cleanly.
- `2` — filesystem error or editor not found.

---

### `planar config validate`

**Synopsis:**
```
planar config validate
```

**Description:** Parse the resolved config file and check:

- TOML syntax.
- Sensitive-data invariant: keys matching the secret-name denylist must not carry literal values.
- Cross-reference consistency (e.g. `auth = "token-env"` requires `token_env` to name an env var).
- The `[queue]` table: every key within its range, no unknown key, and `stale_after` not shorter than `poll_interval` (see [The `[queue]` table](#the-queue-table)).
- Unknown keys outside `[queue]` produce a **warning**, not an error, preserving forward-compatibility; an unknown key inside `[queue]` is refused (see above).

Each issue is printed with its line number and key path.

The verb takes no positional: a path argument is refused at parse time (exit 2). Validate a different file by pointing `PLANAR_CONFIG_PATH` at it.

**Output (human):**
```
error: line 12: token: sensitive key must not carry a literal value
warning: line 5: defaults.future_setting: unknown key (will be ignored)
config validate: ok
```

**Schema effects:** None (read-only).

**Capture:** None.

**Exit codes:**
- `0` — no errors (warnings are printed but do not change the exit code).
- `1` — one or more errors found, or the config file was not found (`error: config file not found: <path>`).
- `2` — parse failure (unexpected argument or flag).

---

### `planar config init`

**Synopsis:**
```
planar config init
```

**Description:** Idempotently write a starter `config.toml` at the resolved
config path if it does not already exist. Prints whether the file was created
or already existed. Exit 0 either way.

Called automatically by `planar init` before applying migrations.

**Output (human):**
```
config init: created ~/.planar/config.toml
```
or:
```
config init: already exists ~/.planar/config.toml
```

**Schema effects:** None.

**Capture:** None.

**Exit codes:**
- `0` — success (created or already existed).
- `2` — filesystem error (directory not writable).

---

### `planar config path`

**Synopsis:**
```
planar config path
```

**Description:** Print the effective config file path. Respects
`$PLANAR_CONFIG_PATH`. Useful for scripting (`$EDITOR "$(planar config path)"`).

**Output:**
```
/Users/alice/.planar/config.toml
```

**Schema effects:** None.

**Capture:** None.

**Exit codes:**
- `0` — success.
- `2` — cannot resolve home directory.

---

## Domain: `templates`

Template-plane commands. Manage, inspect, validate, and render the
JSON templates used by the ext-sync agent to produce external-system payloads
(Jira issues, GitHub Issues). Templates are files only — no
database table. They resolve through a three-level fallback chain: user-chosen
set → baseline `default` set → embedded binary defaults.

Templates use `text/template` directives against a rendering `Context` that
exposes `.Task`, `.Plan`, `.Feature`, `.Scenario`, `.Touches`, `.Assoc`,
`.ExternalKey`, and `.Children`.

The `customfield_10004` (Epic Name) and `customfield_10014` (Epic Link) IDs in
the Jira templates are typical Jira defaults but vary per organization. Users
should override them in a custom template set if their Jira instance uses
different field IDs.

---

### `planar templates list`

**Synopsis:**
```
planar templates list [--system <system>] [--set <name>]
```

**Description:** Walk the resolved templates root and enumerate available
`(set, system, kind)` triples. Includes both disk templates and embedded
defaults not yet extracted to disk. Output is sorted by set, then system, then
kind.

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--system` | Filter by system name (e.g. `github-issues`, `jira`). | all |
| `--set` | Filter by template set name. | all |

**Output (human):**
```
SET                  SYSTEM               KIND                 SOURCE
----------------------------------------------------------------------
default              github-issues        issue                embedded
default              github-issues        parent-issue         embedded
default              jira                 epic                 embedded
```

**Output (`--json`):** One JSON object per line with fields `set`, `system`,
`kind`, `source`, `path`.

**Schema effects:** None.

**Capture:** None.

**Exit codes:**
- `0` — success.
- `2` — cannot resolve templates root.

---

### `planar templates show`

**Synopsis:**
```
planar templates show <set> <system> <kind> [--json]
```

**Description:** Print the raw JSON of the resolved template to stdout. Honors
the fallback chain: user-set → default-set → embedded.

**Arguments:**

| Argument | Description |
|----------|-------------|
| `<set>` | Template set name (e.g. `default`, `acme-internal`). |
| `<system>` | System name (e.g. `github-issues`, `jira`). |
| `<kind>` | Template kind (e.g. `issue`, `epic`, `story`). |

**Schema effects:** None.

**Capture:** None.

**Exit codes:**
- `0` — success.
- `1` — template not found (lists all searched paths in error message).
- `2` — cannot resolve templates root.

---

### `planar templates render`

**Synopsis:**
```
planar templates render <set> <system> <kind> --entity <kind>:<id> [--json]
```

**Description:** Build a rendering `Context` from the named entity, execute the
template, and print the resulting JSON payload to stdout. Dry run — no DB
writes, no external API calls.

**Arguments:**

| Argument | Description |
|----------|-------------|
| `<set>` | Template set name. |
| `<system>` | System name. |
| `<kind>` | Template kind. |

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--entity` | Entity reference in `kind:id` form (e.g. `task:42`, `plan:7`, `scenario:3`). Required. | — |

**Output:** Pretty-printed JSON of the rendered payload.

**Schema effects:** None.

**Capture:** None.

**Exit codes:**
- `0` — success.
- `1` — template not found or entity not found.
- `2` — database error or templates-root resolution failure.

---

### `planar templates validate`

**Synopsis:**
```
planar templates validate <set> <system> <kind> [--json]
```

**Description:** Validate one template, named by the same `<set> <system> <kind>`
triple `templates show` takes and resolved through the same fallback chain
(including embedded defaults). All three positionals are required; there is no
path form and no validate-everything form.

**Arguments:**

| Argument | Description |
|----------|-------------|
| `<set>` | Template set name. |
| `<system>` | System name. |
| `<kind>` | Template kind. |

**Output:** `ok: <set>/<system>/<kind>` on success; under `--json`,
`{"ok":true,"set":"<set>","system":"<system>","kind":"<kind>","issues":[]}`.
When issues are found the detail is written to stdout and a summary line to
stderr.

**Schema effects:** None.

**Capture:** None.

**Exit codes:**
- `0` — no issues found.
- `1` — template not found (`error: template <set>/<system>/<kind> not found`).
- `2` — one or more validation issues found, or a missing positional (`error: set is required`).

---

### `planar templates init`

**Synopsis:**
```
planar templates init [--force] [--json]
```

**Description:** Idempotently extract the embedded baseline templates to
`<templates-root>/default/<system>/<kind>.json`. Without `--force` an existing
file is never overwritten. Reports the list of files written. Also called
automatically by `planar init` (after `config init`, before `migrate apply`).

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--force` | Overwrite an existing template from the embedded default — use this to reset a template you have edited. A **directory** sitting where a template belongs is still skipped: the flag resets files, it does not remove trees. Declared and silently discarded before task 6212, so `--force` reported "nothing to do" and left the edited file in place. | off |
| `--json` | Emit the written-file list as JSON. | off |

**Output:** Lists each newly written file path, or confirms nothing was done.

**Schema effects:** None.

**Capture:** None.

**Exit codes:**
- `0` — success (whether or not any files were written).
- `2` — cannot resolve templates root or write failure.

---

### `planar templates path`

**Synopsis:**
```
planar templates path [--json] [--set <set>] [--system <system>]
```

**Options:**

| Flag | Description |
|------|-------------|
| `--set <set>` | Declared and never read by the handler. |
| `--system <system>` | Declared and never read by the handler. |

**Description:** Print the resolved templates root directory. The verb takes
no positionals — a `<set> <system> <kind>` triple is refused at parse time
(exit 2) — and the output is the same path with or without `--json`.

**Output:**
```
/Users/alice/.planar/templates
```

**Schema effects:** None.

**Capture:** None.

**Exit codes:**
- `0` — success.
- `2` — cannot resolve templates root, or a parse failure (unexpected argument).

---

## Domain: `local`

User-local sandbox for personal skills and agents. Skills live as dir-shape sources at `~/.planar/local/skills/<name>/SKILL.md`; agents are flat at `~/.planar/local/agents/<name>.md`. Each is **projected by copy** into the vendors' directories under the name `planar-local-<name>`; the source is never edited. See `docs/concepts.md § Local sandbox` for the design.

**Projection targets.** A vendor directory is written only when its presence marker exists, the same markers `install.sh` uses (`~/.claude/`; `~/.codex/` or an explicit `$CODEX_HOME`; `~/.copilot/`; `~/.gemini/settings.json`; `~/.gemini/antigravity-cli/`; `~/.config/opencode/`).

| Source | Destination | Present when |
|---|---|---|
| skill | `~/.claude/skills/planar-local-<name>/` | `~/.claude/` exists |
| skill | `~/.agents/skills/planar-local-<name>/` (shared root) | Codex, Copilot, Gemini CLI or OpenCode is present |
| skill | `~/.gemini/antigravity-cli/skills/planar-local-<name>/` | `~/.gemini/antigravity-cli/` exists |
| agent | `~/.claude/agents/planar-local-<name>.md` | Claude present |
| agent | `$CODEX_HOME` or `~/.codex` `/agents/planar-local-<name>.toml` | Codex present |
| agent | `~/.copilot/agents/planar-local-<name>.agent.md` | Copilot present |
| agent | `~/.gemini/agents/planar-local-<name>.md` | Gemini CLI present |
| agent | `~/.gemini/antigravity-cli/agents/planar-local-<name>.md` | Antigravity present |
| agent | `~/.config/opencode/agents/planar-local-<name>.md` | OpenCode present |

A skill copy is the whole skill directory with the frontmatter `name` rewritten to `planar-local-<name>`. A Markdown agent copy is the source with `name` rewritten (or added). The OpenCode copy keeps only `description` and `mode: subagent`. The Codex file is TOML with `name`, `description` and `developer_instructions`, byte-identical to what `scripts/render-codex-agents.py` writes. An agent without a `description` cannot take the OpenCode or Codex form and is skipped for those vendors. A skill's `vendors:` list narrows its copies (`claude` gates the Claude copy; `codex` and `copilot` gate the shared and Antigravity copies); agents ignore it.

**Local names.** A local name is the source's directory name (skills) or file stem (agents), and it becomes part of an Agent Skills name, `planar-local-<name>`. A name must match `^[a-z0-9]+(-[a-z0-9]+)*$` (lowercase letters, digits and single hyphens, none leading, trailing or doubled) and be at most 51 characters, so the projected name stays within 64. `planar local import` and `planar local link` refuse any other name at exit 2 (`invalid_input`) with a message that states the pattern and the limit and names the offending name; nothing is written. Names are never normalized: `My_Skill` is refused, not linked as `my-skill`, because normalizing would let two sources collide on one projection. `link --reconcile` does not project a source whose name breaks the rule, and `list` and `unlink` still act on what was already recorded.

**Retired `shadow` key.** A local source whose frontmatter sets `shadow` (any value) is refused by `planar local import` and `planar local link` at exit 2, naming the retired key. A local source can no longer take the name of a bundled skill; every projection carries the `planar-local-` prefix. `local link --json` no longer emits a `Shadow` field.

**Ownership.** A destination that exists and differs from a fresh projection is replaced only when it is a prior Planar projection: recorded in a `.link-manifest.json`, or a symlink into `~/.planar/local/`. Anything else is refused at exit 6 (`already_exists`) naming the path, before any destination is written. A byte-identical destination is left alone.

### `planar local link [<name>]`

Walk `~/.planar/local/{skills,agents}/`, parse each source file's YAML frontmatter, and project a copy into every present vendor directory (table above).

| Flag | Description |
|---|---|
| `--dry-run` | Preview the planned projections without touching the filesystem. |
| `--vendor <v>` | Restrict to one vendor (`claude`, `codex`, `copilot`, `gemini`, `antigravity`, `opencode`). `codex`, `copilot`, `gemini` and `opencode` also select the shared `~/.agents/skills` copy. `shared` selects it directly. |
| `--reconcile` | Run the reconcile pass instead of linking. Takes no `<name>`; combining the two is refused as invalid input. Honours `--dry-run` and `--json`. |
| `--json` | Emit JSON instead of human text. |

**Behavior:**
- Without `<name>`, links every source file under both `skills/` and `agents/`. A source with an invalid name or the retired `shadow` key stops the whole run at exit 2 before anything is written.
- With `<name>`, links only the matching source (errors if not found).
- Idempotent: a re-link of an unchanged source produces `unchanged` actions and writes nothing.
- Atomic: each copy is written to a sibling temp path and renamed into place.
- Edits to a source reach the copies only when `local link` (or `--reconcile`) runs again; `local list` reports the lag as `stale`.

**`--reconcile`.** Makes the disk match the sources it already tracks (a manifest entry, or an old projection to migrate), in one pass that checks every destination before writing any:
- `stale`: a prior projection differs from a fresh one; it is rewritten.
- `target-missing`: a tracked projection is absent; it is written.
- `legacy`: an old projection (`~/.claude/commands/local-<name>.md`, `~/.codex/skills/local-<name>`, `~/.copilot/skills/local-<name>`, `~/.planar/agents/local-<name>.md`) is removed when it is a symlink into `~/.planar/local/` or a copy equal to its recorded source; the new projections are written in its place. A foreign file at an old path is left alone.
- `source-missing`: the source is gone; its recorded projections are removed and the manifest entry dropped.

Text output prints `done: N change(s)` (`dry-run: N change(s) would be made`). A second run with nothing to change prints `reconcile: manifest already consistent with the filesystem` and modifies nothing: it compares bytes before writing.

### `planar local migrate [--dry-run]`

Convert legacy flat sandbox skills (`~/.planar/local/skills/<name>.md`) into the dir-shape layout (`<name>/SKILL.md`) that skill projection requires.

| Flag | Description |
|---|---|
| `--dry-run` | Preview without renaming on disk. |
| `--json` | Emit JSON instead of human text. |

**Behavior:**
- Idempotent. Skills already in dir-shape are skipped silently.
- A collision (target `<name>/` exists but is not a matching skill dir) is skipped with a reason; nothing is overwritten.
- Agents (`~/.planar/local/agents/<name>.md`) are intentionally left alone — they stay flat.

### `planar local import <path>`

Import operator-authored skill or agent files from an external location into the sandbox and project them into every present vendor directory. The operator points at a single flat `.md` file, a single dir-shape skill source (`foo/` containing `SKILL.md`), or a directory containing a mix of both. Each valid input is materialized in the sandbox in the correct shape — skills as `~/.planar/local/skills/<name>/SKILL.md` (auxiliary files inside dir-shape inputs travel along); agents as `~/.planar/local/agents/<name>.md`. Linking then runs via the same path as `planar local link`.

| Flag | Description |
|---|---|
| `--kind <skill\|agent>` | Target kind. Default `skill`. The frontmatter `kind:` field (if present) must agree with this flag. |
| `--force` | Overwrite an existing sandbox file of the same name. Without this, name collisions are skipped — the operator's prior work is never silently lost. |
| `--dry-run` | Preview the planned imports and links without writing. |
| `--no-link` | Import only; skip the link step. Useful when the operator wants to inspect the sandbox copy before linking. |
| `--json` | Emit JSON instead of human text. |

**Behavior:**
- A single flat `.md` file source imports that file (wrapped into `<name>/SKILL.md` for skills). A single dir-shape source (a directory whose top-level contains `SKILL.md`) imports that whole tree. A directory source containing a mix of `*.md` files and `<name>/SKILL.md` subdirs imports each top-level entry — other subdirectories and non-`.md` files are ignored.
- Every entry's name is checked against the local-name rule above, and a source that sets `shadow` is refused; either refuses the whole import at exit 2 before anything is copied, a dry run included.
- Each file's frontmatter is validated by the same parser as `planar local link` (YAML frontmatter required; `vendors:` if present must be a subset of `{claude, codex, copilot}`; `kind:` if present must match `--kind`).
- After a successful import, the link layer is invoked automatically (suppress with `--no-link`).
- `.link-manifest.json` files in the source directory are explicitly ignored so pointing the importer at the sandbox itself does not corrupt it.

### `planar local unlink <name> [--purge]`

Remove every projection recorded for `<name>` in the per-kind `.link-manifest.json`. With `--purge`, also delete the source file from `~/.planar/local/`. Accepts `--json`.

### `planar local list [--vendor <v>] [--json]`

List every recorded projection across both kinds and all vendors. The vendor column is `claude`, `shared` (the shared skills root), `antigravity`, `codex`, `copilot`, `gemini` or `opencode`. Status column, recomputed from disk:

- `live` — the installed copy equals a fresh projection of the source.
- `stale` — the copy exists and differs: the source (or the copy) was edited since the last link.
- `missing` — nothing is at the destination.
- `broken` — the recorded source is gone.
- `legacy` — an old symlink or copy projection (see `--reconcile`); `local link --reconcile` migrates it.

`live`/`stale`/`missing`/`broken`/`legacy` colorize via the standard palette when colors are enabled. The manifest is not authoritative: each state comes from comparing bytes on disk.

### `PLANAR_LOCAL_HOME` env var

Test hook: when set, `planar local` uses this directory as the operator's `$HOME` for resolving `~/.planar/local/` and the per-vendor projection targets, and ignores `$CODEX_HOME`. Production use never sets this.

## The editflow quartet: `edit` / `view` / `diff` / `review`

Six entity families — `plan`, `task`, `question`, `decision`, `scenario`
and `artifact` — each expose the same four editor-first verbs over their
workbench file. Twenty-four leaves, one shared implementation
(`handlers/drafting.cppm`): the bodies are identical modulo the family
noun and its positional name, so they are documented once here rather
than twenty-four times.

| verb | what it does |
|------|--------------|
| `edit` | open the entity's workbench file in `$EDITOR` |
| `view` | print the entity's workbench file |
| `diff` | diff the workbench file against the database-stored version |
| `review` | reviewer entry point over that same diff |

**Synopsis:**
```
planar <family> edit   <id> [--no-pull] [--json]
planar <family> view   <id>
planar <family> diff   <id>
planar <family> review <id> [--approve] [--request-changes] [--json]
```

`<family>` is one of `plan`, `task`, `question`, `decision`, `scenario`,
`artifact`. The positional is that family's id (`plan-id`, `task-id`, …).

The twenty-four concrete leaves this covers:

| family | leaves |
|--------|--------|
| `plan` | `planar plan edit`, `planar plan view`, `planar plan diff`, `planar plan review` |
| `task` | `planar task edit`, `planar task view`, `planar task diff`, `planar task review` |
| `question` | `planar question edit`, `planar question view`, `planar question diff`, `planar question review` |
| `decision` | `planar decision edit`, `planar decision view`, `planar decision diff`, `planar decision review` |
| `scenario` | `planar scenario edit`, `planar scenario view`, `planar scenario diff`, `planar scenario review` |
| `artifact` | `planar artifact edit`, `planar artifact view`, `planar artifact diff`, `planar artifact review` |

### The four verbs do NOT validate alike

This asymmetry is operator-visible on every failing invocation, and is
reproduced from the oracle deliberately rather than smoothed over:

| verb | rejects `id <= 0`? | maps failures to prose? |
|------|--------------------|-------------------------|
| `view` | no | no |
| `edit` | no | no |
| `diff` | **yes** | **yes** |
| `review` | **yes** | **yes** |

So `planar question diff 0` exits 2 with `question id must be a positive
integer, got 0`, while `planar question view 0` falls through to the
resolver and reports a missing plan link instead. `view` and `edit` have
no failure-mapping arms at all: a failure surfaces as the bare error tag.

### A nonexistent id reports differently per family

`plan` and `task` resolve their anchor directly (`plans.parent_plan_id`,
`tasks.plan_id`); the other four resolve theirs through `entity_links`.
That changes the refusal text:

```
planar plan diff 999      →  no plan with id 999
planar task diff 999      →  no task with id 999
planar question diff 999  →  question 999 is not linked to a plan; ...
```

### `--no-pull` and `--json` on `edit` are DECLARED AND INERT

Both flags appear in `planar schema` and are accepted, and the handler
reads neither. They exist because the oracle declares them and dropping
them would break catalog parity; refusing them would reject an invocation
the oracle accepts. Recorded here rather than left for an operator to
discover — do not expect `edit --json` to produce JSON.

**Description:** `view` renders the entity to its workbench path. For
`plan`, the ANCHOR plan renders to the feature's `README.md` and every
other plan to `plans/<slug>.md`.

**Schema effects:** Reads the family's table and (for the four
link-anchored families) `entity_links`. `edit` may write the workbench
file; none of the four writes the entity row.

**Capture:** None.

**Exit codes:**
- `2` — `diff` / `review` only: non-positive id.
- `1` — id not found, or (link-anchored families) no plan link.

---

### `planar workbench edit <plan>`

Not part of the quartet: it edits a whole feature's workbench files rather
than one entity's.

**Synopsis:**
```
planar workbench edit <plan> [--editor <cmd>] [--json]
```

**Description:** Edit a feature's workbench files in `$EDITOR`.

**Schema effects:** Reads `plans`; writes workbench files under
`$PLANAR_WORKBENCH_ROOT`.

**Capture:** None.

**Exit codes:**
- `1` — plan not found.

---

## Domain: `models`

Routing-model discovery, the advisory evals scorecard, and the operator
candidate registry. Planar does NOT decide what model to use: it records
the vendor and candidate an agent reports when it claims work, and never
validates that string against a supported list. Everything here either
reports what is installed, aggregates evidence after the fact, or manages
operator-declared candidates.

The subcommand set is exactly `evals`, `resolve`, `experiments`, `outcomes`
and `registry`. The plan-540 discovery family — `models list`, `models
refresh`, `models routing`, `models apply`, `models candidates` — was
removed with the curated catalog and the `[models]` / `[roles]` config
blocks (see `docs/concepts.md` § Model routing); every one of them now fails
at parse time with exit 2. Tier→model presets live in the orchestration
layer's `agents/models.md`, hand-maintained, and no `planar` verb writes
them.

### `planar models evals`

**Synopsis:**
```
planar models evals [--vendor <vendor> --role <role> --tier <tier> --work-type <type>
                     --complexity <complexity> --project <id> --validation-policy <v>
                     --routing-policy <v>] [--min-samples <n>] [--quality-floor <f>] [--json]
```

**Description:** Read-only candidate ranking. It has two modes, selected by
whether a non-empty `--vendor` is supplied:

- **Cohort ranking (evidence-backed).** With `--vendor` set, the other seven
  cohort flags (`--role`, `--tier`, `--work-type`, `--complexity`,
  `--project`, `--validation-policy`, `--routing-policy`) are required, and
  the command ranks candidates in that exact cohort over
  declared-experiment terminal samples (`routing_terminal_samples`): sample
  and success counts, the raw rate, the 95% Wilson lower bound,
  gate-failure rate, and expected excess iterations. Candidates with fewer
  than `--min-samples` (default 5) samples are labelled `insufficient_data`
  and never ranked; candidates whose Wilson lower bound is below
  `--quality-floor` (default 0.5) are excluded before any iteration or
  gate-failure ordering, so a fast-but-wrong candidate cannot outrank a
  slower correct one. "No recommendation" is a valid outcome and means keep
  the configured default.
- **Legacy scorecard.** With no `--vendor`, the command falls back to the
  pre-evidence-plane aggregation over the `dispatch_shape:` /
  `model_choice:` note convention in `session_entries`, joined with
  terminal `agent_work_claims` status and `agent_actions(action_kind='test_coder')`
  rows. It emits a per-`(work_type, candidate)` scorecard and preview-only
  recommendations, counts malformed or pre-convention notes in
  `legacy_dispatch_notes_skipped`, and reports `quality_gate_pass_fail=false`
  in `signals_sourced` because that signal is not persisted. It is
  inspectable but not evidence-backed.

Both modes write nothing: no routing-map mutation, no database write, no
config write. Applying a recommendation is a separate operator action in
`agents/models.md`.

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--vendor <vendor>` | Cohort vendor; a non-empty value selects cohort ranking. | none (legacy mode) |
| `--role <role>` | Cohort role. Required with `--vendor`. | none |
| `--tier <tier>` | Cohort tier: `small`, `medium`, `large`. Required with `--vendor`. | none |
| `--work-type <type>` | Cohort work type. Required with `--vendor`. | none |
| `--complexity <c>` | Cohort complexity: `bounded`, `standard`, `high-risk`. Required with `--vendor`. | none |
| `--project <id>` | Cohort project id. Required with `--vendor`. | none |
| `--validation-policy <v>` | Cohort validation policy version. Required with `--vendor`. | none |
| `--routing-policy <v>` | Cohort routing policy version. Required with `--vendor`. | none |
| `--min-samples <n>` | Minimum samples before a candidate is ranked. | 5 |
| `--quality-floor <f>` | Wilson lower-bound floor below which a candidate is excluded. | 0.5 |
| `--json` | Emit the ranking / scorecard as JSON. | off |

**Schema effects:** Reads routing evidence tables (cohort mode) or
`session_entries` / `agent_work_claims` / `agent_actions` (legacy mode).

**Capture:** None (read-only).

**Exit codes:** `0` on success; `2` when a cohort flag is missing or malformed with `--vendor` set; `1` for database or aggregation errors.

---

### `planar models resolve`

**Synopsis:**
```
planar models resolve --role <role> [--task <id>] [--plan <id>] [--fallback-tier <tier>] [--json]
```

**Description:** Resolve a role's routing tier from its authoritative
packet, or report the fallback and why it was used. A fallback is always
reported WITH its reason — an absent or unready packet is never presented
as a derived classification.

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--role <role>` | Required. One of `planner`, `spec-reviewer`, `ingestor`, `orchestrator`, `coder`, `test-coder`, `reviewer`, `research`, `janitor`. | none |
| `--task <id>` | Task id; required for task-bound roles (coder, test-coder, reviewer, research, janitor), which resolve from the task's compiled profile. | none |
| `--plan <id>` | Anchor plan id for pre-task roles (planner, spec-reviewer, ingestor, orchestrator), which resolve from a planning packet. | none |
| `--fallback-tier <tier>` | Configured static fallback tier reported when the packet is absent or unready. | `medium` |
| `--json` | Emit `{source: packet \| static_fallback, tier, reason, work_type, complexity}` as JSON. | off |

`source: packet` means the tier came from the compiled profile;
`static_fallback` names its reason (`no_packet`, `packet_not_ready`,
`policy_not_ready`) and reports work type and complexity as null.

**Schema effects:** Reads routing packet and policy tables.

**Capture:** None (read-only).

---

### `planar models experiments`

**Synopsis:**
```
planar models experiments [--json]
```

**Description:** List declared routing experiments and how much evidence
each has produced. Evidence volume is the point: `planar models evals`
reports `insufficient_data` for an under-sampled cohort rather than a
recommendation, and this is where you see which cohorts are thin.

**Schema effects:** Reads the routing experiment tables.

**Capture:** None (read-only).

---

### `planar models outcomes`

**Synopsis:**
```
planar models outcomes [--limit <n>] [--json]
```

**Description:** List recorded terminal outcomes, **including excluded
ones and why they were excluded**. An outcome can be excluded from the
scorecard (below an evidence floor, outside a declared experiment) and
still be listed here with its exclusion reason, so a surprising evals
result can be traced to the rows behind it. `--limit <n>` caps the rows
shown (default 50).

**Schema effects:** Reads the dispatch-outcome tables.

**Capture:** None (read-only).

---

### Domain: `models registry`

Opaque operator candidates and host observations. "Opaque" is the
contract: a candidate is an exact identifier string Planar stores and
compares, never parses or normalises.

| leaf | purpose |
|------|---------|
| `planar models registry list` | List registrations, bindings, and latest observations. |
| `planar models registry add` | Register one exact opaque candidate identifier. |
| `planar models registry update` | Update enabled state and deterministic fallback order. |
| `planar models registry remove` | Remove a candidate when no immutable evidence references it. |
| `planar models registry bind` | Allow one role and tier for a candidate. |
| `planar models registry unbind` | Remove one explicit role and tier binding. |
| `planar models registry observe` | Append an exact, versioned host capability observation. |
| `planar models registry eligibility` | Report every independent eligibility gate and named exclusion reason. |
| `planar models registry verify-identity` | Compare requested and actual spawn identity without aliasing. |
| `planar models registry export` | Export the versioned registry compatibility document. |

**Arguments and flags.** Bracketed entries are optional; every other flag is required.

| leaf | arguments and flags |
|------|---------------------|
| `planar models registry list` | `[--json]` |
| `planar models registry add` | `--vendor <text> --id <text> --order <n> [--disabled]` |
| `planar models registry update` | `--candidate <n> --order <n> [--disabled]` |
| `planar models registry remove` | `--candidate <n>` |
| `planar models registry bind` | `--candidate <n> --role <text> --tier <text>` |
| `planar models registry unbind` | `--candidate <n> --role <text> --tier <text>` |
| `planar models registry observe` | `--candidate <n> --host <text> --version <n> --availability <text> --spawn-verification <text> --evidence-ref <text> --captured-at <text> --expires-at <text>` |
| `planar models registry eligibility` | `--candidate <n> --host <text> --role <text> --tier <text> --now <text> [--override-supported] [--policy-permits]` |
| `planar models registry verify-identity` | `--candidate <n> --actual-vendor <text> --actual-id <text>` |
| `planar models registry export` | `[--json]` |

**Three properties worth knowing before using these:**

- **`remove` refuses while evidence references the candidate.** Recorded
  outcomes are immutable, so a candidate that has produced any is not
  removable — deregister it with `update` instead.
- **`observe` APPENDS.** Observations are versioned and additive; a new
  observation never rewrites an older one, which is what makes
  `eligibility` able to explain itself historically.
- **`eligibility` reports EVERY gate**, not just the first failure, and
  names each exclusion reason. `verify-identity` exists because a host may
  alias one candidate onto another; it compares requested against actual
  without resolving aliases.

**Schema effects:** `list`, `eligibility`, `verify-identity` and `export`
read; `add`, `update`, `remove`, `bind`, `unbind` and `observe` write the
registry tables.

**Capture:** None.

---

## Domain: `bench`

Record and query benchmark run data — the measurement rig. A run is minted
once, accumulates journal events and file touches, and is then closed with
a terminal status. Nothing here changes planning state.

| leaf | purpose |
|------|---------|
| `planar bench start` | Mint a new run record and print its `run_uid`. |
| `planar bench event` | Append a journal event to a run. |
| `planar bench touch` | Record a declared or actual file touch for a run. |
| `planar bench harvest` | Harvest `git diff` as actual touches for a run/task. |
| `planar bench finish` | Set the terminal status on a run. |
| `planar bench show` | Show a run's full state (header + events + touches). |

**Arguments and flags.** Bracketed entries are optional; every other flag is required. `bench start --task` is repeatable and limits the declared-touch snapshot to the given task ids; omitted, every plan task is snapshotted.

| leaf | arguments and flags |
|------|---------------------|
| `planar bench start` | `<run-uid> --plan <n> --arm <text> --base-sha <text> --config-hash <text> [--config-json <text>] [--corpus-repo <text>] [--task <text>...]` |
| `planar bench event` | `<run-uid> --kind <text> --seq <n> [--payload <text>]` |
| `planar bench touch` | `<run-uid> --task <n> --path <text> --kind <text>` |
| `planar bench harvest` | `<run-uid> --task <n> --worktree <text> [--base <text>] [--head <text>]` |
| `planar bench finish` | `<run-uid> --status <text>` |
| `planar bench show` | `<run-uid> [--json]` |

**The declared-vs-actual distinction is the point.** `touch` records what a
run SAID it would change; `harvest` records what it actually changed, read
out of `git diff`. Comparing the two is why both exist — a run that touched
files it never declared is the finding the rig is built to surface.

`start` takes the `run_uid` as its required positional and prints it back; every other verb takes the same `run_uid`, so a session is
normally `start` → repeated `event`/`touch` → `harvest` → `finish`.

**Schema effects:** `show` reads; the other five write the benchmark run
tables. No planning entity is read or written.

**Capture:** None.

---

## Domain: `update`

### `planar update`

**Synopsis:**
```
planar update [--check] [--version <tag>]
```

**Description:** Update the Planar installation to a published release
(plan 1122 M3; tech spec 677, "The update verb"). The installation root is
`$PLANAR_HOME` when set, else `~/.planar`: the root `install.sh` itself uses.
The verb opens no database; the database probe, migration and live-queue
warning belong to the installer it hands off to. The release base is
`https://github.com/rdrsss/planar/releases`, or `PLANAR_RELEASE_URL` under the
bootstrap's grammar (`https://HOST[:PORT]/...`, `file:///PATH`, or
`http://127.0.0.1[:PORT]/...` / `http://localhost[:PORT]/...`; no userinfo,
query or fragment). A malformed value is refused at exit 2.

| Flag | Description | Default |
|------|-------------|---------|
| `--check` | Print `installed <v> latest <v>` and change nothing. `<v>` is `release.json`'s `version`, or `none` without one; latest is `<base>/latest/download/VERSION`, which must be a release tag. Exit 0 when they are equal, 10 when they differ. Takes no lock. Cannot be combined with `--version` (exit 2). | off |
| `--version <tag>` | Install this release (`vMAJOR.MINOR.PATCH`) instead of the latest. A malformed tag is refused at exit 2. | latest |

A plain run, or `--version <tag>`:

1. Takes the common mutation lock (`<root>.lock`, the protocol in
   `scripts/install-lib/mutation-lock.sh`, which this verb implements natively)
   as an `update`, recording its download directory
   `<root>/.planar-update/update-<hex>/` before creating it. A running install,
   update or uninstall refuses it at exit 1, naming the owner's operation and
   pid. An updater that was killed is proven dead (its pid is gone, or was
   reused) and its recorded download directory, and nothing else, is removed.
2. Reads the recovery journal under the lock. An interrupted install
   (`mutating`) or uninstall (`uninstalling`) is reported as an incomplete
   installation at exit 1 with the journal's durable retry command, before any
   verdict about the installed release; `--check` reports it the same way. The
   verb never replays recovery itself: the version-pinned bootstrap does.
   A `prepared` or aborted attempt leaves the previous completed release
   authoritative.
3. Resolves the tag. When it equals the installed release of a completed
   install, prints that there are no changes and exits 0.
4. Downloads `SHA256SUMS` (at most 1 MiB) and `planar-<os>-<arch>.tar.gz` (at
   most 512 MiB) from `<base>/download/<tag>/`, following redirects with every
   hop checked against the same grammar, sending no `Authorization` header,
   within a 10-minute bound. Exactly one checksum record must name the asset;
   the asset is verified with SHA-256.
5. Lists and extracts the archive with the system `tar` (every entry under
   `planar-<os>-<arch>/`, no `..`, no links), checks that the bundle's
   `release.json` names the tag, on Linux that the host's glibc
   (`ldd --version`) meets the bundle's `os_floor`, and that the bundle's
   `schema_version` is not below this binary's own (an older release is never
   installed over a newer database).
6. Replaces itself with `bash <bundle>/install.sh --prebuilt <bundle> --cleanup
   <download dir>`, passing `PLANAR_MUTATION_HANDOFF=<generation>:<nonce>`. The
   lock is never released before the exec: the installer adopts it (the exec
   keeps the pid and start time), removes the download directory and releases
   the lock when it ends. If the exec fails, the verb removes the directory and
   releases the lock itself.

`SIGINT` (Ctrl-C) or `SIGTERM` from just before step 1 until the exec stops
the run: a download in flight is abandoned within about a second, the download
directory is removed, the lock is released (`released.<G>`, so the next update
starts clean rather than reclaiming), and the verb prints `error: planar update
was interrupted by SIGINT; ...` and exits `130` (`143` for `SIGTERM`), the
codes the bootstrap's own traps use. The message ends "the mutation lock
released" only when this run had taken the lock; a signal that arrives before
the lock is taken says it came "before it took the mutation lock" and that no
lock was held. A signal that was ignored when the verb
started (a background job's `SIGINT`) stays ignored. Immediately before the
exec the verb restores the signals' previous dispositions, so one arriving
from then on is never lost: before the exec it ends the verb as a `KILL` would
(the next owner proves the updater dead and removes its recorded directory);
after it the installer owns the directory and the lock and handles it.
`--check` holds nothing and does not catch either signal.

The shadow check belongs to the installer, after placement: when `command -v
planar` and `<root>/bin/planar` are different files (compared after resolving
symlinks, so a `PATH` entry that links to the installed binary is not a
shadow) it prints `<path> shadows the installed <root>/bin/planar: your shell
runs <path>. Put <root>/bin first on PATH, or remove <path>`. It names
`~/.local/bin/planar` for that path and appends `(the retired 'make install'
put it there)` only when that is the shadowing path. The verb does not repeat
it: it never regains control after the exec. The bootstrap prints the warning
once: it leaves it to a bundled installer that carries the check and keeps the
same check, with the same wording prefixed `get-planar: warning: `, only for an
older bundle that lacks it.

Refusals use the bootstrap's (`get-planar.sh`) messages, after `error: `:
`cannot reach the release server at <base>: ...`, `release <tag> does not
exist on the release server <base>: ...`, `checksum mismatch for <asset>: ...`,
`<base>/latest/download/VERSION holds '<value>', which is not a release tag
...`, `this host has glibc <host> but this release needs glibc <floor> or
later; nothing was installed`, and `unsupported platform <os> <arch>; ...`.
Every refusal before the exec, a failed exec, and an interrupt leave no
download directory and no held lock.

**Writes:** outside the database only: the lock record
`<root>.lock/owner.<G>` and the download directory under
`<root>/.planar-update/`; everything else is the installer's.

**Exit codes:** `0` success or no changes, `1` a fault, refusal, competing
owner or incomplete installation, `2` bad input, `10` (`--check` only) an
update is available, `130` / `143` interrupted by `SIGINT` / `SIGTERM` before
the hand-off.

**Capture:** None.

---

## Miscellaneous leaves

Commands that do not group into a larger domain page.

### `planar version`

**Synopsis:**
```
planar version
```

**Description:** Print the planar version, commit, and C++ toolchain. Git
metadata (sha, date, dirty flag) is embedded only in builds configured with
`-DPLANAR_VERSION_META=ON`; a dev build prints the stable sentinel `dev`
instead. That is deliberate — resolving it by default bakes the live sha
into a module every binary imports, so every commit invalidates the whole
build graph.

**Capture:** None.

---

### `planar completion`

**Synopsis:**
```
planar completion <shell>
```

**Description:** Generate the autocompletion script for the specified
shell.

**Capture:** None.

---

### `planar skills`

**Synopsis:**
```
planar skills
```

**Description:** **Retired.** Rendering and drift detection moved to the
external renderer binary (plan 918 M5). Planar no longer renders
vendor projections nor tracks their install-drift in-band. The command
remains so that an operator running it gets told where the functionality
went rather than a bare unknown-command error.

**Capture:** None.

---

### `planar task cancel <task-id>`

**Synopsis:**
```
planar task cancel <task-id> [--json] [--scope <scope>]
```

**Options:**

| Flag | Description |
|------|-------------|
| `--scope <scope>` | Declared and never read by the handler. |

**Description:** Cancel a task. `cancelled` is a terminal status, reachable
from `todo` or `doing`; see the status lifecycle under `planar task`.
Single-argument form only.

**Schema effects:** Writes `tasks.status`.

**Capture:** Yes.

---

### `planar plan step list <plan-id>`

**Synopsis:**
```
planar plan step list <plan-id> [--json]
```

**Description:** List a plan's steps with their status and any linked task.
The same step data `planar plan show` renders inline, without the
surrounding plan detail.

**Schema effects:** Reads `plan_steps`.

**Capture:** None (read-only).

---

### `planar plan divergence <plan-id>`

**Synopsis:**
```
planar plan divergence <plan-id> [--json]
```

**Description:** Report the declared-vs-derived closure divergence for a
plan's open tasks (decision D4). Declared closure is what the tasks say
they touch; derived closure is what the graph implies. A divergence means
one of the two is wrong, and the report names which tasks disagree rather
than silently reconciling them.

**Schema effects:** Reads `tasks`, `entity_links`, and the closure tables.

**Capture:** None (read-only).

---

### `planar groups recommend <plan-id>`

**Synopsis:**
```
planar groups recommend <plan-id> [--json] [--budget <n>] [--solver <name>]
```

**Options:**

| Flag | Description |
|------|-------------|
| `--budget <n>` | Per-slice window budget, an unsigned 32-bit integer (default `128000`). An unparseable or out-of-range value is refused as invalid input. |
| `--solver <name>` | Requested solver (`greedy` default, or `mtkahypar`). The recommendation's `solver` field reports the one that actually ran; `--solver mtkahypar` is accepted and degrades to `greedy` with `optimal_available:false` on this build, which ships greedy only. The solver arm lives on branch `dev/grouping-solvers` (decision 1293). An unknown value is refused. |

**Description:** Recommend closure-minimizing task slices for a plan —
groupings that keep each slice's touched surface as small as possible.
Advisory: it proposes slices, it does not create or reorder anything.

**Schema effects:** Reads `tasks` and the closure/touches tables.

**Capture:** None (read-only).

---

### `planar decision link` / `planar scenario link`

**Synopsis:**
```
planar decision link <decision-id> <kind>:<id> [--relationship <rel>] [--json] [--scope <scope>]
planar scenario link <scenario-id> <kind>:<id> [--relationship <rel>] [--json] [--scope <scope>]
```

**Options:**

| Flag | Description |
|------|-------------|
| `--scope <scope>` | Declared and never read by the handler. |

**Description:** Create an entity link from a decision (or scenario) to
another entity. Two arms of the same seven-arm entity-link surface `planar
link` exposes; see that domain for the relationship vocabulary and the
note that link verbs are **deliberately unguarded** by the cross-scope
guard, because `entity_links` edges legitimately cross scopes.

**Schema effects:** Writes `entity_links`.

**Capture:** Yes.

---

### `planar workflow run`

**Synopsis:**
```
planar workflow run <workflow> [--args <json>] [--local]
```

**Options:**

| Flag | Description |
|------|-------------|
| `--local` | Restrict resolution to sandbox (local) workflows only. |

**Description:** Run an installed workflow. See the `workflow` domain above
for discovery and the workflow contract.

**Capture:** Yes.

---

### Domain: `annotate` — the remaining leaves

The `annotate` domain's core verbs (`add`, `list`, `show`, `update`,
`remove`, `tag`, `capabilities`) are documented under their own domain.
These six complete it.

| leaf | purpose |
|------|---------|
| `planar annotate dismiss` | Dismiss an annotation. |
| `planar annotate archive` | Archive an annotation. |
| `planar annotate bulk-dismiss` | Dismiss every ACTIVE annotation matching the filter. |
| `planar annotate bulk-archive` | Archive every annotation matching the filter, **including non-active rows**. |
| `planar annotate command` | Apply a receipt-backed annotation JSON request from stdin (`--request @-`). |
| `planar annotate receipt` | Look up a durable annotation command receipt. |

**Arguments and flags.** Bracketed entries are optional.

| leaf | arguments and flags |
|------|---------------------|
| `planar annotate dismiss` | `<annotation-id> [--json]` |
| `planar annotate archive` | `<annotation-id> [--json]` |
| `planar annotate bulk-dismiss` | `[--operation-id <text>] [--anchor-path <text>] [--plan <n>] [--task <n>] [--vendor <text>] [--tag <text>] [--scope <text>] [--json]` |
| `planar annotate bulk-archive` | `[--operation-id <text>] [--anchor-path <text>] [--plan <n>] [--task <n>] [--vendor <text>] [--tag <text>] [--scope <text>] [--json]` |
| `planar annotate command` | `[--request <text>] [--json]` |
| `planar annotate receipt` | `[--source-uuid <text>] [--operation-id <text>] [--json]` |

**The two bulk verbs do not have the same reach**, and the asymmetry is
easy to miss: `bulk-dismiss` acts only on ACTIVE rows, while
`bulk-archive` acts on everything the filter matches regardless of state.
A filter that looks equivalent between the two will not affect the same
set.

**`command` and `receipt` are a pair.** `command` applies a JSON request
read from stdin and mints a durable receipt for it; `receipt` looks that
receipt up afterwards. The receipt is what makes a bulk mutation auditable
after the fact, so the two are only useful together.

**Schema effects:** All six write or read `annotations`; `command` and
`receipt` also touch the receipt store.

**Capture:** Yes for the mutating verbs; none for `receipt`.

---

## Domain: `tree`

> **Stale section (task 6140, 2026-09-09).** `planar tree`'s actual flag set
> (`planar schema`) is `--scope`, `--all-scopes`, `--depth`, `--kind`,
> `--status`, `--sort`, `--json` — none of them has a short alias. It does
> not implement `-L`, `-I`/`-P`, `--ignore-case`, `-r`/`-t`/`-c`/`-U`,
> `--dirsfirst`/`--no-dirsfirst`, `--noreport`, `--prune`, `-i`/`--no-indent`,
> `--ascii`, `--no-truncate`, or `-J`; each fails at parse time with exit 2
> (`error: tree: The following arguments were not expected: <flag>`). Task
> 6140 fixed only the `--dirsfirst`/`--no-dirsfirst` claims named in its
> scope; the rest of this synopsis and options table describes a much
> larger aspirational surface and is filed separately as a follow-up
> doc-drift finding rather than rewritten here.

The `tree` domain provides a hierarchical view of Planar entities — plans, tasks, artifacts, decisions, scenarios, and questions — for one or all scopes. Read-only; no schema effects.

The walk follows `plans.parent_plan_id` for plan→plan, `tasks.plan_id` and `tasks.parent_task_id` for plan→task and task→subtask, and `entity_links(relationship='derives-from')` for artifacts / decisions / scenarios / questions attached to plans. Filters apply during the walk so excluded subtrees never enter the output.

The flag surface deliberately mirrors `tree(1)` wherever the semantic translates. Filesystem-specific flags from `tree(1)` are explicitly rejected at parse time rather than silently ignored — see [`tree(1)` flags with no Planar analog](#tree1-flags-with-no-planar-analog).

### `planar tree`

**Synopsis:**
```
planar tree [--scope <scope> | --all-scopes] [--depth <N>]
            [--kind <kind>] [--status <status>] [--sort <key>] [--json]
```

**Description:** Render a hierarchical view of the cwd-derived read set (default) or another scope, walking plans → tasks → derived artifacts/decisions/scenarios/questions. At a workspace root, the default output contains one scope root for the workspace org and one for each member project; inside a member repo, the most specific repo root is used.

**Options:**

**This table was rewritten on 2026-09-11 (task 6675) against the binary.** Earlier editions documented twenty-two flags modelled on `tree(1)`, fifteen of which do not exist. Each was probed individually: everything absent below fails at parse time with exit 2 (`error: tree: The following argument was not expected: <flag>`).

| Flag | Default | Description |
|------|---------|-------------|
| `--scope <X>` | cwd-derived | Render this scope only. Accepts `global`, `repo:<slug>`, `assoc:<slug>`, or a bare association slug. |
| `--all-scopes` | off | Render every scope as a separate section. |
| `--depth <N>` | `-1` (unbounded) | Max tree depth. |
| `--kind <kind>` | all | Restrict to a **single** kind. Not repeatable and not a comma-list, despite what earlier editions of this page claimed — `--help` says "Restrict to a single kind". |
| `--status <status>` | all | Restrict to a **single** status. Same caveat as `--kind`. |
| `--sort <key>` | `id` | Sort key. |
| `--json` | off | Emit the subtree as JSON instead of human text. |

That is the complete flag set. `planar schema` declares exactly these seven.

#### Flags this page used to document that do not exist

None of these are accepted; all fail at parse time with exit 2. They are listed only so a reader who remembers them from an earlier edition can stop looking.

`-L`, `-I`, `--ignore`, `-P`, `--match`, `--ignore-case`, `-r`, `--reverse`, `-t`, `--sort-updated`, `-c`, `--sort-created`, `-U`, `--unsorted`, `--noreport`, `--prune`, `-i`, `--no-indent`, `--ascii`, `--no-truncate`, `-J`.

Note that `--depth`, `--kind`, `--status`, `--sort` and `--json` **do** exist — but their short aliases (`-L`, `-J`) do not.

#### `tree(1)` flags with no Planar analog

Earlier editions claimed these "are rejected at parse time with a clean error pointing at the Planar alternative (or noting no analog exists) rather than silently ignored". **That is not what happens.** They produce the same generic parse error as any other unknown flag, with no alternative suggested:

```
$ planar tree --scope global -d   # cli-lint-ignore: the flag's ABSENCE is the point
error: tree: The following argument was not expected: -d
(exit 2)
```

The list below is retained as design rationale for why no analog was built — not as a description of any behaviour:

| Flag | Rationale for having no analog |
|------|-------------------------------|
| `-a` | no concept of hidden entities |
| `-d` | `--kind plan` covers it |
| `-f` | entity references are `kind:id`; a full path is redundant |
| `-s`, `-h`, `-p`, `-u`, `-g`, `-D` | no filesystem analog |
| `--inodes`, `--device` | filesystem-specific |
| `-Q` | Planar titles are sanitized at insert; no quoting needed |
| `-X`, `-H` | use `--json` and pipe through a transformer |
| `-v` | no semver in entity titles |
| `--filelimit`, `--matchdirs` | out of scope; revisit on user signal |
| `-C` | no color output exists at all (see [Color output](concepts.md#color-output)) |
| `-o` | use a shell redirect (`> file`) |
**Output (human):**
```
assoc:project:planar
├── plan:1 [active]  Backlog
│   ├── plan:2 [active]  ext propagate --github-strategy override
│   │   ├── task:1  Add --github-strategy flag                       [todo, pri:10]
│   │   └── task:2  Plumb override                                   [todo, pri:20]
│   ├── artifact:1  Architecture overview                            [tech_spec, active]
│   └── question:3  scope handler prints errors twice                [open]
└── plan:4 [active]  Entity visibility: scope columns and tree verb
    └── plan:5 [done]  Scope column on *-list commands

2 plans, 2 tasks, 1 artifact, 0 decisions, 0 scenarios, 1 question
```

With `--all-scopes`, every scope is rendered as its own section. The `global` section is always emitted, even when it has zero entities.

**Output (`--json`):** A nested JSON object (or array when multiple roots from `--all-scopes`) with the shape:
```json
{
  "kind": "scope",
  "scope_kind": "association",
  "scope_id": 1,
  "scope_label": "assoc:project:planar",
  "children": [
    {
      "kind": "plan",
      "id": 1,
      "title": "Backlog",
      "slug": "backlog",
      "status": "active",
      "created_at": "2026-05-13T...",
      "updated_at": "2026-05-13T...",
      "children": [...]
    }
  ]
}
```

The `children` array is always present, even when empty (the empty-`global` signal under `--all-scopes`).

**Schema effects:** None. Read-only.

**Capture:** None.

**Exit codes:**
- `0` — success.
- `1` — invalid flag (including any deliberately-omitted `tree(1)` flag), invalid `--kind` / `--sort` value, conflicting sort flags, or unresolvable `--scope`.
- `2` — database open or query failure.

---

## Binary: `planar-agent`

`planar-agent` is the agent-callable coordination binary. It owns coordination
writes to `agent_work_claims`, `agent_actions`, `workflow_runs`,
`context_records`, and the `routing_dispatch_*` authorization tables (behind
`dispatch preview` / `dispatch confirm`), plus the bounded `tasks.status`
transitions performed by atomic terminal operations. It also owns the queue
tables in `planar.db` (`queue_entries`, `queue_history` and their compatibility
marker `queue_schema`, behind `queue run`). Operator-recovery verbs (`reconcile`, `abort`) live
here because the capability boundary tracks write ownership, not audience. See
[Five-binary architecture](architecture.md#five-binary-architecture) for the
binary split.

Schema-version handshake: `planar-agent` is a **consumer** of the schema, not its owner. Startup queries `schema_migrations.max(version)` and refuses with exit **7** when the live DB is older than the binary's embedded minimum. The remediation pointer ("run `planar init`") is printed to stderr.

### Verb surface

```text
# Atomic operations — each wraps (claim lifecycle + action lifecycle +
# task status transition) in a single BEGIN IMMEDIATE transaction.
planar-agent pull       <plan-id> [--vendor <v>] [--vendor-session <vendor:id>] [--role coder] [--ttl <duration>] [--purpose <text>] [--base-ref <git-ref>] [--worktree <id-or-path>] [--repo-root <path>] [--no-locality-probe] [--metadata <json>] [--parent-action <action-id>] [--run <run-id>] [--stage <stage>] [--json]
planar-agent peek       <plan-id> [--json]
planar-agent complete   --claim <token> [--summary <text>] [--as caller|engine] [--attempt <id>] [--override-supervisor] [--json]
planar-agent fail       --claim <token> --reason <text> [--category usage_limit|context_limit|output_limit|tool_failure|validation|unknown] [--as caller|engine] [--attempt <id>] [--override-supervisor] [--json]
planar-agent release    --claim <token> [--reason <text>] [--as caller|engine] [--attempt <id>] [--override-supervisor] [--json]
planar-agent block      --claim <token> --blocker <task-id> [--reason <text>] [--as caller|engine] [--attempt <id>] [--override-supervisor] [--json]

# Direct claims — for orchestrator dispatch when the caller already knows the
# target entity by id. Task claims atomically transition todo → doing unless
# --no-transition is supplied; plan and plan-step state is never changed.
planar-agent claim      --entity task:<id>|plan:<id>|plan_step:<id> [--vendor <v>] [--vendor-session <vendor:id>] [--role <r>] [--model <s>] [--ttl <duration>] [--purpose <text>] [--worktree <id-or-path>] [--repo-root <path>] [--no-locality-probe] [--no-transition] [--force] [--run <run-id>] [--stage <stage>] [--json]
planar-agent heartbeat  --claim <token> [--ttl <duration>] [--status <text>] [--as caller|engine] [--attempt <id>] [--json]

# Associate an already-acquired active claim with a workflow run (and
# optional stage) after the fact, for claims taken before the run existed,
# and/or hand it to the engine supervisor (plan 1033; see "Engine
# supervision" below). At least one of --run or --supervisor.
planar-agent claim-associate --claim <token> [--run <run-id> [--stage <s>]] [--supervisor caller|engine [--attempt <id>]] [--json]

# `--model` records the model actually used, verbatim, as an OPAQUE STRING.
# Planar does not decide, validate, or publish what is "supported": an
# unrecognized value is stored, not rejected, and a vendor/model pair that
# disagrees is recorded as given rather than corrected. The catalog and spawn
# verification belong to the orchestration/host boundary. Omitting it is valid and
# stores NULL — reporting is optional.

# Nested action lifecycle — for sub-tool-calls or sub-phases inside a
# claim. Optional; lightweight claims skip these.
planar-agent action start  --claim <token> --kind <kind> [--entity <kind>:<id>] [--vendor-role <s>] [--repo-root <path>] [--no-locality-probe] [--metadata <json>] [--json]
planar-agent action end    --action <id> [--outcome ok|error|aborted|timeout] [--summary <s>] [--json]

# Vendor hook ingestion — translates hook events into the primitives.
# M2 ships a skeleton handler; full adapter routing lands in M4.
planar-agent ingest     --vendor claude --event @<file|-> [--json]

# Operator recovery — agent_* table writers, which is why they live on
# planar-agent (not planar). The operator invokes them directly; vendor
# hooks never do.
planar-agent reconcile  [--dry-run] [--stale-after <duration>] [--plan <id>] [--category usage_limit|context_limit|output_limit|tool_failure|validation|unknown] [--override-supervisor] [--json]
planar-agent abort      --claim <token> [--reason <text>] [--category usage_limit|context_limit|output_limit|tool_failure|validation|unknown] [--vendor <s>] [--vendor-session <vendor:id>] [--override-supervisor] [--json]

# Workflow run lifecycle — used by an external workflow harness to manage
# workflow_runs rows while staying DB-handle-free (decision 444). The caller
# supplies the harness pid (not getpid()) so crash reconciliation probes the
# right process. `abandoned` status is reserved for `reconcile`; `run end`
# never writes it.
#
# `--pid` and `--ttl` are mutually exclusive supervision modes (decision
# D11, task 6847): exactly one is required. A pid-bound run (`--pid`) is
# probed for liveness by `reconcile`. A pid-less run (`--ttl`) carries a
# lease in `expires_at`, extended by `run heartbeat`, and `reconcile`
# abandons it only once that lease has lapsed — it is never pid-probed.
# Supplying neither refuses at InvalidInput and writes no row.
planar-agent run start     --plan <plan-id> --workflow <name> --run-id <identifier> (--pid <harness-pid> | --ttl <duration>) --repo-root <path> [--json]
planar-agent run end       --run-id <identifier> --status completed|failed|interrupted [--json]
planar-agent run heartbeat --run-id <identifier> --ttl <duration> [--json]

# Run-scoped working-memory (context_records) — plan 585 task 3901.
# Workers holding a run-associated claim write records via `context add`;
# anyone can read via `context list`; `context resolve` drives the
# active → consumed|superseded lifecycle (stage-close compaction calls these
# primitives). `run_id`, `stage`, `session_id`, and `claim_id` are stamped
# server-side from the claim row (decision 447) — the caller never sets them.
# `kind` must be one of: finding | risk | artifact | followup | summary | capsule.
# For a `capsule` kind, `--compiled-from <id,id,...>` records provenance back
# to the raw record ids that were distilled (decision 446).
# Records are append-only — no uniqueness constraint per Q599.
planar-agent context add     --claim <token> --kind <kind> --body <text> [--compiled-from <id,...>] [--json]
planar-agent context capsule --run <run-id> --stage <s> --body <text> [--compiled-from <id,...>] [--session <id>] [--json]
planar-agent context list    --run <run-id> [--stage <s>] [--status active|consumed|superseded] [--kind <k>] [--json]
planar-agent context resolve --status consumed|superseded (--id <record-id> | --run <run-id> --stage <s>) [--json]

# Dispatch authorization — the two-phase routing handshake that writes
# routing_dispatch_previews and routing_dispatch_snapshots (the
# routing_dispatch_events ledger is read by `planar models evals`; nothing in
# this tree appends to it). `preview` records the resolved candidate plus the packet / profile /
# policy / capability digests it was resolved against and mints a
# single-use token; `confirm` spends that token before --expires-at, and
# refuses if any currently observed digest, candidate, or policy version
# differs from the preview (drift is a refusal, not a warning). Every flag
# is an opaque value the orchestration layer supplies; Planar compares and
# never interprets them. Run `planar-agent dispatch preview --help` /
# `dispatch confirm --help` for the full flag set.
planar-agent dispatch preview --work-item <id> --project <id> --validation-policy <v> --routing-policy <v> --profile-rule <v> --vendor <s> --role <s> --tier small|medium|large --work-type <t> --complexity bounded|standard|high-risk --packet-digest <d> --profile-digest <d> --policy-digest <d> --capability-digest <d> --candidate <row-id> --host <id> --class fallback|default|override|declared_experiment --evidence-state evidential|observational --expires-at <rfc3339> [--task <id>] [--experiment <id>] [--claim <token>] [--claim-status <s>] [--json]
planar-agent dispatch confirm --token <preview-token> --dispatch-key <key> --now <rfc3339> --packet-digest <d> --profile-digest <d> --policy-digest <d> --capability-digest <d> --candidate <row-id> --vendor <s> --role <s> --tier <t> --work-type <t> --complexity <c> --validation-policy <v> --routing-policy <v> [--claim <token>] [--claim-status <s>] [--reviewer <disposition>] [--decision confirmed|overridden] [--json]

# Host-wide build and test queue (plan 1080). Everything after `--` is the
# command; it runs in the caller's directory with the caller's environment.
# See "Queue verbs" below.
planar-agent queue run  [--detach] [--timeout <duration>] [--wait-timeout <duration>] [--label <text>] [--vendor <name>] [--role <name>] [--claim <token>] [--notices] -- <command> [args...]   # cli-lint-ignore: `--` is the argument terminator, not a flag
planar-agent queue cancel [--vendor <name>] [--role <name>] <seq>
planar-agent queue status <seq> [--json]
planar-agent queue wait <seq> [--timeout <duration>] [--json]
planar-agent queue rule

# `version` prints the binary version; `schema` dumps the flat JSON catalog.
planar-agent version
planar-agent schema
```

**Duration grammar:** `--ttl`, `--stale-after`, and `--interval` accept either a bare integer (interpreted as seconds for the `--ttl` / `--stale-after` surface; `--interval` follows the same default for back-compat with the legacy parser) or a number with an ISO-style suffix: `ns`, `us`, `ms`, `s`, `m`, `h`. Examples: `--ttl 600` (10 minutes), `--ttl 10m` (same), `--ttl 1h`, `--interval 500ms`. The implementation is the shared `cli.duration` helper.

**Terminal-verb envelope for a plan or plan_step claim (task 7118):** `release` is the only terminal verb that accepts a plan or plan_step claim (`complete`, `fail` and `block` refuse it with `ClaimNotOnTask`). Such a claim holds no task, so the response names none: `--json` emits `{"ok":true,"claim_token":"...","claim":{...},"task":null}` and the human line is `ok entity:<kind>:<id> claim_status:released`. A task claim's envelope (`"task":{...}`, `ok task:<id> status:<s> claim_status:<s>`) is unchanged. Before this fix the response looked up the task whose id equalled the plan's id and reported that unrelated row.

**`planar-agent heartbeat --ttl` semantics (task 6093):** Omitting `--ttl` **renews the lease length the claim currently holds** — a heartbeat on a claim taken with `--ttl 8h` sets the new expiry to eight hours from now. It never shortens the lease it was sent to preserve. Passing `--ttl` sets the lease absolutely from now, in either direction, so a deliberate re-TTL (longer or shorter) is still available; a subsequent bare heartbeat then renews *that* new length. The renewed length is derived from the stored `(last_heartbeat_at, lease_expires_at)` pair, which already encodes the current TTL — there is no stored-TTL column and no migration involved.

Previously `--ttl` carried a hardcoded `600` default, so an omitted flag was indistinguishable from `--ttl 600` and silently cut a long lease to ten minutes. That made a faithfully-heartbeating long dispatch *more* likely to lose its claim than one that never heartbeated at all.

**`planar-agent heartbeat --status <text>` (plan 467 M1):** When `--status` is provided, `heartbeat` inserts a closed `heartbeat`-kind `agent_actions` row with the text in the `summary` column alongside the lease refresh. This makes current activity visible in `planar-watch ps` (`activity:"<summary>"` text column) and `planar-watch feed`. When `--status` is omitted no action row is written (pre-M1 behavior preserved). The payload is capped at **256 bytes**; oversize values exit with `InvalidInput`. An explicit `--status ""` (empty string) writes an action row with an empty summary — distinct from omission.

### Queue verbs

`planar-agent queue` is the host-wide build and test queue (plan 1080). One queue per user per host serves every project, so a build or test one agent starts does not run on top of another's. There is no daemon: the process that submits a command is the process that runs it.

The foreground form is `queue run` with optional `--timeout <duration>`, `--wait-timeout <duration>`, `--label <text>`, `--vendor <name>`, `--role <name>`, `--claim <token>` and `--notices`, then the argument terminator and the command with its arguments (`queue run [--timeout <duration>] [--wait-timeout <duration>] [--label <text>] [--vendor <name>] [--role <name>] [--claim <token>] [--notices] <terminator> <command> [args...]`):

1. It checks the command before touching the configuration or the store, in this order. **The command guard**: a command whose program is on the model-launcher list is refused at exit **2**, with one `error: queue: ...` line on standard error that names the program. **Resolution**: a program that cannot be found is refused at exit **127**, and a program that exists but cannot be executed (a file without the execute bit, or a directory) at exit **126**, each with one `error: queue: ...` line naming the program. None of the three creates an entry, a ticket or a history row, and the command is never started; the caller learns of them from the exit code of `queue run` itself. See [the command guard](#the-queue-command-guard).
2. It reads the `[queue]` configuration ([the `[queue]` table](#the-queue-table)). A configuration it cannot use refuses at exit **125** before anything is enqueued.
3. It opens the queue's store, which is `planar.db` (`PLANAR_DB`, else `~/.planar/planar.db`), and inserts an entry in state `waiting`. The queue domain is exempt from the pre-dispatch path check, so the verb resolves the path itself: with neither `PLANAR_DB` nor `HOME` in the environment it refuses at exit **125** with one `error: queue: ...` line (every other `planar-agent` verb exits 1 there). The file is opened as an existing file only: `queue run` never creates `planar.db`, so on a host where `planar init` has not run it refuses at **125** and names `planar init`. It never migrates the file, and it runs the version handshake before anything else, in this order. **Behind** (the file's `schema_migrations` version is below this binary's): refused at **125** with `schema version <S> in <path> is older than this binary's <N>; run \`planar init\``. **Equal or ahead**: the queue's own compatibility check (`queue_schema`, `queue_entries` and `queue_history` exist, the marker's newest row admits this binary, and every column this binary names exists) decides. An ahead `planar.db` whose queue tables this binary can still use is **used as it is**, so agents on the host keep queueing builds and tests while a newer build has migrated the shared file; when the check fails it refuses at **125**, naming the queue versions or the missing column (`queue_schema_incompatible`: ahead, a newer build fixes it) or, at an equal version, the missing table or column and that this database's migration is not this binary's (`queue_schema_foreign`: two branches shipped different migrations under one number, which a newer build alone will not fix). A file that cannot be opened or is not a database, a path that is a directory, and a lock that outlasts the five-second bound refuse at **125** too. Every refusal is exactly one `error: queue: <reason>; <remedy>` line, the command is never run directly as a fallback, and `planar.db` is never created. Inserting the entry also deletes history rows older than `[queue] history_days` and their log files. Sequence numbers start above 1,000,000 (the queue migration seeds the counter), so a `PLANAR_QUEUE_SLOT` left over from the old numbering names no entry.
4. It polls at `[queue] poll_interval` until the entry is among the first `slots` live entries by arrival order, then marks it `running`. Each poll reads `slots`, `poll_interval`, `stale_after` and `grace` again, so an edit to `config.toml` takes effect on the next poll. A configuration that cannot be read during the wait keeps the previous settings and is reported once on standard error.
5. It runs the command in the caller's working directory, with the caller's environment plus `PLANAR_QUEUE_SLOT` set to the entry's sequence number, and with the caller's standard streams. Standard output carries the command's own output and nothing else, with or without `--notices`. Standard error carries the command's own output too, plus queue notices only when `--notices` is given, and otherwise nothing else, except the `error: queue: ...` lines and the `warning: queue: ...` lines, which are written with or without `--notices`: a warning is written when the queue itself hit a failure it could not act on (for example a stopping signal that failed, a terminating entry that could not be ended, or a configuration that could not be reloaded; the `--notices` paragraph below lists more).
6. While the command runs it refreshes the entry at each poll, so an entry whose submitter is alive stays live.
7. When the command ends it removes the entry and writes its one `queue_history` row in a single transaction, then exits with the command's status: its own exit code, or `128` plus the signal that terminated it.
8. It catches SIGINT, SIGTERM and SIGHUP for the whole submission and never leaves the entry behind. **While waiting**, a signal removes the entry with outcome `cancelled` (the canceller's `pid` is the process that sent the signal, read from the signal's `si_pid`, or the submitter's own pid when the sender is unknown, as for a signal the kernel raised; the canceller's `vendor` and `role` are always the submitter's own, because a signal cannot carry them), the command is not started, and the submitter exits **125**, the queue's cancelled exit, because the command never ran (`128` plus N is for a command a signal ended), with one `error: queue: ... was interrupted by <signal> before its turn; the command was not run` line on standard error. A signal that arrives while the turn is being taken is treated the same way; only a signal in the short window between that check and the start of the command is forwarded to the command instead. The entry behind it takes its turn as usual. **While the command runs**, the signal is forwarded to the command's process group and the submitter keeps supervising: it neither marks the entry terminating nor escalates. What happens next is the command's choice. A command that dies of the signal ends the entry `signaled` and the submitter exits `128` plus the signal; one that traps it and exits ends the entry `exited` with the command's own code, which the submitter passes through; one that carries on keeps its slot until it ends, or until its run limit stops it, and a second signal is forwarded again. A signal that was ignored when the submitter started (`nohup`, or a shell's background job for SIGINT) stays ignored and is not forwarded. Only the process group recorded for the command is ever signalled. A signal received while a stopped command's process group drains (after a run limit or a cancellation has begun the stop, once the command's first process has been reaped) is not forwarded: the reaped process's group id is no longer proof of the group, and the stop's own SIGKILL already follows once the `[queue] grace` period has passed, so a forwarded signal would add nothing.

`command` is the argument vector, given after `--`. `--label` is stored with the entry and shown in listings.

`--vendor <name>` and `--role <name>` name the submitting agent, and are stored with the entry and its `queue_history` row so the operator can see which vendors and roles use the queue. Each is the flag when it is given and not empty, else the environment variable `PLANAR_VENDOR` or `PLANAR_ROLE` when it is set and not empty, else it is stored empty (no value; listings show it as absent). Planar does not guess either value, and neither flag has a format rule beyond that: the text is stored as given. The values carry over to a rejoined entry (see below) and to a nested run, which takes its own flags, or the inherited environment, since a nested command's environment is the submitter's.

`--claim <token>` makes the submitter renew that claim at half its lease interval, from its first poll until its command ends: at every wait tick while the entry waits, and at every tick while the command runs. A renewal is the transaction `planar-agent heartbeat --claim <token>` runs without `--ttl` or `--status`, so the lease keeps the length it has, no action row is written, and an engine-supervised claim is refused as it is for any caller. The lease length is read from the claim at the first renewal, which is made at once, and again after every renewal, so a lease re-set with `heartbeat --ttl` sets the next cadence, so an unusable token or main database is reported at the start rather than when the lease is about to lapse. Claims live in the same `planar.db` as the queue, but they keep `planar-agent`'s **exact-version rule** while the queue itself tolerates an ahead database, so the renewal opens its own bounded connection, only when `--claim` is given, lazily, never creates the file (an absent file is a failed renewal) and never migrates it, and it refuses a schema on either side of the binary's. **Against an ahead `planar.db` every renewal therefore fails:** the submitter prints one `warning: queue: cannot renew the claim: ...; the command is not affected` line and the command still runs with its own exit status, but if the wait plus the run outlasts the claim's lease the claim lapses, and the agent's next heartbeat or terminal verb fails with `ClaimNotActive`. During a migration cycle run `queue run --claim` with the head binary (the build that migrated the file), or leave `--claim` out and heartbeat separately with it. A **failed renewal never stops the command** and never changes its exit status: an unresolvable, absent, schema-locked, ahead or busy database, an unknown, ended or lapsed claim, and an engine-supervised claim each write one `warning: queue: cannot renew the claim: <why>; the command is not affected` line to standard error (to the output file of a detached run), with or without `--notices`, once per distinct reason, and are tried again after the shorter of the renewal interval and five seconds. The database is opened as an existing file only (a file that goes missing is a failed renewal, never a created one), without touching its journal mode, and a lock on it is waited for at most one second per statement from the first statement on; an attempt that times out stops there, so a locked database costs the submitter about one second per attempt. A detached submitter renews; the invoked process returns its ticket before the database is opened and does not. A nested run renews the claim it is given on its own schedule. The token is recorded with the queue entry and is never printed.

`--notices` writes queue notices to standard error, one line each, in the form `queue: entry <seq> <what happened>`: `waiting at position <n>` (the entry's place among the waiting entries, counting from 1, when it first waits and each time its place changes; a rejoined entry says it again under its new number), `started`, and last, the outcome: `exited with code <n>`, `terminated by signal <n>`, `stopped at its run limit`, `cancelled`, `cancelled before its turn`, `removed at its wait limit`, `not started`, or, for an entry that ended without this submitter or that the queue gave up on, a short truthful text (`cancelled`, `ended as <outcome> without this submitter`, `ended without a history row`, `abandoned, rejoin limit reached`, `abandoned, could not be polled`, `abandoned, cannot observe the command's status`, `ended: cannot rejoin the queue`, `ended: store busy while rejoining`, `ended: cannot read the clock`). An entry whose start failed for a reason other than a missing or unexecutable program ends `abandoned` and says `abandoned, could not be started`. A notice describes what this submitter observed of its command, while `queue status` holds the entry's record, and the two can differ: a running entry that another process removed shows `exited with code <n>` in the notice but `abandoned` in its history. Every exit after the entry exists ends with one of these lines, written after any `error: queue: ...` line. The last line on standard error therefore names the sequence number and how the entry ended. Standard output is never written by the queue, with or without the flag, and without `--notices` the queue writes nothing to standard error on the happy path, so the stream is byte-identical to a direct run. `started` is written once the command has been spawned, so it is not ordered against the command's own output: the command's first bytes on standard error may come before it. The `warning: queue: ...` diagnostics of a degraded path (a failed signal, a configuration that could not be reloaded, a rejoin, a store that could not be reached) and the `error: queue: ...` lines are the documented exception: they are written with or without `--notices`.

`--timeout <duration>` is the **run limit**, default `30m`: how long the command may run once it has started. The limit is recorded on the entry as its deadline when the entry starts. At the deadline the submitter marks its own entry terminating with reason `timeout` and sends SIGTERM to the command's process group; if the group still has members once the `[queue] grace` period has passed, it sends SIGKILL. The entry is removed only when the group is empty, so a stopped command keeps its slot until it is gone: after the command's first process is reaped, the submitter keeps advancing the stop (sending the SIGKILL once the grace period has passed) until the engine has ended the entry, and only then exits. If a member survives SIGKILL for `[queue] grace` plus 15 seconds, the submitter exits anyway and leaves the entry, still live while its group has members, for the next poll of any submitter to end. The entry ends with outcome `timeout` and the submitter exits **124**, whatever signal ended the command. The same holds when another process marked the entry (a cancellation ends it `cancelled`, exit **125**); a stopped command is never recorded as `signaled`. **When the entry cannot carry the limit** the submitter stops the command itself, directly: SIGTERM at the deadline it holds, SIGKILL once `[queue] grace` has passed, both to the process group it started, and only while that group's leader still has the start time the submitter read when it started the command (a group id that came to belong to another process is never signalled; the submitter says so on standard error instead). That is the case in two situations. If the store refuses to record the command's process group on the entry, the submitter retries at the poll interval, five more times; an entry that still names no group cannot be signalled by any other process, so at the limit the submitter stops the command as above and then ends the entry itself with outcome `timeout`, so the history and the exit **124** agree (a helper the command left behind in its group is not chased in that case). And if the entry is removed while the command runs, the submitter keeps its own deadline and does the same; the row the removing process wrote stays the only history row, and the submitter exits **124** and says on standard error that the entry is no longer in the queue and the limit is still enforced.

`--wait-timeout <duration>` is the **wait limit**, default none: how long the entry may wait for its turn. The limit is recorded on the entry (as `wait_deadline_mono`) when it is enqueued. A submitter whose turn has not come when the limit passes removes its entry with outcome `wait_timeout`, does not run the command, and exits **125** with one `error: queue: ...` line. An entry whose turn comes at the limit runs; the wait limit never cuts short a command that has started.

Both flags take the [`[queue]` duration grammar](#the-queue-table): an integer immediately followed by `ms`, `s`, `m` or `h`, at most `24h`. The flags refuse what a configuration key would not need to: zero, a negative value, a bare integer and an unknown unit are refused at exit **2** with an `error: queue: run: <flag>: ...` line, before the configuration or the store is touched, so nothing is enqueued. (An unknown flag or a flag with no value is a parse failure, exit **1**.) `queue run` with no command is a parse failure: exit **1**, nothing enqueued.

A command that passes both checks can still fail to start at its turn: the program vanished or lost its execute permission while the entry waited, or it is a file with the execute bit but no interpreter line, which passes resolution and fails when it is executed. The entry then ends with outcome `not_started` and one `queue_history` row (exit code **127** for a missing program, **126** otherwise), the submitter exits with that code and writes `error: queue: cannot start '<program>': ...` on standard error, and the next entry takes its turn.

The exit code passes the command's status through, so it is ambiguous by design (a command may exit `125` itself); `queue_history` records how each entry ended. The queue's own failure is exit **125** with one `error: queue: ...` line on standard error. `queue run` has no `--json`: standard output belongs to the command, so no envelope is written on any path.

**Detached runs (`--detach`).** `queue run --detach` (with the flags above, then the argument terminator and the command) returns a ticket at once and leaves the waiting, the run and the history to a detached submitter, so a harness that stops long foreground commands can submit a build and observe it with a finite `queue wait` budget. The refusals of step 1 (the command guard at exit **2**, resolution at **127** and **126**, and an invalid `--timeout` or `--wait-timeout` at **2**) still happen in the invoked process, before anything is forked, so a refused command detaches nothing, creates no log file and prints no ticket. Everything after that is done in a child, in this order:

1. The invoked process creates a pipe and forks, before it reads the configuration or opens the database. The fork is made only while the process has a single thread; a second thread aborts the process (an internal invariant) rather than risk a deadlocked child.
2. The child starts a new session (`setsid`), so it survives its caller's process group being killed, closes every descriptor it inherited except the pipe (all of them, however high their number: `close_range` on Linux, a listing of the open descriptors on macOS), and reads standard input from `/dev/null`.
3. The child reads the configuration, opens `planar.db` as `queue run` does (so every refusal of step 3 above is reported through the pipe, once) and inserts the entry, recording its own process id and start time as the submitter's.
4. The child creates `<planar-db-directory>/queue-logs/<seq>.log` exclusively (mode `0600`, in a directory created with mode `0700`; a log directory that already exists must be owned by the current user and not writable by group or others, and is otherwise refused as a failure to create the log, never tightened or written into; a file that already exists, left by an earlier run before the sequence counter went back, is never appended to or removed and counts as a failure to create the file: the refusal names the file and the "Host-queue rollback recovery" section of `migrations/README.md`, whose `scripts/queue-logs-after-reset.py` archives such logs instead of deleting them). **The log directory follows the database file name:** for a database named `planar.db` it is `queue-logs/` beside it, which is the default location and unchanged; for any other file name it is `<stem>.queue-logs/` beside it, where the stem is the file name with its final extension removed (`other.db` gives `other.queue-logs/`, `scratch.sqlite` gives `scratch.queue-logs/`, `a.b.db` gives `a.b.queue-logs/`, and `queuedb` or `.hidden`, which have no extension, give `queuedb.queue-logs/` and `.hidden.queue-logs/`). Every database starts its counter at 1,000,001, so two databases in one directory would otherwise collide on `1000001.log` on their first detached runs. **Accepted residual:** `x.db` and `x.sqlite` in one directory share `x.queue-logs/`; their detached runs can collide on the same `<seq>.log`, and the second create fails under `O_EXCL` and refuses at **125** without overwriting anything. Rename one of the two databases; do not run the counter-reset helper, which could archive logs the other database's runs are still writing. records the path on the entry (the `log_path` column of the entry and of its history row) and points its standard output and standard error at the file. If the file cannot be created, the child deletes the entry it inserted, writes no history row, and reports the failure.
5. The child writes the sequence number and the path to the pipe and closes it.
6. The invoked process reads the pipe to its end. On success it prints two lines on standard output, the **sequence number** and then the **log path**, and exits **0**. On a failure the child reports, it prints that error (one `error: queue: ...` line) and exits **125**. When the pipe closes with nothing written (the child died first) it prints `error: queue: no ticket was issued: the detached submitter ended before it reported` and exits **125**. It does not wait for a child that has gone, and it writes nothing else to standard output.

From then on the child is the submitter of the ordinary run described above (wait, run, finish, history), with the command's standard output and standard error going to the log, byte for byte and in order, `--notices` lines and every `error: queue: ...` and `warning: queue: ...` line included, and its standard input empty. The exit status of a detached run is not returned to anyone: it is in the `queue_history` row. The log is deleted with the history row that names it. A submitter that cannot hand its ticket over because the invoked process has already gone removes its entry and the log and exits, so no run exists that nobody can name. Signals: the detached submitter is in its own session, so a signal sent to the invoking process or its group does not reach it, and a terminal hangup does not either; stop one by signalling the submitter's process id (the entry's `pid`), which it forwards to the command as for a foreground run.

**Nested runs.** A queued command that itself calls `queue run` does not wait behind its own entry. The variable `PLANAR_QUEUE_SLOT` in the command's environment carries the entry's sequence number; a `queue run` that finds it naming a live running entry inserts its own entry in state `running`, with `parent_seq` set to that number, outside the slot count and the arrival order, and runs the command at once. The nested entry has its own sequence number, its own deadline (its own `--timeout`, or the 30-minute default, not what is left of the parent's), and its own `queue_history` row with `nested` set and the parent's sequence number. A nested command sees its own sequence number in the variable, so a third level nests under the second. The inner exit code passes through as for any run. `--wait-timeout` has nothing to bound on a nested run and is ignored.

The variable is advisory. A value that is not a positive integer, that names no entry (never existed, or already ended), that names an entry still waiting, that names an entry that is not live, or that names an entry recorded on another host identity (or on an unknown one), is ignored: the command queues normally and is not marked nested. A nested run nests only under an entry on the submitter's own host identity, however fresh the other entry is: nesting asserts that the parent's command started this run, which can only hold on the parent's host, so a marker naming an entry on another host identity, or one recorded with an unknown identity (or when this host's own identity is unknown), queues normally. A session can copy a real sequence number of an entry on its own host to skip the queue, and a long-lived process a queued command left running keeps the variable until its parent ends; both are accepted, and the history makes them visible. If the store stays busy past its timeout while the nested entry is inserted, the insert is retried at the poll interval for as long as `[queue] stale_after`, then refused at exit **125** with `error: queue: the store stayed busy ...; the command was not run`. It is not queued normally, because it would then wait behind the entry that waits for it, and it is not run unqueued, because nobody checked the marker. Any other failure of the insert is refused at exit **125** at once.

**A submitter whose own entry goes missing** reads the history row for its sequence number. A *waiting* submitter whose row says `abandoned` (another submitter reaped it while it was stopped, for example by `SIGSTOP` or a suspended laptop) rejoins the queue: it inserts a new entry, which has a higher sequence number and so waits behind everything that arrived meanwhile, and records the new number as the old row's `successor_seq`, in one transaction, so `queue status` can follow the chain. The new entry keeps the original `--wait-timeout` deadline, so the limit bounds the whole wait. A submitter rejoins at most three times and then exits **125**; the command is not run. A row saying `cancelled` (or any other outcome), or no row at all (never written, or pruned by retention), exits **125** without running the command. A *running* submitter whose entry disappears keeps supervising its command, never rejoins and never runs the command a second time, and exits with what it observed of the command. Its run limit is still enforced: the submitter holds the deadline itself and stops the command directly at it (see `--timeout` above), exits **124**, and says once on standard error that the entry is no longer in the queue. A waiting submitter that cannot complete any poll for longer than `stale_after` ends its own entry as `abandoned` (no exit code, signal, start time, run time or successor, since the command never ran) and exits **125**.

**Reading a ticket (`queue status`).** `planar-agent queue status <seq> [--json]` reports what became of one entry. The sequence number is the first line of the ticket `queue run --detach` prints. It answers from the entry while it is in the queue and from its `queue_history` row afterwards, and it is the authoritative record of how a command ended: the exit code of `queue run` is ambiguous by design, `outcome` is not.

`queue status` **reads and changes nothing.** It opens `planar.db` read-only at the SQLite layer (the file's mode does not matter, and a read-only file and directory work), so it cannot reap, refresh, mark or end an entry, migrate the store, or create one that does not exist (SQLite may leave its `-shm` WAL-index sidecar next to the store; it holds no store content). It runs the same version handshake and queue compatibility check as `queue run`, and so reads an ahead database whose queue tables it can use. It judges liveness by the rules a poll applies (a waiting entry is live when its submitter's process exists, its start time matches the recorded one and the entry was refreshed within `stale_after`; a running entry is also live while its child group has members) and reports the verdict without acting on it: an entry whose submitter is gone reports `live` false and stays in the store until a poll reaps it. It resolves the path itself, so with neither `PLANAR_DB` nor `HOME` it exits **125** like the other queue verbs.

The `--json` output is one object with exactly these fields, in this order. A field that does not apply is `null`.

| Field | Meaning |
|---|---|
| `seq` | The sequence number asked for |
| `state` | `waiting`, `running` or `ended` |
| `live` | Whether the entry passes the liveness rules; `null` when ended (or when a process query failed) |
| `position` | Place among the waiting entries, counting from 1; running entries do not count |
| `outcome` | The history outcome (`exited`, `signaled`, `timeout`, `cancelled`, `wait_timeout`, `not_started`, `abandoned`), when ended |
| `exit_code`, `signal` | How the command ended |
| `terminating` | The reason (`timeout` or `cancelled`), while the entry is being stopped |
| `cancelled_by` | An object with `vendor`, `role` and `pid` (`vendor` and `role` are `null` when the canceller gave none), when cancelled, in history or while being stopped |
| `superseded_by` | The sequence number that replaced this one, when it was re-enqueued |
| `nested`, `parent_seq` | Nesting |
| `cwd`, `argv`, `label`, `vendor`, `role` | As submitted; `argv` is an array |
| `log_path` | The output file, for a detached run |
| `enqueued_at`, `started_at`, `ended_at` | Wall-clock milliseconds |
| `waited_ms`, `ran_ms` | Durations: for an entry still in the queue, up to now |
| `run_limit_ms`, `wait_limit_ms` | The limits in force, in milliseconds: `wait_limit_ms` is the `--wait-timeout` the entry was submitted with (`null` without one); `run_limit_ms` is its `--timeout` (default 30 minutes), recorded when the entry starts and `null` while it waits. An ended entry reports the values its history row copied. Both also read `null` wherever an older build (agent schema version 2, which has no limit columns) did the writing: a limit is `null` when such a build enqueued the entry (`wait_limit_ms`), started it (`run_limit_ms`), or ended it, including a reap by its poll (both, on the history row); and both are `null` for every entry and row while the store itself is still at version 2, which `queue status` reads without migrating it |
| `slots`, `grace_ms` | The `[queue]` settings in force now, for an entry still in the queue; `null` when ended, or when the `[queue]` configuration cannot be used |

**A successor is followed.** When the number asked for was reaped while its submitter was stopped and the submitter rejoined the queue (see above), its history row names the new number. `queue status` then describes the new entry (its `state`, `position` and so on, and its end once it has one), sets `superseded_by` to the new number and keeps `seq` as the number asked for. A chain of rejoins is followed to its end; a successor whose history was pruned ends the chain at the last row that exists.

Without `--json` the same answer is printed as `key: value` lines, one per field that applies, in the order above (`cancelled_by` reads `vendor=<v> role=<r> pid=<n>`, `argv` is a compact JSON array). A value with a control character, a Unicode format character (bidirectional override, zero-width mark, line or paragraph separator), a byte that is not valid UTF-8, a backslash or a leading double quote is shown double-quoted with `\n` / `\u00xx` / `\u202e`-style escapes, so a quoted value is never mistaken for an unquoted one; a `label`, `vendor` or `role` (and the canceller's) is cut, ending with `…`, once its escaped form would pass 48 columns (counted as for `planar-watch queue`). `--json` is never cut and carries the exact bytes.

**Exit codes of `queue status`** (decision 1188 fixes the queue's own codes; it does not list this verb's, so these follow this binary's table):

| Code | Meaning |
|---|---|
| 0 | The entry was found; the answer is on standard output |
| 1 | No entry and no history row has that sequence number (never issued, or its history was pruned), or no sequence number was given (a parse failure, as everywhere in this binary) |
| 2 | The argument is not a positive integer |
| 125 | The queue failed: `planar.db` does not exist or cannot be read, is behind this binary, has queue tables this binary cannot use, or an internal error |

**An unusable `[queue]` configuration does not fail `queue status`.** The configuration is read only for an entry still in the queue. When it cannot be used (a refused value, or a file that cannot be read or parsed), the answer is still printed and the exit is 0: `slots` and `grace_ms` are `null`, liveness is judged against the default 30-second staleness window (no value from the unusable table is trusted, including a `stale_after` that parsed), and one line `warning: queue status: the [queue] configuration cannot be used (<reason>); ...` is written on standard error.

A refusal writes `error: queue status: <message>` on standard error. With `--json` it also writes one object on standard output, `{"error":{"verb":"queue status","tag":"<tag>","message":"<message>"}}` (plus `"seq"` for exit 1), so a script reads the reason without parsing prose. The tags are `not_found` (exit 1), `invalid_input` (exit 2), and, all at exit 125, `store_unreachable` (no path, a missing file, an unopenable path or a file that is not a database), `store_unreadable` (a query failed or a lock outlasted the bound), `internal`, `schema_version_behind` (`planar.db` is older than this binary: run `planar init`), `queue_schema_incompatible` (`planar.db` is ahead and its queue tables are not usable by this binary) and `queue_schema_foreign` (the version is equal but the queue tables or a column differ: two branches shipped different migrations under one number). Neither of the last two is `schema_version_ahead`, which means exit 7 elsewhere, because their remedies differ: a newer build fixes the first and not the second.

#### Waiting for a logical ticket (`queue wait`)

`planar-agent queue wait <seq> [--timeout <duration>] [--json]` observes the logical job named by a detached ticket. `<seq>` is a positive integer within signed 64-bit range, normally the first line printed by `queue run --detach`. The observer follows recorded successors and stops at a final history outcome or an observation reason. Its `--timeout` is a **separate observation budget**: a positive integer followed by `ms`, `s`, `m` or `h`, default `30m`, maximum `24h`. Zero, negative, fractional, missing-unit and overflowing values exit 2. The monotonic budget starts before the read-only store open and covers lock waits, queue backlog, command runtime, reads, successor traversal and sleeps. It never resets when a successor appears. For a long gate, allow for both backlog and runtime, for example `queue wait <seq> --timeout 3h` after a submission with `queue run --timeout 2h` when backlog may take an hour.

The three time limits have different owners: `queue run --wait-timeout` bounds how long a submitted entry waits for a slot; `queue run --timeout` bounds the command after it starts (default `30m`); `queue wait --timeout` bounds only this observer. `queue wait` does not renew a task claim. The detached submitter can renew a caller-supervised claim when `queue run --claim <token>` was supplied; an agent whose observer stops must still honor its claim ritual. Waiting is read-only: it cannot reap, cancel, rejoin, signal, refresh, migrate, alter history or logs, or stop the submitted command. Interruption by SIGINT/SIGTERM stops only the observer. A timeout or interruption is safe to follow with another `queue wait` on the **same original sequence number**; neither is reason to resubmit the build.

The JSON result is one object with `seq` (requested), `observed_seq` (latest resolved successor or `null`), `wait_reason`, `elapsed_ms`, `timeout_ms`, `result_exit_code`, `status` (the existing [`queue status`](#queue-verbs) object or `null`) and `error` (a tagged `queue wait` error or `null`). Unavailable fields are `null`, including on invalid input. Text prints stable `key: value` lines with the reason, recorded status and error tag when present; errors also go to standard error. Neither form includes job log contents. `result_exit_code` is the process exit code, not proof of a command outcome. Interpret it together with `wait_reason`, `status.state`, `status.outcome`, `status.exit_code` and `status.signal`:

| `wait_reason` / recorded outcome | Exit | Meaning |
|---|---:|---|
| `completed` / `exited` | recorded child exit, including 124 or 125 | The command ran and exited with that code. |
| `completed` / `signaled` | 128 + recorded signal | The command ended by signal. |
| `completed` / `timeout` | 124 | The queue stopped the command at its run limit. |
| `completed` / `cancelled` or `wait_timeout` | 125 | The queue recorded cancellation or expiry of the slot wait. |
| `completed` / `not_started` | recorded 126 or 127 | The command could not start. |
| `timed_out` | 124 | This observer reached its budget; the job's final outcome is unknown. |
| `interrupted` | 130 (SIGINT) or 143 (SIGTERM) | Only this observer stopped. |
| `stalled` | 125 | The same active sequence was observed `live: false` twice at least one second apart; the job's outcome is unknown. |
| `history_unavailable` | 1 | The requested ticket is missing, a previously observed ticket vanished, or an explicit successor has no entry or history row. |
| `error` | 2 for invalid input; 125 for store, schema, read, clock or internal errors | Inspect `error.tag` and `error.message`; a refused observation gives no command verdict. |

An active `waiting` or `running` entry remains pending while `live` is true **or null**, including when its `terminating` marker is set. `terminating` alone is not a final history outcome. A confirmed `live: false` result is `stalled`, not a reap or completion; a later observation of the same ticket is safe. An ended `abandoned` row with no successor can acquire one in a later rejoin transaction, so observation continues until its deadline and then returns `timed_out` with the latest abandoned status. If an explicit successor is missing or its history was pruned, the result is `history_unavailable`, not success. A broken `[queue]` configuration warns once and uses the default staleness window; slots and grace remain unknown. A behind, incompatible or foreign queue schema and an unreadable store fail with a tagged error. Unlike `queue status`, `queue wait` distinguishes a missing explicit successor from an abandoned row that has not acquired one.

Exit 124 can mean an observer deadline, recorded run timeout, or child exit 124. Exit 125 can mean an observer failure or stall, recorded cancellation or wait timeout, or child exit 125. Never infer refusal or success from either exit code alone. An empty log, absent active row or `live: false` is also not completion evidence; use the recorded history outcome. A queue read refusal or missing history calls for diagnosis, not an unqueued build or automatic duplicate submission.

#### Printing the agent rule (`queue rule`)

```
planar-agent queue rule
```

`queue rule` prints the rule that tells an agent to send its builds and tests through the queue, so the operator can paste it into a project's own agent guide. It writes the text to standard output, byte for byte, and exits **0**. It takes no argument and no flag (there is no `--json`; the text is Markdown, and it begins with the `##` heading "Builds and tests go through the host queue").

The text is one authored file, `src/lib/queuerule/queue-rule.md`, embedded into `planar-agent` at build time. It holds:

- what counts as a build or test command, with examples, and what does not;
- the detached submit and status polling procedure; the bounded `queue wait` recipe is being adopted in the agent guidance alongside this reference contract;
- the instruction to pass `--vendor` and `--role`;
- the instruction, for an agent that holds a task claim, to pass `--claim <token>` to `queue run` (detached and foreground) so the queue renews the claim while the entry waits and runs, in place of renewing it by hand between polls;
- what to do on each outcome `queue status` reports;
- the instruction to stop and report when a queue command exits **125**, and that the command must not then be run directly;
- what a non-zero exit from the detached submit means (no ticket was issued) and the route for each code;
- the check for a Planar without the queue (`planar-agent queue rule >/dev/null` exits non-zero), the fallback of running the command directly and telling the operator that Planar needs upgrading, and a table that sets this apart from exit 125. The check is `queue rule`, not `--help`: an older Planar answers an unknown command's `--help` with its general help and exits 0, so a `--help` check would pass where there is no queue. `queue rule` exits non-zero there and opens no database.

`queue rule` **opens no database**, and is the one queue verb that needs no `planar.db`. It reads no environment variable and no configuration file, so it prints the same text with `PLANAR_DB` unusable and with `HOME` and `PLANAR_DB` unset, and it creates nothing on disk.

#### Known limits

These are accepted, and stated here so they are not rediscovered.

- **A queued command cannot read the terminal.** The command runs in a process group of its own, never the terminal's foreground group, so a read from the terminal stops it with SIGTTIN (a write to it may stop it with SIGTTOU when the terminal sets `tostop`). The queue does not hand the terminal over and does not redirect standard input: the command inherits the submitter's standard streams as they are. Queued commands are builds and tests, which do not read the terminal; a command that prompts belongs outside the queue. Run with standard input from a file, a pipe or `/dev/null` (as agent harnesses and CI do) and the limit never arises.
- **A signal to a group of exited processes is refused on macOS.** `kill(-pgid, sig)` fails with `EPERM` for a process group whose every member has exited without being reaped (and for the few milliseconds a killed member spends being torn down), so a SIGKILL sent to a stopped command's group can come back "not permitted" for a group with nothing left to stop. `queue run` and `queue cancel` read that refusal as an empty group, and stay silent, only when the kernel's own process list shows every member of the group in that state; any other `EPERM` (a group owned by another user, for one) is still reported as `warning: queue: SIGKILL to entry <seq>'s process group failed: not permitted`. Linux never answers this way, so nothing changes there.
- **A helper a command leaves in its process group is treated differently in three cases.** While the submitter is alive, the entry ends when the command's own first process exits, and the helper is left alone and holds nothing. When the submitter is gone, the helper keeps the entry live and holds the slot until the deadline, and is then stopped, SIGTERM and then SIGKILL after the grace period, by whichever submitter next polls. While an entry is being stopped, it is removed only once the group, helper included, is empty.
- **A deadline is enforced only by a process that is polling.** Nothing runs when no submitter is polling, so an orphaned command past its deadline keeps running until the next `queue run` polls, or until `queue cancel` stops it. The queue has no daemon to do it sooner.
- **An entry reaped while its submitter is alive, or whose command ends while the store cannot be reached, is recorded as `abandoned` whatever the command's exit status.** The submitter still exits with the command's own status, so the exit code and the history row can disagree; `queue status` is the record.
- **Freshness is compared across host identities as if they shared a clock.** A container on macOS runs in a virtual machine with a clock unrelated to the host's, so its entries cannot be judged correctly from the host, or the host's from it. An entry whose host identity differs from the checker's is judged by freshness alone, its process ids are never tested, and `queue cancel` refuses to signal it.
- **Time the host spends asleep does not count against a limit.** Every compared time comes from a monotonic clock (`CLOCK_UPTIME_RAW` on macOS, `CLOCK_MONOTONIC` on Linux) that stops while the host sleeps, so waking a laptop does not time out a build or stale every entry. The wall-clock columns (`enqueued_at`, `started_at`, `ended_at`) are for display and do advance, so a command that slept through a night can show a run time longer than its limit.
- **A command that moves its work into a new session or process group looks finished when its original group is empty,** while that work still runs. The entry is then removed and the slot is released.
- **A detached submitter survives its caller's process group being killed, not a harness that kills by container or control group.** `--detach` starts the submitter in a session of its own, which protects it from a `kill(-pgid)` of the caller's group. A harness that stops everything in a container, a systemd unit or a cgroup stops the submitter too, and its entry is then reaped as `abandoned`.
- **A command may exit with a code the queue also uses** (124, 125, 126, 127 or 128 plus a signal number). The exit code of `queue run` does not say which produced it; `--notices` names the outcome on the last line of standard error, and `queue status` is the authoritative record.
- **When the store never recorded a running entry's process group, a helper left behind is not stopped.** Recording the group is retried at the poll interval a few times. If it never succeeds, no other process can signal the command, so the submitter stops it at the deadline itself and ends the entry as `timeout` once the command's first process exits. A helper left in the group after that is not stopped, because nothing can then confirm the group still belongs to the command.

#### The queue command guard

`queue run` refuses a command that would start a model, because a model launcher queued behind builds and tests holds a slot for as long as a conversation lasts. The list is short and purpose-built (it is not derived from `planar-execute`'s host-function deny list): `claude`, `codex`, `gemini`, `copilot`, `aider`, `opencode` and `cursor-agent`.

- The guard matches the **basename** of the program, whole and without regard to ASCII case: `/usr/local/bin/claude`, `claude` and `Claude` are refused, while `codex-lint-report` and `claude_fixture` are queued, because a name that only contains a listed word is a different program. Case is ignored because macOS's default volumes are case-insensitive, where looking up `Claude` on `PATH` runs `claude`; the same folding applies to `env`. The refusal names the listed spelling (`claude`).
- Before choosing the program the guard skips leading `NAME=value` assignments and a leading `env` (by basename) with its options and the assignments after it: the flags that need no value (`-i`, `-0`, `-v`), the ones that take a value, in their separate, attached and long forms (`-u NAME`, `-C DIR`, `-P PATH`, `-a NAME`, FreeBSD's `-L USER` and `-U USER` (macOS and GNU `env` reject them, so a command using one fails inside `env` before anything runs; the guard consumes their value anyway, which is harmless), and the long `--unset`, `--chdir` and `--argv0`), clustered short options such as `-iu NAME`, and the `--` that ends the options. The string given to `env -S` (or `--split-string`) is split on white space, honouring quotes, and read as the start of the command. The skipping repeats, so `env env claude` is refused too. A command that is only `env`, with nothing after it to run, has no program and is not refused.
- The guard is a denylist and looks no deeper: a launcher run through `sh -c`, an interpreter or a wrapper script of your own is not seen.

A refusal exits **2**, writes `error: queue: refusing to queue '<program>': it is a model launcher` on standard error, and enqueues nothing. The full exit-code table for `queue run` is [Queue run exit codes](#queue-run-exit-codes).

#### Queue run exit codes

Foreground `queue run` exits with the command's own exit status (decision 1188). The codes below belong to the queue; each is checked against the built binary by `make exit-code-contract`.

| Code | Meaning | When |
|------|---------|------|
| `1` | **Parse failure**: an unknown flag, a flag with no value, or no command given. | Before anything is enqueued. |
| `2` | **Refused input**: a command the guard refuses (a model launcher), or an invalid `--timeout` / `--wait-timeout` value. | Before the configuration or the store is touched. |
| `124` | The command was **stopped at its run limit** (`--timeout`), whatever signal ended it. | The entry ends `timeout`. |
| `125` | The **queue failed**: `planar.db` unreachable (neither `PLANAR_DB` nor `HOME` set, a missing file, a directory, not a database), behind this binary (`schema_version_behind`: run `planar init`), or ahead or equal with queue tables this binary cannot use (`queue_schema_incompatible`, `queue_schema_foreign`); configuration unusable, wait limit reached, cancelled before its turn, or an internal error. Also returned when a handler passes through a status outside 0..255. | The command was not run (except an internal error after it started). |
| `126` | The command was found and **could not be executed**. | The entry ends `not_started` if it had been enqueued. |
| `127` | The command was **not found**. | The entry ends `not_started` if it had been enqueued. |
| `128` + N | The command was **terminated by signal N** (`143` for SIGTERM), capped at 255. | The entry ends `signaled`. |

Any other value is the command's own exit status, passed through unchanged (`7` is `7`); the status of a command that exits `0` is `0`. Because a command may itself exit with any of the values above, the exit code alone is ambiguous: `queue status <seq>` is the authoritative record (its `outcome` field says whether the command ran), and with `--notices` the submitter's last line on standard error names the sequence number and the outcome. A status outside 0..255 cannot be reported by a process, so the dispatcher refuses it with one `error:` line and exits `125`.

#### Cancelling an entry (`queue cancel`)

```
planar-agent queue cancel [--vendor <name>] [--role <name>] <seq>
```

`queue cancel` cancels the entry with sequence number `<seq>`. Any caller that can open the store may cancel any entry, whether or not it is the entry's submitter and whether or not that submitter is alive; the canceller is recorded, not checked. A *detached* entry has no terminal to interrupt, and is cancelled the same way. A nested entry is cancelled like any other; its parent is not touched.

- **A waiting entry** is removed in one transaction, and its `queue_history` row is written with outcome `cancelled`. Its submitter finds that row at its next poll, does not run the command, and exits **125** (with `--notices` its last line is `queue: entry <seq> cancelled`).
- **A running entry** is stopped in two steps: a transaction marks it terminating with reason `cancelled` and the canceller, and only after that commits does cancel send SIGTERM to the command's process group. Cancel does not leave the rest to a later poll. It then advances that one entry itself, at short intervals, until the group is empty: once the entry has been terminating for the `[queue] grace` period and the group still has members it sends SIGKILL, and it removes the entry (writing the history row, outcome `cancelled`) only when the group is empty, so the next entry cannot start on top of a command that is still running. Cancelling an entry that is already terminating (a run limit, or an earlier cancel) changes no marker and records no second canceller; it sends SIGKILL at once if the grace period has already passed. The submitter of a running entry that is alive sees the reason and exits **125** too; if it is gone, cancel alone ends the entry.
- **A group not yet recorded.** If the entry's submitter has not yet recorded the command's process group when the cancel marks it, nothing can be signalled at that moment; cancel keeps looking and sends the SIGTERM as soon as the group is recorded. If cancel itself is killed before that, no SIGTERM is ever sent, and only the SIGKILL of a later poll of any process, once the grace period has passed, stops the command.
- **The wait is bounded** by the grace period plus 15 seconds. A group that still has members after that (a process that survives SIGKILL, or one whose group was never recorded) leaves the entry marked terminating, with its slot, for the next poll of any process to finish, and cancel exits **125** and says which it was.
- **The canceller** is stored in `cancelled_by` as `{"vendor": ..., "role": ..., "pid": ...}`: `--vendor` and `--role` as for `queue run` (the flag when given and not empty, else `PLANAR_VENDOR` / `PLANAR_ROLE`, else empty; Planar does not guess them) and the process id of the cancelling process.

On success cancel writes one line to standard output (`cancelled entry <seq>: removed before its turn`, or `cancelled entry <seq>: its command was stopped`, with `(SIGKILL after the grace period)` when it had to send one). Refusals write one `error: queue: cancel: ...` line to standard error. `queue cancel` has no `--json`.

#### Queue cancel exit codes

Each code is checked against the built binary by `make exit-code-contract` where a case can run without a live process; the rest are pinned by the black-box cases in `src/cmd/planar-agent/queue_cancel.t.cpp`.

| Code | Meaning | When |
|------|---------|------|
| `0` | The entry was **cancelled**: a waiting entry removed, or a running entry stopped and ended. | The history row has outcome `cancelled` (or, for an entry already stopping at its run limit, the outcome that stop names). |
| `1` | **No such entry**: no entry has that number and there is no history row for it (never issued, or pruned by retention). A missing argument is a parse failure and also exits `1`. | Nothing is written. |
| `2` | **Refused input**: `<seq>` is not a positive integer. | Before the store is touched. |
| `6` | The entry has **already ended**; the message names its outcome. Also returned when cancel marked a running entry but its own submitter ended it with what its command did (`exited`) before reading the marker: the cancellation did not take effect. | Cancel writes nothing (the entry's one history row is the submitter's). |
| `125` | The **queue failed**: `planar.db` is unusable (unreachable, `schema_version_behind`, `queue_schema_incompatible` or `queue_schema_foreign`, as for `queue run`) or the configuration is unusable, the entry's process group was not empty after SIGKILL and the bounded wait, or the entry belongs to another host identity, whether this call marked it or it was already terminating (it is refused at once; a poll on that host will stop it). | The entry may be left marked terminating. |

### Atomic operation transaction shapes

| Verb       | Transaction body |
|------------|------------------|
| `pull`     | SELECT next eligible task → INSERT `agent_work_claims (status=active)` → UPDATE `tasks.status='doing'` → INSERT `agent_actions`. Returns `{ok, no_work, claim_token, claim, task, action_id}`. No writes on no-eligible-task path. |
| `complete` | Verify claim active → UPDATE action ended_at + outcome='ok' → UPDATE task status='done' → UPDATE claim status='completed'. |
| `fail`     | Same as complete with outcome='error', task status='todo', claim status='aborted', and `failure_category` set from `--category` (default `unknown`). |
| `release`  | Same as fail with outcome='aborted', claim status='released'. (Distinct semantically from fail — "graceful give-up" vs "I tried and failed".) |
| `block`    | INSERT `entity_links(from=task, to=blocker, relationship='depends-on')` → UPDATE task status='blocked' → UPDATE action ended_at + outcome='aborted' → UPDATE claim status='released'. |
| `peek`     | Read-only: same SELECT as step 1 of pull; no writes. |
| `reconcile`| (1) Claim sweep: SELECT expired active claims → UPDATE status='stale' and optional operator-supplied `failure_category`; a claimed task in `doing` returns to `todo` only when the claim has an ownership action and no active replacement. Direct-claim `claim_check` markers are closed as aborted; the existing ended-session sweep closes other orphaned actions. (2) Run sweep: SELECT running `workflow_runs` rows. A pid-bound row (`pid` set) is probed with `kill(pid,0)` → ESRCH ⇒ dead. A pid-less row (decision D11, task 6847) is NEVER pid-probed — it is dead only once `expires_at` has lapsed. Either way, dead ⇒ mark `abandoned` + set `ended_at`. Both sweeps run inside the same `BEGIN IMMEDIATE` transaction. `--dry-run` returns candidates + run_candidates without writing. `--plan <id>` scopes both sweeps to claims/actions/runs belonging to the given plan. |
| `abort`    | UPDATE claim status='aborted' + released_at + release_reason + optional operator-supplied `failure_category` → restore `doing` → `todo` only for a default direct task claim carrying its transactional `claim_check` marker → close that marker as aborted → INSERT audit `agent_actions` row naming the aborting session. Primitive `--no-transition` and pull claims retain their previous task-status behavior. |

All write verbs open `BEGIN IMMEDIATE` so the writer lock blocks any concurrent claim attempt on the same row. The status-transition guard (`policy.status.check`) is consulted before each `UPDATE tasks SET status` — refusal rolls the transaction back and the claim keeps its previous state.

Failure categories are terminal observations, not retry policy. Systemic
capacity handling is implemented by the caller through
`planar-execute run workflows/parallel-dispatch.lua --phase
capacity_reconcile --args <json>`, not by a `planar` provider-capacity verb.
That deterministic phase consumes supplied lane outcomes and emits a
provider-scoped breaker/recovery packet; it performs no claim write, provider
call, spawn, abort, reconcile, or persisted breaker reset. Use
`planar-watch claims --plan <id> --status all --json` to inspect claims,
`planar-agent reconcile --plan <id> --dry-run --json` to preview stale recovery,
and `planar report --json` for aggregate `{provider,category,count}` rows.

### JSON shapes

Stable across versions; new keys may be added, existing keys do not change name or type without a migration. Full reference: `docs/architecture.md` § "JSON shapes" and the tech-spec at `~/.planar/workbench/project_planar/p85-agent-activity/58-agent-activity-tracking-tech-spec.md`.

| Verb          | Shape |
|---------------|-------|
| `pull`        | `{ok, no_work, claim_token?, claim?, task?, action_id?}` |
| `peek`        | `{ok, no_work, task?}` |
| `complete` / `fail` / `release` / `block` | `{ok, claim_token, claim, task}` |
| `claim` / `heartbeat` | `{ok, claim_token, claim}` |
| `action start` / `action end` | `{ok, action_id, action}` |
| `ingest`      | `{ok, sessions_created, claims_created, actions_created, events_processed}` |
| `reconcile`   | `{ok, claims_marked_stale, actions_closed, runs_abandoned, candidates?, run_candidates?}` (`candidates` + `run_candidates` present only with `--dry-run`) |
| `abort`       | `{ok, claim_token, claim, aborting_session}` |
| `run start`   | `{ok, run_id, run}` where `run` includes `id`, `plan_id`, `workflow_name`, `run_identifier`, `pid` (number or `null` for a pid-less lease-supervised run), `expires_at` (string or `null`), `repo_root`, `status:"running"` |
| `run end`     | `{ok, run_id, status}` where `status` is the terminal status written |
| `run heartbeat` | `{ok, run_id, expires_at}` — the run's new lease deadline |

`ClaimRow` matches the `agent_work_claims` row shape with snake_case keys (including nullable `failure_category`, locality columns `repo_root`, `branch`, `head_sha_at_claim`, `dirty_at_claim`, worktree columns `worktree_id`, `worktree_path`, and workflow run correlation columns `run_id`, `stage`). `Task` matches `planar task show --json`. `ActionRow` matches `agent_actions` (including locality columns `head_sha`, `dirty`).

### `planar-agent reconcile` flags

`reconcile` sweeps expired claims, orphaned actions, and dead workflow run pids. All flags are optional; without filters the sweep is global.

| Flag | Description | Default |
|------|-------------|---------|
| `--dry-run` | Report candidates without writing. Returns the same JSON shape as a live run, with `candidates` and `run_candidates` arrays populated. | off |
| `--stale-after <duration>` | Additional grace period beyond the lease expiry before marking a claim stale. Accepts bare integer (seconds) or a suffixed duration (`10m`, `1h`, `500ms`). Useful for giving agents a small window to heartbeat after lease expiry before the reconciler fires. | `0` |
| `--plan <id>` | Scope both the claim sweep and the run sweep to claims/actions/runs belonging to the given plan id. When omitted, the sweep is global (all plans). | (global) |
| `--category <value>` | Record a closed failure category on every claim made stale by this sweep. Accepted values: `usage_limit`, `context_limit`, `output_limit`, `tool_failure`, `validation`, `unknown`. Dry-run never writes it. | (null) |
| `--json` | Emit a JSON result object. | off |

**JSON shape:**
```json
{"ok":true,"claims_marked_stale":2,"actions_closed":1,"runs_abandoned":0}
```

With `--dry-run`:
```json
{"ok":true,"claims_marked_stale":0,"actions_closed":0,"runs_abandoned":0,"candidates":[{"claim_token":"...","claimed_at":"..."}],"run_candidates":[]}
```

---

### Engine supervision (plan 1033 D3/D4)

A claim is **caller-supervised** until `claim-associate --supervisor engine
--attempt <id>` hands it to the engine supervisor (decision 1007). A
caller-supervised claim — every claim nobody associates — takes every verb
exactly as before, and a claim never goes back: `--supervisor caller` on an
engine claim is refused.

On an **engine-supervised** claim the engine alone extends the lease and
issues the one terminal verb, acting as `--as engine --attempt <id>` with
the attempt the claim is associated with:

| Verb | Caller (no `--as`) | `--as engine --attempt <associated>` | `--as engine`, other attempt |
|---|---|---|---|
| `heartbeat` | only with `--status` and no `--ttl`: records the status, lease **unchanged** | extends the lease | `AttemptMismatch` |
| `complete` / `fail` / `release` / `block` | `SupervisorMismatch`, unless `--override-supervisor` | lands once; a repeat after it landed is exit 0 with no change | `AttemptMismatch` |
| `claim-associate --supervisor engine` | same attempt: no-op; new attempt: moves the claim to it | — | — |

**Recovery leaves engine work to the engine** (task 6489). `reconcile`
skips expired engine-supervised claims and dead-pid `engine = 'centurion'`
runs — their recovery is the Centurion host's — and reports them: JSON gains
`skipped_engine_claims` / `skipped_engine_runs`, text gains a `skipped
(engine-supervised; ...)` block, both emitted only when something was
skipped, so a caller-only sweep prints exactly what it always did. `abort`
on an engine claim is `SupervisorMismatch`. `--override-supervisor` on
either takes the work over and logs one `supervisor_override` action per
engine claim. `planar-watch claims` renders an expired engine claim that
reconcile left alone as `status:lapsed (engine)` (it appears under
`--status stale` and `--status all`).

`--as engine` on a caller claim is `SupervisorMismatch`. Every refusal is
`error: <verb>: <Tag>` at exit 1 and leaves claim, task and lease untouched.
Flag misuse is refused before anything is written, at exit 2: `--as engine`
without `--attempt`, `--attempt` without `--as engine`, `--override-supervisor`
with `--as engine`, and `claim-associate` with neither `--run` nor
`--supervisor`.

Each supervised write leaves one closed `agent_actions` row:
`claim_associate` (association or a new attempt), `claim_terminal` (the
engine's terminal verb), `supervisor_override` (an operator override). These
kinds are written only by these verbs — `action start --kind` refuses them
and `pull --role` treats them as unknown (falls back to `coder`).

### Locality flags

`pull`, `claim`, and `action start` accept:

- `--repo-root <path>` — absolute path of the checkout to probe locality against. Falls back to the process cwd when omitted.
- `--no-locality-probe` — short-circuit; records locality columns as NULL.

Per-action-kind defaults: planner / coder / reviewer / test_coder probe; heartbeat / tool_call skip. The probe runs `git symbolic-ref`, `git rev-parse HEAD`, and `git status --porcelain` against the resolved root; any subprocess failure (non-git directory, missing `git`, error exit) records `unknown` rather than refusing the claim.

### Workflow run correlation flags (`pull` and `claim`)

`pull` and `claim` accept two optional flags for associating a claim with an external workflow harness run (decision 450). Decision 1007 (plan 1033) permits the run these flags correlate against to be driven by an external workflow harness, a host-native workflow, or a background agent rather than only the model orchestrator — see `agents/methodology.md` § Worktrees and `docs/concepts.md` § Worktree.

- `--run <run-id>` — integer id of the `workflow_runs` row to link on the claim. Set by the external harness when dispatching a worker inside a run. Omit for interactive operator claims (leaves `run_id` NULL on the row).
- `--stage <stage>` — free-text stage name (e.g. `code`, `review`, `plan`) recorded on the claim. Requires `--run`; omitting `--stage` while passing `--run` leaves `stage` NULL. The `context add --claim <token>` verb (task 3901) stamps `run_id` and `stage` server-side from the claim row — the worker passes only `--claim` (decision 447).

Claims acquired without `--run`/`--stage` behave byte-for-byte as before (no behavior change, no default values). The columns are nullable; existing callers and tools that do not pass these flags are unaffected.

### Entity-ref parser (`claim --entity`)

`--entity <ref>` accepts `task:<id>` / `plan:<id>` / `plan_step:<id>`. Other prefixes are rejected with a non-zero exit. Missing colon or non-integer id is rejected the same way. The strict parser matches the tech-spec § "claim --entity <ref> parser" contract.

### Exit codes

| Code | Meaning |
|------|---------|
| `0`  | Success. |
| `1`  | Operational failure (atomic op rolled back, invalid state, missing claim token), and every **parse failure** — unknown flag, missing required flag or argument, a non-integer where an integer is declared. |
| `2`  | Invalid input raised by a handler (e.g. an `--entity` value that is not a valid entity ref). |
| `7`  | Schema version mismatch — DB older than this binary's embedded minimum, or newer than its embedded max. Remediation: run `planar init`. |
| `64` | Not implemented yet (reserved for future verbs). |

### Capability boundary

A process invoked as `planar-agent` writes only to `agent_work_claims`,
`agent_actions`, `workflow_runs`, and `context_records`, plus `tasks.status`
inside atomic coordinated operations with status guards, and, in `planar.db`
itself, `queue_entries` and `queue_history` (through `queue run` and `queue cancel`). It
never writes plan, decision, question, scenario, artifact, annotation, or
feedback-triage rows. A vendor hook configured with only `planar-agent` on its
PATH therefore has a bounded planning-state blast radius. `queue run` also
executes the command its caller names; the queue is a coordination aid, not a
security boundary.

---

## Binary: `planar-watch`

`planar-watch` is the human-facing **read-only viewer** for live agent activity. Third of Planar's now-five binaries to be added (plan 85 M8). See `docs/architecture.md` § "Five-binary architecture" for the binary split. Note: `planar-watch` is the scriptable NDJSON streaming viewer; `planar explore` is reserved for launching Planar Explorer and is not yet implemented (bare `planar` prints the root help) — see [Domain: `explore`](#domain-explore).

Schema-version handshake: `planar-watch` is a **consumer** of the schema, not its owner. Startup queries `schema_migrations.max(version)` and refuses with exit **7** when the live DB is older than the binary's embedded minimum (same code `planar-agent` uses; remediation message "run `planar init`").

### Capability invariant

A process invoked as `planar-watch` performs **no writes**. Two defenses:

1. The command tree (`src/cmd/planar-watch/`) registers exactly ten read verbs — `feed`, `ps`, `claims`, `actions`, `plans`, `log`, `tree`, `run`, `sync-events`, `queue` — plus the conventional `version` / `completion` / `schema` helpers. There is no write verb anywhere in the tree.
2. The bootstrap calls `db::connection::open_read_only` which opens the DB via `sqlite3_open_v2(..., SQLITE_OPEN_READONLY, ...)`. The SQLite driver itself returns `SQLITE_READONLY` on any attempted `INSERT` / `UPDATE` / `DELETE` / DDL — verified by the `a read-only connection refuses a write` unit test in `src/lib/db/db.t.cpp`. The `queue` verb reads a second store, the agent database, through the same kind of handle (`planar.db.agentdb::open_agent_db_read_only_at`); `capability.t.cpp` asserts that handle refuses a write and that running the verb leaves the store's bytes and modification time unchanged.

A vendor hook or operator script configured with only `planar-watch` on its PATH cannot modify the database under any circumstances.

### Verbs

```
# Cross-cutting activity feed (default invocation; `planar-watch` with no
# args routes here).
planar-watch              # alias for `planar-watch feed`
planar-watch feed     [--follow]  [--vendor <v>] [--plan <id>] [--task <id>] [--since <ISO>] [--limit N] [--tail N] [--json] [--interval <D>]

# Snapshot: active (and stale) claims.
planar-watch ps       [--follow]  [--vendor <v>] [--plan <id>] [--stale]  [--sort-by heartbeat|lease] [--group-by role|scope|vendor] [--json] [--interval <D>]

# Claim ledger (active | stale | all buckets).
planar-watch claims   [--follow]  [--vendor <v>] [--plan <id>] [--status active|stale|all] [--json] [--interval <D>]

# Action ledger (filterable by kind / entity / plan / task).
planar-watch actions  [--follow]  [--vendor <v>] [--kind <k>] [--entity <kind:id>] [--plan <id>] [--task <id>] [--limit N] [--json] [--interval <D>]

# Plans with in-flight work.
planar-watch plans    [--follow]  [--in-flight-only] [--json] [--interval <D>]

# Per-entity / per-claim history (union of actions + claim transitions).
planar-watch log      (--task <id> | --plan <id> | --entity <kind:id> | --session <id> | --claim <token>) [--limit N] [--json]

# Orchestrator → sub-agent topology forest (M4 addition, plan 467).
planar-watch tree     [--root-session <id>] [--follow] [--interval <D>]

# Workflow run observability (plan 585 addition).
planar-watch run list [--plan <id>] [--status running|completed|failed|interrupted|abandoned] [--arm wf|op|all] [--json]
planar-watch run show <id> [--json]

# Sync-event ledger (plan 638 addition — read-only view over sync_events).
planar-watch sync-events [--plan <id>] [--system <slug>] [--entity <kind:id>] [--outcome <value>] [--since <ISO8601>] [--limit <n>] [--json]

# Host build and test queue (plan 1080 — read-only view over the queue tables in planar.db).
planar-watch queue [--json]
planar-watch queue history [--since <duration>] [--json]

# Conventional helpers.
planar-watch version
planar-watch completion <bash|zsh|fish>
```

`--follow` (default off) turns each subcommand into a streaming view: the initial snapshot prints, then new events append as the underlying tables change. M8 ships **Tier 1** of the wake-tier ladder (poll every `--interval`, default `1s`; sub-second intervals available for tests). The watermark column set and the JSON event shape are part of the public contract — Tier 2 (kqueue / inotify on the SQLite `-wal` file) lands in a follow-up without changing either.

`--plan <id>` widens past the literal `entity_kind='plan'` match: feed / ps / claims / actions all return events whose entity is the plan itself, OR a task on the plan, OR a plan_step on the plan. This is what the operator means by "show me plan N" — task-on-plan events are usually the only ones a session actually generates.

### `planar-watch ps` — M3 flag additions (plan 467)

| Flag | Description | Default |
|------|-------------|---------|
| `--sort-by heartbeat\|lease` | Sort order for active claims. `heartbeat` (default) orders by `last_heartbeat_at desc` — freshest-heartbeated agent first. `lease` restores the pre-M3 behavior: `claimed_at desc`. Null `last_heartbeat_at` sorts to the end. Invalid values exit with `InvalidValue`. | `heartbeat` |
| `--group-by role\|scope\|vendor` | Group active (and, with `--stale`, also stale) rows by a dimension. Text output emits one `[group: <key>]` section header per distinct key, with matching claim lines beneath. JSON wraps claims in `"groups": {"<key>": [...]}` instead of the flat `active`/`stale` envelope. When absent, the pre-M3 `active`/`stale` envelope shape is preserved (strict backward compat). Invalid values exit with `InvalidValue`. | unset (no grouping) |

**M3 text columns** (appended after `scope:`, before or after `vendor:` in the order below):

| Column | Format | Source |
|--------|--------|--------|
| `activity:"<summary>"` | Quoted string; truncated at 80 bytes with `…` (U+2026). Empty quotes `""` when no action exists. | Most-recent `agent_actions.summary` for the claim. |
| `worktree:<basename>` | Basename of `worktree_path`. Prefixed with `…` when the full path exceeds 40 chars. Empty quotes `""` when `worktree_path` is null. | `agent_work_claims.worktree_path`. |
| `last_hb:<rel>` | Relative time (e.g. `15s`, `2m`, `just now`) produced by the `relativeTime` helper; the trailing ` ago` suffix is stripped. Empty string when `last_heartbeat_at` is empty. | `agent_work_claims.last_heartbeat_at`. |
| `category:<value>` | Appended only when the claim carries a closed terminal `failure_category`; null-category rows retain the previous column sequence unchanged. | `agent_work_claims.failure_category`. |

Full text column order (M3): `<entity>:<id>  scope:<label>  activity:"<summary>"  vendor:<v>  branch:<b>  worktree:<basename>  sha:<8-char>  last_hb:<rel>  [category:<value>]  token:<tok>`. `planar-watch claims` uses the same categorized-only addition before its `token:` column. JSON claim rows always carry nullable `failure_category` additively.

Implementation: `ps()` in `src/cmd/planar-watch/handlers/shared/live.cpp` (tasks 3053–3058).

### `planar-watch feed` — M3 flag addition (plan 467)

| Flag | Description | Default |
|------|-------------|---------|
| `--tail <N>` | Emit only the most-recent N events on the initial snapshot (the `journalctl -f -n` idiom). Must be a positive integer (`N > 0`); `N ≤ 0` exits with `InvalidValue`. Composes with `--follow`: the tail is emitted first, then only events newer than the tailed set stream (no re-emit of tailed events). When `--tail` and `--limit` are both given, `--tail` takes precedence for the snapshot cap. | unset (uses `--limit`, default 100) |

### `planar-watch tree` — M4 addition (plan 467)

Renders the orchestrator → sub-agent action forest by walking the `agent_actions.parent_action_id` chain using a `WITH RECURSIVE` CTE. Root actions are rows where `parent_action_id IS NULL`; children are rendered beneath their parent, indented with unicode tree characters.

**Flags:**

| Flag | Description | Default |
|------|-------------|---------|
| `--root-session <id>` | Scope output to the subtree rooted at actions from the named session. Error (exit non-zero) when the session id is unknown or `< 1`. | unset (all sessions) |
| `--follow` | Stream: re-renders on WAL change (Tier-2 wake). | off |
| `--interval <D>` | Maximum poll cadence for `--follow` (e.g. `100ms`, `1s`). | `1s` |

**Text row format (depth 0 = root):**

```
action:<id>  scope:<label>  vendor:<v>  activity:<summary>  worktree:<wt>  branch:<b>  last_hb:<rel>
    └── scope:<label>  vendor:<v>  activity:<summary>  ...
```

Each row shows the same columns as `ps`: `scope`, `vendor`, `activity`, `worktree`, `branch`, `last_hb`. When the action has no linked claim the row shows `session:<id>  (no claim)` instead of the claim columns.

**Tree characters** (unicode):

| Context | Characters |
|---------|-----------|
| Non-last child | `├── ` (U+251C U+2500 U+2500 + space) |
| Last child | `└── ` (U+2514 U+2500 U+2500 + space) |
| Vertical guide (ancestor still open) | `│   ` (U+2502 + 3 spaces) |

**Choosing `tree` vs `ps --group-by`:** `ps --group-by role` is the flat-by-role view — use it when each claim's identity (role, vendor, heartbeat recency) is the question. `tree` is the topology view — use it when the orchestrator→coder dispatch fanout is the question (e.g. "which sub-agents did orchestrator A dispatch?"). When fanout density exceeds what `--group-by` makes readable (≥ 3 orchestrators each with multiple coders), prefer `tree`.

**Implementation:** `tree()` in `src/cmd/planar-watch/handlers/shared/live.cpp` (plan 467 M4, tasks 3064–3067).

### `planar-watch run` — workflow run observability (plan 585)

Read-only view of run tables. `list` covers both the `workflow_runs` table (context-plane, wf-arm) and the `runs` table (op/workflow-arm, op-arm). `show` drills into wf-arm runs (with context_records); op-arm run detail is via `planar run show <run_uid>`.

**`run list [--plan <id>] [--status <s>] [--arm wf|op|all] [--json]`**

Lists runs from one or both source tables, newest first. All filters combine with AND when supplied.

| Flag | Description | Default |
|------|-------------|---------|
| `--plan <id>` | Restrict to runs associated with the given plan id. | unset (all plans) |
| `--status <s>` | Restrict by run status: `running`, `completed`, `failed`, `interrupted`, `abandoned`. | unset (all statuses) |
| `--arm <a>` | Source table filter: `wf` (workflow_runs / context-plane runs written by `planar-agent run start`), `op` (runs table / op-arm runs written by `planar run start`), `all` (both). | `all` |
| `--json` | Emit a single JSON object `{generated_at, runs:[RunRow]}`. | false (human text) |

Each `RunRow` in the JSON output carries a `source` field (`"wf"` or `"op"`) indicating which table the row came from, followed by `engine` (plan 1033 task 6493): `"embedded"` or `"centurion"` for a `wf` row (a pre-migration-00038 row reads `"embedded"`), `null` for an `op` row, whose table has no engine. `plan_id` is `null` for a workflow run bound to no plan (allowed since migration 00038) — never `0`. The text line gains `engine:<e>` after `source:` (`-` for an op row) and prints `plan:-` for a plan-less run. For `wf`-source rows, `planar-watch run show <id>` provides context_records. For `op`-source rows, `planar run show <run_identifier>` provides journal events.

**`run show <id> [--json]`**

Shows the full `workflow_runs` row for `<id>` (wf-arm only) plus all `context_records` for that run, grouped by stage (alphabetical ascending) then ordered by `created_at` ascending within each stage. Exits non-zero when the run id is not found.

| Flag | Description | Default |
|------|-------------|---------|
| `--json` | Emit `{run: RunRow, context_records: [ContextRecord]}`. | false (human text) |

Human text format for `run show`: prints run metadata (id, plan_id, status, pid, workflow, `engine:<e>`, timestamps, identifier, repo_root), followed by context records indented under `[stage: <name>]` section headers. The `body` field is previewed at up to 80 bytes with `…` when truncated.

**Implementation:** `src/cmd/planar-watch/handlers/run/run.cpp` (plan 585, task 3906; op-arm inclusion added task 4349; engine and nullable plan task 6493).

**Engine supervision in `claims` and `feed` (plan 1033 task 6493).** Every claim object `claims --json` and `feed --json` emit carries `"supervisor":"caller"|"engine"` and `"attempt_id"` immediately after `stage` — a claim never handed to the engine reads `"supervisor":"caller","attempt_id":null`, the keys are never omitted. `ps`, `log` and `planar-agent`'s payloads keep the lean claim object. In text, an engine claim's `claims` line gains `supervisor:engine  attempt:<id>` before `token:` (a caller claim's line is unchanged), an expired engine claim reads `status:lapsed (engine)`, and a `feed` line for one of the supervision action kinds (`claim_associate`, `claim_terminal`, `supervisor_override`, `run_submitted`, `run_reconciled`) appends the kind.

### `planar-watch sync-events` — per-row view over sync_events (plan 638)

Read-only, filterable window into the `sync_events` table. Returns rows ordered by `at` descending.

**Synopsis:**
```
planar-watch sync-events [--plan <id>] [--system <slug>] [--entity <kind:id>] [--outcome <value>] [--since <ISO8601>] [--limit <n>] [--json]
```

**Flags:**

| Flag | Description | Default |
|------|-------------|---------|
| `--plan <id>` | Restrict to events whose `external_links` row belongs to the given plan id. | unset (all plans) |
| `--system <slug>` | Restrict to events via a link on the named external system slug. | unset (all systems) |
| `--entity <kind:id>` | Restrict to events via a link on one entity, e.g. `task:42` or `plan:7`. | unset |
| `--outcome <value>` | Filter by outcome value (e.g. `ok`, `conflict`, `error`, `noop`). | unset (all outcomes) |
| `--since <ISO8601>` | Only return rows with `at` >= this timestamp. | unset |
| `--limit <n>` | Cap row count. | 100 |
| `--json` | Emit one JSON object per row (NDJSON). | off |

Filters are combined with AND when multiple are supplied.

**Human output (one row per line):**
```
id    at                         direction  outcome   link_id  detail
15    2026-06-14T10:03:12.000Z   push       ok        7
16    2026-06-14T09:55:00.000Z   pull       conflict  7        status: local=done remote=in-progress
```

**JSON output (NDJSON, one object per row):**
```json
{"id":15,"at":"2026-06-14T10:03:12.000Z","link_id":7,"direction":"push","outcome":"ok","fields_changed":"title,status","detail":null}
{"id":16,"at":"2026-06-14T09:55:00.000Z","link_id":7,"direction":"pull","outcome":"conflict","fields_changed":"status","detail":"status: local=done remote=in-progress"}
```

**Schema effects:** Reads `sync_events`, `external_links`, `external_systems`. No writes.

**Exit codes:**
- `0` — success (empty result is not an error).
- `1` — invalid flag value (non-integer `--plan` or `--limit`, malformed `--entity` reference, invalid `--since` timestamp).

### `planar-watch queue` — the host build and test queue (plan 1080)

Read-only listing of every running and waiting entry of the host-wide queue that `planar-agent queue run` feeds, across every project. It reads the `queue_entries` table of the **main database** (`planar.db`) through the same read-only handle as every other `planar-watch` verb and under its rules: a missing database is the usual open error, and a schema version behind or ahead of this binary exits `7`.

**Synopsis:**
```
planar-watch queue [--json]
```

**What it lists.** Entries are listed in sequence order, running and waiting alike. `POS` is the place among the *waiting* entries from 1; a running entry holds a slot and has none. Each entry is judged by the same liveness rules a queue poll applies, against the `stale_after` window of the [`[queue]` table](#the-queue-table), but the judgement is only reported: this verb never reaps, refreshes or removes an entry, never sends a signal to any process (the liveness check is a signal-0 existence probe, which delivers nothing), and the store's bytes and modification time are unchanged by it. An entry that is not live stays listed and is marked `NOT-LIVE`; a nested entry is marked `nested:<parent seq>`; an entry being stopped is marked `stopping:<reason>`. When the `[queue]` table cannot be used, the default window applies and one `warning: queue: ...` line goes to standard error (the same degradation as `planar-agent queue status`).

**Human output.** A header and one line per entry, columns padded: `SEQ  STATE  POS  NOTES  WAITED  RAN  VENDOR  ROLE  LABEL  DIRECTORY  COMMAND`. `NOTES` is `-` or a comma-joined list of `NOT-LIVE`, `LIVE-UNKNOWN` (the process query failed), `nested:<n>`, `stopping:<reason>`. An empty value is `-`. `WAITED` and `RAN` are `<s>s`, `<m>m<ss>s` or `<h>h<mm>m`. `COMMAND` is the argument vector with each word shell-quoted. An empty queue prints nothing. A value that holds a control character, a Unicode format character (the bidirectional overrides and isolates U+202A-U+202E and U+2066-U+2069, the zero-width and directional marks U+200B-U+200F, U+2060-U+2064, U+206A-U+206F, U+061C, the byte-order mark U+FEFF, and the line and paragraph separators U+2028 and U+2029), a byte that is not valid UTF-8, a space, a double quote or a backslash is written as a double-quoted string with `\n` / `\u00xx` / `\u202e`-style escapes (`\xNN` for an invalid byte), so a submitted label, vendor, role, directory or argument cannot start a line of its own, move the cursor or reorder the text around it. A command word holding a control or format character is escaped the same way: it is then shown double-quoted with those escapes, which is display text and **not a pasteable shell word**. A `VENDOR`, `ROLE` or `LABEL` is cut, and ends with `…`, once its **escaped** form would pass 48 columns (a `\uXXXX` escape counts 6, `\xNN` 4, `\n`, `\\` and `\"` 2, a combining mark 1 although it displays in 0, else its display width), so a value made of invisible or control characters cannot widen a column; it is cut before quoting, so an escape is never split, and the quoted cell adds its two quote characters; `--json` is never cut. Columns are padded by display width, not bytes: a code point counts one column, a wide East Asian or emoji code point two and a combining or format character none. This is an approximation, not a full Unicode width table, so a terminal that measures differently may still show a small misalignment.

```
SEQ  STATE    POS  NOTES     WAITED  RAN    VENDOR  ROLE    LABEL       DIRECTORY  COMMAND
41   running  -    -         0s      3m12s  claude  coder   -           /work/a    make test
42   waiting  1    NOT-LIVE  4m10s   -      codex   tester  "my label"  /work/b    ctest -j 8
```

**JSON output.** One array, `[]` when the queue is empty, of objects with exactly these members in this order; a member that does not apply is `null`:

| Field | Meaning |
|---|---|
| `seq` | The sequence number |
| `state` | `waiting` or `running` |
| `live` | Whether the entry passes the liveness rules; `null` when the process query failed |
| `position` | Place among the waiting entries, from 1; `null` for a running entry |
| `terminating` | The reason (`timeout`, `cancelled`) while the entry is being stopped |
| `nested`, `parent_seq` | Nesting |
| `cwd`, `argv` | As submitted; `argv` is an array |
| `label`, `vendor`, `role` | As submitted |
| `log_path` | The output file of a detached run |
| `enqueued_at`, `started_at` | Wall-clock milliseconds |
| `waited_ms`, `ran_ms` | Durations at the moment of the listing |
| `run_limit_ms`, `wait_limit_ms` | The limits in force; `null` when no limit applies |

**Schema effects:** Reads `queue_entries` in `planar.db`. No writes; the database is never created or migrated.

**Exit codes:**
- `0` — success, an empty queue included.
- `1` — `planar.db` is missing (`error: OpenFailed`) or this process cannot read it (`error: DatabaseUnreadable`, naming the path and SQLite's reason); neither `PLANAR_DB` nor `HOME` is set; a parse failure.
- `7` — `planar.db` is behind or ahead of this binary's schema; both versions are named. The same rule as every other `planar-watch` verb.

**Main database.** There is no exemption: `queue` and `queue history` need the main database's path like every other verb, and exit `1` with `neither PLANAR_DB nor HOME is set` when neither variable is.

### `planar-watch queue history` — what the host queue has run (plan 1080)

Read-only listing of the entries that have **ended**, from the `queue_history` table of the same `planar.db` `planar-watch queue` reads. It answers "what went through the queue, how did it end, and who used it": every row carries the submitting vendor, role, directory and command, so a project that builds and never appears here is not following the rule. `planar-watch queue` (bare) is unchanged and still lists running and waiting entries; `queue` is both a verb and the group `history` sits under.

**Synopsis:**
```
planar-watch queue history [--since <duration>] [--json]
```

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--since <duration>` | Only rows that ended within this long: `ended_at` at or after now minus the duration. An integer followed by `ms`, `s`, `m`, `h` or `d`, greater than zero and at most `36500d`, the history retention maximum. It has its own parser: `planar-agent queue run --timeout` and `--wait-timeout` take no `d` and stop at `24h`. | all rows |
| `--json` | One JSON array. | text |

**Order.** Oldest first: by end time, then sequence number.

**Human output.** A header and one line per row, columns padded: `SEQ  OUTCOME  RESULT  ENDED  WAITED  RAN  NOTES  VENDOR  ROLE  LABEL  DIRECTORY  COMMAND`. `OUTCOME` is `exited`, `signaled`, `timeout`, `cancelled`, `wait_timeout`, `not_started` or `abandoned`. `RESULT` is `code:<n>`, `signal:<n>` or `-`. `ENDED` is UTC, `YYYY-MM-DDTHH:MM:SSZ`. `WAITED` and `RAN` are `<s>s`, `<m>m<ss>s` or `<h>h<mm>m` (`RAN` is `-` for an entry that never started). `NOTES` is `-` or a comma-joined list of `cancelled-by:<vendor>/<role>/<pid>`, `superseded-by:<seq>` and `nested:<parent>`. An empty value is `-`. `COMMAND` is the argument vector with each word shell-quoted. An empty history prints nothing. Values are escaped, cut and aligned exactly as in `planar-watch queue`, including the `VENDOR`, `ROLE` and `LABEL` of a `cancelled-by:` note.

```
SEQ  OUTCOME    RESULT    ENDED                 WAITED  RAN    NOTES                            VENDOR  ROLE      LABEL  DIRECTORY  COMMAND
41   exited     code:0    2026-09-30T14:03:22Z  1s      1m05s  -                                claude  coder     build  /work/a    make test
42   cancelled  -         2026-09-30T14:10:01Z  4m10s   12s    cancelled-by:claude/reviewer/91  codex   tester    -      /work/b    ctest -j 8
```

**JSON output.** One array, `[]` when empty, of objects with exactly these members in this order; a member that does not apply is `null`:

| Field | Meaning |
|---|---|
| `seq` | The sequence number |
| `outcome` | As above |
| `exit_code`, `signal` | How the command ended |
| `cancelled_by` | The canceller as an object with `vendor`, `role` (each `null` when not given) and `pid`, when cancelled |
| `superseded_by` | The successor's sequence number, when the entry was re-enqueued (the name `queue status --json` uses). A row whose entry was reaped and then re-enqueued (`abandoned`) names the new entry here |
| `nested`, `parent_seq` | Nesting |
| `cwd`, `argv` | As submitted; `argv` is an array |
| `label`, `vendor`, `role` | As submitted |
| `log_path` | The output file of a detached run |
| `enqueued_at`, `started_at`, `ended_at` | Wall-clock milliseconds |
| `waited_ms`, `ran_ms` | Durations, from the recorded times |
| `run_limit_ms`, `wait_limit_ms` | The limits in force; `null` when no limit applies |

**Schema effects:** Reads `queue_history` in `planar.db`. No writes; the database is never created or migrated.

**Exit codes:**
- `0` — success, an empty history included.
- `1` — `planar.db` is missing or cannot be read; neither `PLANAR_DB` nor `HOME` is set; a parse failure (unknown flag, missing value).
- `2` — `--since` is not a duration in the accepted grammar or range (an integer plus `ms`, `s`, `m`, `h` or `d`; zero, a bare integer and more than `36500d` are refused); the message names the value and nothing is printed on standard output.
- `7` — `planar.db` is behind or ahead of this binary's schema; both versions are named.

Like `queue`, this verb follows every other `planar-watch` verb's main-database rules: it needs `PLANAR_DB` or `HOME` to locate `planar.db`, and exits `7` on a version mismatch in either direction.

---

### JSON shapes

```
planar-watch feed --json  (NDJSON, one event per line):
  { event: "claim_acquired" | "heartbeat" | "released" | "stale"
         | "completed" | "failed" | "blocked"
         | "action_started" | "action_ended" | "task_status_changed",
    at: ISO8601,
    claim?: ClaimRow,
    action?: ActionRow,
    task?: { id, status_before, status_after } }

planar-watch ps --json (without --group-by):
  { generated_at: ISO8601, active: [ClaimRow], stale: [ClaimRow] }

planar-watch ps --json --group-by <dim>:
  { generated_at: ISO8601, groups: { "<key>": [ClaimRow, ...], ... } }

planar-watch claims --json:
  { generated_at: ISO8601, claims: [ClaimRow] }

planar-watch actions --json:
  { generated_at: ISO8601, actions: [ActionRow] }

planar-watch plans --json:
  { generated_at: ISO8601,
    plans: [{ plan: Plan, in_flight: bool,
              active_claims: int, active_actions: int,
              last_event_at: ISO8601 | null }] }

planar-watch log --json:
  { entity: { kind, id }, entries: [LogEntry] }
  LogEntry is the discriminated union:
    { kind: "action",
      at: ISO8601,
      action: ActionRow }
    { kind: "claim_acquired" | "claim_released" | "claim_completed"
          | "claim_aborted" | "claim_stale",
      at: ISO8601,
      claim: ClaimRow }

planar-watch run list --json:
  { generated_at: ISO8601,
    runs: [RunRow] }
  RunRow: { id, plan_id, workflow_name, run_identifier, pid,
            repo_root, started_at, ended_at: ISO8601|null,
            status: "running"|"completed"|"failed"|"interrupted"|"abandoned",
            source: "wf"|"op" }

planar-watch run show <id> --json:
  { run: RunRow,
    context_records: [ContextRecord] }
  ContextRecord: { id, run_id, stage, session_id, claim_id,
                   kind: "finding"|"risk"|"artifact"|"followup"|"summary"|"capsule",
                   body, status: "active"|"consumed"|"superseded",
                   compiled_from: string|null, created_at: ISO8601 }
  Records are grouped by stage (alphabetical asc) then ordered by created_at asc.
```

`ClaimRow` matches the canonical shape from `engine.runtime.agentactivity.json.writeClaim` (snake_case keys mirroring the `agent_work_claims` columns, including the locality columns `repo_root`, `branch`, `head_sha_at_claim`, `dirty_at_claim` and the worktree columns). `ActionRow` mirrors `agent_actions`. `Plan` matches `planar plan show --json`.

**M3 addition — `ClaimRow.latest_action`:** `planar-watch ps --json` and `planar dashboard --agents --json` each embed a `latest_action` field on every `ClaimRow` returned by those two verbs:

```
latest_action: { kind: string, summary: string, started_at: ISO8601 } | null
```

`null` when no `agent_actions` row exists for the claim. The field is sourced from `latest_action_for_claim()` (implementation: `src/engine/runtime/agentactivity.cpp`). The `feed` and `log` verbs do **not** embed `latest_action` on their claim payloads — they are time-ordered event streams where the action rows are already present as first-class events.

### Exit codes

| Code | Meaning |
|------|---------|
| 0 | Success (and the `--follow` graceful-SIGINT exit). |
| 1 | Generic failure (DB I/O error), and every parse failure — unknown flag, a flag value outside its accepted set. |
| 2 | Invalid input raised by a handler (missing required filter on `log`, an `--entity` value that is not `kind:id`). |
| 7 | Schema version mismatch (DB older than binary's embedded minimum, OR newer than its embedded max); `queue` and `queue history` follow the same rule. `planar-agent` and `planar-ext` fold both directions into `7` the same way; `planar` returns `7` only for a newer schema and `1` for an older one. |

### `--follow` and SIGINT

Each `--follow` verb installs a SIGINT handler that flips an atomic flag. The poll loop checks the flag between iterations and exits cleanly with code **0** on the next tick. Pressing Ctrl-C is "stop watching" — a success outcome, not an error.

---

## Introspection: `schema` (all planning-state binaries)

Every Planar binary — `planar`, `planar-agent`, `planar-watch`, `planar-ext`
(decision 998, added when `planar-ext` was extracted) and, since decision
1030 (D18, plan 1033 M0, task 6486), `planar-execute` — exposes a `schema`
verb that prints a deterministic flat JSON catalog of its entire command
tree: each command's full path, subcommands, aliases, positionals, and flags
(with inherited flags merged in). Output is always JSON. `planar-execute`'s
catalog describes its hand-rolled `run` surface without moving the parser
onto CLI11; its frozen Lua host-function manifest is a separate surface,
still covered by unit tests rather than by the catalog.

```sh
planar schema
planar-agent schema
planar-watch schema
planar-ext schema
planar-execute schema
```

### Narrowing the catalog: `--command` and `--compact`

The full `planar` catalog is about 350 KB, so every binary's `schema` takes two flags that cut it down. With neither flag the output is the full catalog, unchanged.

| Flag | Effect |
|------|--------|
| `--command <path>` | Emit exactly that command's catalog object (the same object that appears in `commands[]`) and nothing else. `<path>` is the full path (`"planar task update"`) or relative to the root (`"task update"`, `task`); runs of whitespace are collapsed. |
| `--compact` | Emit one row per command with only `command` (the full path) and `summary`, inside the usual envelope with `"layout":"compact"`. The row count equals the full catalog's command count; for `planar` the output is about 24 KB. |

With both flags the output is the compact row for that one command. A path that names no command exits `2`, names the path on stderr (`error: schema: unknown command '<path>'`), and writes nothing to stdout. `--command` is bound to its value, so a bare verb name such as `task` is a lookup and never a subcommand: `planar schema --command task` prints the `planar task` object, wherever the flag sits relative to the verb.

```sh
planar schema --command "planar task update"
planar schema --compact
planar-agent schema --compact --command "queue status"
```

`planar-execute` reads the same two flags in its own parser (`--command=<path>` is accepted too) and takes the same exit code.

`planar`, `planar-agent`, `planar-watch` and `planar-ext` each also expose a
`version` verb (`planar version`, `planar-agent version`, `planar-watch version`,
`planar-ext version`) that prints the binary's version, commit, and compiler;
`planar-execute` has none.

<!-- surface-lint-ignore surface-path-missing: names the deleted-with-zig/ source cli_usage_lint was ported from, for history -->
The catalog is built from the command tree at startup (no DB access), so the verb is a pure read. It is intended for structured consumers — LLM tool routers, editor integrations, and the schema-driven first pass of `make cli-usage-check`, which validates that authored agent/skill/doc surfaces never reference a flag a binary does not expose (implemented as the `cli_usage_lint` C++ tool under `src/tools/`, ported from the Zig tree's `tools/cli_usage_lint.zig` at task 6402). All five binaries are passed to that pass. The same target then runs the semantic authored-surface validator (`surface_lint`); use `make surface-lint` to run that semantic pass alone. Its finding codes, including the host-queue rule's `surface-queue-command` and `surface-queue-marker-invalid` and the `queue-lint-ignore` line and region markers that exempt text from them, and the retired-reference rule's `surface-retired-reference` and `surface-retired-ref-marker-invalid`, are listed in [architecture.md](architecture.md#authored-surface-validation).

---

## Binary: `planar-execute`

The deterministic, spawn-free Lua workflow engine (plan 633; plan 1033 is
turning it into a client over a Centurion host, and its verb shape is kept
through that change). It holds **no SQLite handle** and reaches Planar state
only by shelling `planar` / `planar-agent` from inside a workflow. Unlike the
other four binaries it parses its own arguments and writes its usage banner
to **stderr** — on `--help` too, where stdout stays empty. The exit codes
are its own, not the `planar` table above: a bare invocation and every usage
failure exit `2`, an unreadable workflow, undecodable `--args`, an
unusable engine selection, or a failed phase exits `1`, and `--help` /
`schema` / `profile show` exit `0`. Nothing else is produced:
every refusal, with its exact stderr line, is pinned as a fixture under
`src/cmd/planar-execute/golden/errors/` (plan 1033 M0), and that test also
asserts the observed exit-code set is exactly `{0, 1, 2}`.

### The Centurion client verbs (plan 1033 M2)

> **These verbs require a Centurion-enabled build** (the
> `dev/centurion-integration` branch). This build does not link the Centurion
> client, and it refuses each of them. The refusal comes after argument
> parsing, so a malformed invocation is still a usage failure (exit `2`). It
> comes before a profile is resolved or any state is written, and it prints
> `planar-execute: planar-execute was built without the Centurion engine` on
> stderr, nothing on stdout, and exits `1`. That is the code a durable
> refusal maps to, because only a different build changes the answer. The
> rest of this section describes the verbs as a Centurion-enabled build runs
> them.

`planar-execute` is becoming a CLIENT of a Centurion daemon (decision 1007 /
1075). These verbs act on the daemon serving one execution profile
(`[execute.profiles.<name>]`, default `default`); only `submit` ever STARTS a
daemon, because for an inspection verb a daemon that is not running is the
answer rather than a reason to start one.

- `planar-execute submit <bundle> [--input <json>] [--profile <name>]` — start a bundle run,
  follow it to a terminal state, and print its result JSON on stdout. Exit `0`
  when the run completes, `1` when it ends badly or is durably refused, and
  **`75`** when the daemon refused in a way that stays replayable (a draining
  host, a transient refusal) — "come back later" is not "the work failed", and
  a caller that cannot tell them apart records a failure that never happened.
- `planar-execute status [<run-id>] [--profile <name>] [--json]` — one run's durable
  projection, or the profile itself (state directory, socket, whether a daemon
  is serving) when no run is named. A daemon that is down is a normal answer
  here, exit `0`.
- `planar-execute follow <run-id> [--from <cursor>] [--profile <name>]` — stream the run's
  committed events, one JSON object per line, resuming after the last event
  this client accepted. `--from` overrides that remembered cursor; `--from 0`
  replays from the beginning.
- `planar-execute cancel <run-id> [--profile <name>] [--json]` — stop a run this profile
  admitted. It needs no console session (Centurion ADR-0057).
- `planar-execute host status|drain|stop [--profile <name>]` (`--json` on `host status` only) — report who is serving
  the profile; refuse new submissions while letting running work finish; or ask
  the daemon to stop, which it does by draining under Centurion's bounded
  shutdown. `drain` is a Planar-side admission gate: Centurion exposes no "stop
  admitting" operation, so what Planar stops is its own submitting.

#### The installed daemon

This build installs no daemon. On a Centurion-enabled build, `centuriond` is
installed BESIDE the Planar binaries — `make build` copies it into `./bin/`
and `install.sh` writes `$PLANAR_HOME/bin/centuriond` — because
`planar-execute` resolves it as its own sibling, the same rule `cli.planar(...)`
uses for `planar`. What runs is the daemon that shipped with this client, not
whatever a `PATH` names first.

It is a STOCK Centurion daemon built from the pinned archive (Planar decision
1075): Planar never patches or forks it, and configuration is the whole
Planar-specific input. Its migrations and a `build-identity.json` — the tag,
commit, archive digest, whether it came from a source build or a release
binary, and the binary's own digest — live under
`$PLANAR_HOME/share/centurion/`. That identity is one element of the
compatibility tuple a client compares before joining a daemon another client
started.

The workflow BUNDLE a daemon runs is not installed yet: publishing
`workflows/*.lua` with the Lua prelude and the command policy is plan 1033 M3
(tasks 6510 / 6514), and until it lands a profile points `bundle` at a
directory the operator supplies.

#### Provider configuration

`[execute.profiles.<name>.providers.<vendor>]` is passed to the daemon when
Planar starts one. The pinned Centurion declares exactly one provider, its
CLIProxyAPI model transport, so `<vendor>` must be `cliproxyapi` and its keys
are:

| Key | Reaches the daemon as |
|-----|-----------------------|
| `base_url` | `--model-base-url` |
| `trusted_hostnames` | `--trusted-http-hostname` |
| `api_key` | `CENTURION_CLIPROXYAPI_API_KEY` in its environment |

The credential goes in the ENVIRONMENT and never in the argument vector:
arguments are visible in every process listing on the machine. An unknown
vendor or an unknown key is **refused by name**, and no daemon is started —
passing it through would leave an operator believing a setting took effect
that the daemon never saw. A richer per-vendor provider schema is Centurion's
own plan 1045 and does not exist yet (Planar decision 1146).

### `planar-execute run <workflow.lua> --phase <name>`

Load the workflow in the sandbox, register the deterministic host surface
(`cli` / `git` / `fs` / `flow` / `ctx`), call the named phase, and print the
workflow's `flow.result(table)` payload as JSON on stdout.

- `<workflow.lua>` — required positional; the workflow file path.
- `--phase <name>` — required; the phase function to invoke.
- `--args <json>` — JSON blob exposed to the phase as `ctx.args` (default `""`, which is an empty table). Any JSON value is accepted, not only an object: an array or a string becomes `ctx.args` as-is. Text that is not JSON, or JSON with trailing bytes, is refused before the workflow loads with `planar-execute: args error: --args is not valid JSON` (nesting deeper than 200 levels: `--args is nested too deeply`) and exit `1`.
- `--worktree <dir>` — directory the `git` / `fs` host functions are confined to (default `""`).
- `--sandbox-root <dir>` — root bounding every `fs` path the workflow may touch (default `""`).
- `--engine <embedded|centurion>` — execution engine (plan 1033, decision 1017). Any other value is a usage failure, exit `2`. See [Engine selection](#engine-selection) below.

An unrecognised `--flag`, a second bare positional, a flag with no value, or
a missing `--phase` all print the usage and exit `2`; `run --help` is one of
those, not a help request. `planar workflow run <name>` (see the `workflow`
domain below) resolves a workflow by name and execs this verb with the same
flags.

### Engine selection

Decision 1007 moves workflow execution to a Centurion host; until plan 1033's
cutover the embedded runner stays the default, and both are selectable by
name. The first of these that is set wins:

1. `--engine <embedded|centurion>` on `run`.
2. `$PLANAR_EXECUTE_ENGINE` (an empty value counts as unset).
3. The `execute.engine` config key (`[execute] engine = "…"` in
   `~/.planar/config.toml`), read by running the sibling
   `planar config show --json` — `planar-execute` has no config reader of
   its own. `planar config show` lists the key with its provenance.
4. `embedded`.

Tier 3 costs one `planar` process and is skipped whenever tier 1 or 2 is
set. An invalid env or config value is refused before the workflow loads
(`planar-execute: PLANAR_EXECUTE_ENGINE must be embedded or centurion, got:
…`, or `execute.engine must be … (config file)`), exit `1`. `centurion`
resolves but is refused at dispatch until the host lands in plan 1033 M2:
`planar-execute: engine 'centurion' is not available yet (<source>); the
Centurion host lands in plan 1033 M2`, exit `1`, nothing on stdout. The
selection never changes `run`'s stdout.

### Command policy (`workflows/command-policy.json`)

Under the Centurion engine (plan 1033, tech-spec D2/D14) a workflow reaches
`planar`, `planar-agent`, `planar-watch` and `git` only through Centurion's
stock `command.exec` activity, and only for a `(binary, verb path)` listed in
this closed policy, which ships with the workflow bundle. Each entry carries
an `effect` (`idempotent` for a read, `reconcilable` for a write whose outcome
recovery can re-establish), a `cwd` rule (`any` for a command that addresses
its target by id or token, `workspace` for one that must run inside the
registered lane workspace), and `used_by`, the workflows that call it.
`planned_workflows` names workflows not shipped yet (the M4
claim-supervision workflow); their entries must become used once they are.

Two gates keep it honest. `src/cmd/planar-execute/handlers/shared/policy.t.cpp` extracts
every host call from the shipped workflows (`cli.*`, the `ctx.*` reads, and
`git.*`) and fails if one resolves to no entry, if an entry is unused, or if
an entry's `used_by` is not exactly the workflows that use it; it also
requires every Planar entry a shipped workflow uses to pass the embedded
engine's own allowlist, until the embedded runner is retired. `make
cli-usage-check` fails when an entry names a verb path that is not a runnable
leaf in the live schema catalogs. Nothing enforces the policy at run time
until the Centurion host lands (M2/M3); the embedded engine still enforces
its own allowlist.

### `planar-execute profile show [--profile <name>] [--json]`

Print the resolved engine (with the provenance that chose it) and the
resolved **execution profile** on stdout, exit `0`. `--profile` defaults to
`default`. Text is `key: value` lines (`engine`, `engine_source`, `profile`,
`configured`, `state_dir`, `planar_db`, one `allowed_root:` per root or
`allowed_roots: -`, `idle_grace_seconds`, `command_policy`, `bundle`, one
`provider.<vendor>.<key>:` per setting or `providers: -`); `--json` is
`{"engine":…,"engine_source":…,"profile":{"name":…,"configured":…,"state_dir":…,"planar_db":…,"allowed_roots":[…],"idle_grace_seconds":…,"command_policy":…|null,"bundle":…|null,"providers":{…}}}`.
Any other `profile` shape prints the usage and exits `2`.

A profile (plan 1033 task 6494, tech-spec D10) is the unit a Centurion host
runs for — one host per profile, identified by its canonical state
directory. It is configured as

```toml
[execute.profiles.<name>]
state_dir          = "~/.planar/execute/<name>"   # default: $PLANAR_HOME/execute/<name>, else ~/.planar/execute/<name>
planar_db          = "~/.planar/planar.db"        # default: $PLANAR_DB, else ~/.planar/planar.db
allowed_roots      = ["~/code"]                   # default: none
idle_grace_seconds = 300                          # default 300
command_policy     = "…"                          # default: unset
bundle             = "…"                          # default: unset

[execute.profiles.<name>.providers.<vendor>]
<key> = "…"                                       # passed to Centurion verbatim
```

`default` always exists; unconfigured, it is exactly the defaults. Every
path is `~`-expanded, made absolute and canonicalised (`weakly_canonical`,
since the state directory may not exist yet), so two profiles whose
`state_dir` is a path and a symlink to it report the same identity. The
profile is read like every other `planar-execute` setting, through the
sibling `planar config show --json` (whose resolver surfaces every
`execute.profiles.*` key the file sets). Every configured profile is
validated, not only the one asked for; an unknown key (`unknown key
'execute.profiles.x.state_dirr' in <config file>`), a profile name that is
not configured, `allowed_roots` that is not an array, or an
`idle_grace_seconds` that is not a positive integer is refused naming the
config file, exit `1`.

### `planar-execute schema`

Print the flat JSON catalog for this binary — the same envelope
(`schemaVersion`, `layout`, `root`, `commands`) the other four emit — on
stdout, exit `0`, nothing on stderr. Added by decision 1030 so
`cli_usage_lint` polices authored references to `planar-execute` verbs; the
catalog is a description of the hand-rolled parser, pinned against it by
test, not a second parser.
`--command <path>` and `--compact` narrow the output as described under
[Introspection: `schema`](#introspection-schema-all-planning-state-binaries); an
unknown path exits `2` with the message on stderr.

---

## Domain: `run`

Operational run-record surface for Planar-native workflow execution. Run records track a workflow's lifecycle — start, streamed journal events, and a terminal finish — and are queryable via `planar run show`. The `run` domain uses `arm=op` (the operational arm), distinguishing it from the measurement-rig `planar bench` surface (which uses `arm=strict`, `arm=eligibility`, etc. and additionally tracks declared/actual file touches and git-diff harvests).

Both `planar run` and `planar bench` write to the same `runs` + `run_events` tables (migration `00025_runs`). The read surface is shared: `planar-watch run list` / `planar-watch run show` display records from both. The `run` domain does NOT manage claims — claim lifecycle is `planar-agent pull` / `complete` / `fail` / `release`. Use `planar run` to bracket the outer workflow trace; claims inside the workflow use `planar-agent`.

Note: `runs`/`run_events`/`run_touches` are distinct from the context-plane tables `workflow_runs`/`context_records` (migration `00022`), which are written by `planar-agent run start/end` and `planar-agent context` — not by `planar run`/`bench`.

---

### `planar run start`

**Synopsis:**
```
planar run start --plan <plan-id> [--workflow <name>] [--json]
```

**Description:** Mint a new operational run record for the named plan and print its `run_uid` (a stable string identifier) as JSON. The run starts in `running` status. The caller uses `run_uid` for all subsequent `event` and `finish` calls.

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--plan <plan-id>` | Plan id to associate the run with. | Required. |
| `--workflow <name>` | Human-readable workflow name (free-form). When supplied, it is recorded as the `workflow_name` on the run row. | (none) |
| `--json` | Emit a JSON object. | off |

**Output (`--json`):**
```json
{"run_uid":"run-2026-06-14-abc123","plan_id":7,"arm":"op"}
```

**Schema effects:** Inserts into `runs(run_uid, plan_id, arm, status='running', started_at)`. When `--workflow <name>` is supplied the name is stored as the `arm` value instead of the default `"op"`.

**Capture:** None.

**Exit codes:**
- `0` — success.
- `1` — plan id not found or invalid integer.

---

### `planar run event <run-uid>`

**Synopsis:**
```
planar run event <run-uid> --kind <kind> [--payload <text>] [--json]
```

**Description:** Append a journal event to the named run. The sequence number is auto-incremented server-side. Repeat for each meaningful step during workflow execution.

**Arguments:**

| Argument | Description |
|----------|-------------|
| `<run-uid>` | Run uid returned by `run start`. |

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--kind <kind>` | Event kind (free-form label, e.g. `step`, `note`, `error`). | Required. |
| `--payload <text>` | Optional event payload body. | none |
| `--json` | Emit a JSON result object. | off |

**Output (`--json`):**
```json
{"ok":true,"run_uid":"run-2026-06-14-abc123","seq":1,"kind":"step"}
```

**Schema effects:** Inserts into `run_events(run_uid, seq, kind, payload, at)`.

**Capture:** None.

**Exit codes:**
- `0` — success.
- `1` — run uid not found or run is already in a terminal status.

---

### `planar run finish <run-uid>`

**Synopsis:**
```
planar run finish <run-uid> --status <status> [--json]
```

**Description:** Set the terminal status on a run. After `finish`, no further `event` calls are accepted.

**Arguments:**

| Argument | Description |
|----------|-------------|
| `<run-uid>` | Run uid returned by `run start`. |

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--status <status>` | Terminal status. Accepted values: `completed`, `aborted`, `error`. | Required. |
| `--json` | Emit a JSON result object. | off |

**Output (`--json`):**
```json
{"run_uid":"run-2026-06-14-abc123","status":"completed"}
```

**Schema effects:** Updates `runs(status, ended_at)`.

**Capture:** None.

**Exit codes:**
- `0` — success.
- `1` — run uid not found; or run is already in a terminal status; or `--status` value is not one of the accepted values.

---

### `planar run show <run-uid>`

**Synopsis:**
```
planar run show <run-uid> [--json]
```

**Description:** Show a run's full state — header metadata and all journal events in sequence order.

**Arguments:**

| Argument | Description |
|----------|-------------|
| `<run-uid>` | Run uid returned by `run start`. |

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--json` | Emit a JSON object with `run` header and `events` array. | off |

**Output (human):**
```
run run-2026-06-14-abc123  plan:7  status:completed
  started:  2026-06-14T10:00:00Z
  ended:    2026-06-14T10:05:30Z
  workflow: my-workflow

events:
  1  step     "Started coder dispatch"
  2  step     "Coder cycle complete"
  3  note     "Integration tests passed"
```

**Output (`--json`):**
```json
{
  "run_uid":"run-2026-06-14-abc123","plan_id":7,"arm":"op","status":"completed","started_at":"2026-06-14T10:00:00Z","ended_at":"2026-06-14T10:05:30Z",
  "events": [
    {"id":1,"seq":1,"kind":"step","payload":"Started coder dispatch","created_at":"2026-06-14T10:00:01Z"},
    {"id":2,"seq":2,"kind":"step","payload":"Coder cycle complete","created_at":"2026-06-14T10:04:00Z"},
    {"id":3,"seq":3,"kind":"note","payload":"Integration tests passed","created_at":"2026-06-14T10:05:00Z"}
  ]
}
```

**Schema effects:** Reads `runs`, `run_events`. No writes.

**Capture:** None (read-only).

**Exit codes:**
- `0` — success.
- `1` — run uid not found.

---

## Domain: `report`

### `planar report [--days <n>] [--tail <n>] [--json]`

**Description:** Emit the diagnostic bundle: invocation aggregates, failure tail, closed claim-failure category aggregates, and always-on health metrics. Reads `cli_invocations` (when CLI logging is enabled) plus the always-on observability tables (`agent_actions`, `sync_events`, `agent_work_claims`, `handoffs`) and renders a structured diagnostic bundle.

When `[introspection].cli_log` is off (the default), the invocation and failure sections render "logging disabled" instead of counts — the operator is never shown fabricated zeros. The always-on sections (`actions`, `sync`, `claims`, `claim_failure_categories`, `handoffs`, `health`, schema version) render normally in either case. JSON output also includes `introspection_preview` with bounded `signals`, per-adapter `coverage`, and `warnings`, collected read-only from the effective `[introspection.transcripts]` paths. A failed adapter degrades only its own coverage; other adapters still contribute. Successful commands are coverage observations, not gap findings; only explicit invalid-flag/help-bounce evidence is normalized as `gap`.

**Privacy:** All queries are structurally redacted by construction in `src/engine/introspect/introspect.cpp`. The bundle selects only counts, closed categories, provider identities, verb paths, statuses, and timestamps — never entity `title`, `body`, or `summary` columns, never release reasons or action summaries, never scope slugs, and never path-bearing columns. The claim-failure aggregate includes only `aborted`/`stale` terminals; a null category on those legacy or uncategorized recovery rows is reported as `unknown`. Completed and released claims are non-failure terminals and are excluded.

**Flags:**

| Flag | Type | Default | Description |
|------|------|---------|-------------|
| `--days <n>` | int | 30 | Window in days. Must be a positive integer. |
| `--tail <n>` | int | 20 | Number of failure-tail rows to include in the text output. Must be a positive integer. |
| `--json` | bool | false | Emit stable machine-readable JSON. |

**JSON wire format (`--json`):** Top-level fields are the contract consumed by downstream agents and skills:

| Field | Type | Notes |
|-------|------|-------|
| `version` | string | Binary version string. |
| `schema_version` | integer | Max schema_migrations version applied. |
| `health` | string | `"ok"` or `"degraded"`. |
| `window` | integer | The `--days` value queried. |
| `invocations` | array | Per-verb-path aggregate rows; empty array when logging disabled or no data. |
| `failures` | array | Per-error-category failure counts; empty array when logging disabled or no data. |
| `actions` | array | Agent-action outcome aggregates (always-on). |
| `sync` | array | Sync-event outcome aggregates (always-on). |
| `claims` | object | `{stale_claims, never_consumed}` (always-on). |
| `claim_failure_categories` | array | Stable `{provider, category, count}` rows for aborted/stale claims in the window, ordered by provider then the closed category order. Null categories aggregate as `unknown`; empty windows emit `[]`. |
| `handoffs` | object | `{stale_handoffs, never_consumed}` (always-on). `never_consumed` counts handoffs created in the window that were never transitioned to `consumed` status (distinct from `stale_handoffs`, which counts only `pending`/`validated` handoffs older than 24 h). |
| `reopens` | integer | Count of `task_reopens` rows created in the window (always-on). |

Empty windows emit empty arrays, never nulls or missing fields.

**Exit codes:**

| Code | Meaning |
|------|---------|
| 0 | Bundle rendered successfully (including "logging disabled" path). |
| 2 | Invalid flag value — `--days` or `--tail` must be a positive integer; no partial bundle is emitted. |
| 1 | Database error. |

**Example:**

```
planar report
planar report --days 7 --tail 5
planar report --json
planar report --days 14 --json
```

**Schema effects:** Read-only. Touches `cli_invocations`, `agent_actions`, `sync_events`, `agent_work_claims`, `handoffs`, `schema_migrations`, `tasks`, `context_snapshots`. No writes.

---

## Domain: `search`

### `planar search <query> [--kind <kind>] [--status <status>] [--scope <scope>] [--plan <id>] [--limit <n>] [--json]`

**Description:** Full-text search across plans, tasks, questions, scenarios, decisions, and artifacts (backed by the FTS5 index from migration `00011_slug_refs_fts`). Without `--scope`, search uses the cwd-derived read set: workspace roots search the org plus member projects, and member repo cwd searches the most specific repo. Narrow results with `--kind` (restrict to a single entity kind), `--status`, `--scope`, or `--plan` (entities under a given plan); cap with `--limit`. `--json` emits the structured result list.

**Schema effects:** Reads the FTS index and the underlying entity tables. No writes.

---

## Domain: `explore`

### `planar explore [--plan <id>] [--task <id>] [--scope <s>] [--plain]`

**Description:** Reserved for launching Planar Explorer, a separate project that presents a read-only view of the planning graph. Not yet implemented: the command prints its own help page and exits 0. It does not open the database.

**Flags:**

| Flag | Description | Default |
|------|-------------|---------|
| `--plan <id>` | Reserved: seed initial focus on the given plan ID. | unset |
| `--task <id>` | Reserved: seed initial focus on the given task ID. | unset |
| `--scope <s>` | Reserved: seed the scope filter. | cwd-derived |
| `--plain` | Print the help page. This is currently the only behavior. | off |

**Schema effects:** None.

---

## Domain: `workflow`

Discovery layer for user-authored and shipped Lua workflows for `planar-execute`. Both subverbs are **read-only filesystem scans** — they never open SQLite.

**Shipped workflows** are sourced from `$PLANAR_HOME/workflows/` (default `~/.planar/workflows/`, populated by `install.sh`). **Sandbox workflows** live at `~/.planar/local/workflows/` and are marked `local`.

### `@meta` block convention

Every workflow file may declare a machine-parseable metadata block in a Lua long-string comment at the very top of the file:

```lua
--[[ @meta
name: finalize-closeout
description: Deterministic closeout gate — never force-closes.
phases: closeout
seam: planar run start/event/finish, planar plan closeout
--]]
```

Rules:
- The opening line must be `--[[ @meta` (no leading whitespace).
- Key/value pairs are `key: value` (plain scalars; no nesting).
- The block closes with `--]]` on its own line.
- Unknown keys are silently skipped.
- A missing block is not an error — `workflow list/show` fall back to the filename stem as the name.

### `planar workflow list [--local] [--json]`

**Description:** List every discovered workflow across shipped and sandbox directories, sorted by name.

**Flags:**

| Flag | Description | Default |
|------|-------------|---------|
| `--local` | Show only sandbox workflows (`~/.planar/local/workflows/`). | off |
| `--json` | Emit one JSON object per line (NDJSON). | off |

**Behavior:**
- Without `--local`, shipped workflows are listed first, then sandbox workflows.
- When a workflow has a `@meta` block with a `name:` field, that name is used; otherwise the filename stem (sans `.lua`) is used.
- An absent directory (no workflows installed yet) is silently treated as empty.

**Text output columns:** `name`, `kind` (`shipped` or `local`), `phases`, `description`.

**JSON shape (one object per line):**
```json
{"name":"finalize-closeout","kind":"shipped","path":"/…/workflows/finalize_closeout.lua","filename":"finalize_closeout.lua","meta_found":true,"description":"…","phases":"closeout","seam":"…"}
```

**Exit codes:**
- `0` — success (including zero results).
- `1` — directory scan error (unreadable dir).

### `planar workflow show <name> [--json]`

**Description:** Resolve a workflow by name and print its `@meta` fields and source path. Name matching uses the effective name (see `@meta` convention above). Shipped directories are searched before sandbox.

**Positional:**

| Argument | Description | Required |
|----------|-------------|----------|
| `<name>` | Workflow name to resolve. | yes |

**Flags:**

| Flag | Description | Default |
|------|-------------|---------|
| `--json` | Emit a single JSON object. | off |

**Behavior:**
- The `<name>` argument is matched against each workflow's effective name (the `name:` field from `@meta`, or the filename stem).
- Shipped directories are searched before sandbox directories.
- Exits `1` (not found) when no workflow with that name exists.

**Text output:**
```
name:        finalize-closeout
kind:        shipped
path:        /…/workflows/finalize_closeout.lua
meta:        present
description: Deterministic closeout gate — never force-closes.
phases:      closeout
seam:        planar run start/event/finish, planar plan closeout
```

**JSON shape:**
```json
{"name":"finalize-closeout","kind":"shipped","path":"…","filename":"finalize_closeout.lua","meta_found":true,"description":"…","phases":"closeout","seam":"…"}
```

**Exit codes:**
- `0` — workflow found.
- `1` — workflow not found.

### Sandbox directory

The sandbox directory `~/.planar/local/workflows/` mirrors the `~/.planar/local/skills/` model: it is user-machine-local state, never committed to the repo, and is created on demand. Place any `.lua` file there to author and test a workflow before promoting it to the repo's `workflows/` directory. See [Recipe 28 — Author and graduate a workflow](workflows.md#recipe-28--author-and-graduate-a-workflow) in `docs/workflows.md`.

**`PLANAR_WORKFLOWS_DIR` env var:** When set, overrides the shipped workflows directory. Used by the integration test harness to point `workflow list/show` at a temporary directory seeded with fixture workflows.

---

## Command Index

For quick reference, all documented commands grouped by domain:

| Domain | Commands |
|--------|----------|
| `init` | `init` (calls `config init` and `templates init` before applying migrations) |
| `config` | `config show`, `config show --effective`, `config show --raw`, `config show --defaults`, `config edit`, `config validate`, `config init`, `config path` |
| `templates` | `templates list`, `templates show`, `templates render`, `templates validate`, `templates init`, `templates path` |
| `tree` | `tree` |
| `scope` | `scope show`, `scope suggest` (`scope use`/`pop`/`clear` removed in plan 153 M5) |
| `assoc` | `assoc list`, `assoc create`, `assoc add`, `assoc remove`, `assoc members`, `assoc detect` |
| `plan` | `plan create`, `plan show`, `plan list`, `plan update`, `plan descendants`, `plan step add`, `plan step done`, `plan step skip`, `plan step link`, `plan link` |
| `task` | `task add`, `task show`, `task list`, `task update`, `task edit`, `task view`, `task diff`, `task review`, `task done`, `task reopen`, `task block`, `task link`, `task facts stage`, `task touches add`, `task touches remove` |
| `question` | `question add`, `question answer`, `question wontfix`, `question list`, `question show`, `question edit`, `question view`, `question diff`, `question review`, `question link` |
| `scenario` | `scenario add`, `scenario verify`, `scenario list`, `scenario show`, `scenario edit`, `scenario view`, `scenario diff`, `scenario retire` |
| `decision` | `decision add`, `decision accept`, `decision supersede`, `decision withdraw`, `decision list`, `decision show`, `decision edit`, `decision view`, `decision diff` |
| `artifact` | `artifact add`, `artifact show`, `artifact list`, `artifact update`, `artifact edit`, `artifact view`, `artifact diff`, `artifact link` |
| `document` | `document project`, `document validate-range` |
| `annotate` | `annotate add`, `annotate show`, `annotate list`, `annotate capabilities`, `annotate update`, `annotate remove`, `annotate tag`, `annotate resolve`, `annotate dismiss`, `annotate archive`, `annotate bulk-resolve`, `annotate bulk-dismiss`, `annotate bulk-archive`, `annotate verify`, `annotate sweep` |
| `promote` | `promote`, `demote` |
| `workbench` | `workbench lint`, `workbench push`, `workbench pull`, `workbench status`, `workbench resolve`, `workbench sync`, `workbench archive`, `workbench restore`, `workbench list`, `workbench publish`, `workbench edit` |
| `workspace` | `workspace init`, `workspace doctor`, `workspace routing build`, `workspace routing show`, `workspace regenerate` |
| `ext` | `ext register jira`, `ext register github`, `ext list`, `ext test`, `ext create`, `ext propagate`, `ext propagate-one` |
| `link` | `link`, `unlink` |
| `sync` | `sync pull`, `sync push`, `sync status`, `sync resolve` |
| `resume` | `resume`, `resume validate` |
| `handoff` | `handoff`, `handoff create`, `handoff validate`, `handoff list`, `handoff show`, `handoff consume`, `handoff abandon` |
| `capture` | `capture session`, `capture end`, `capture commits`, `capture note`, `capture command`, `capture file`, `capture snapshot` |
| `audit` | `audit trail`, `audit session`, `audit commits`, `audit publish-decision`, `audit handoff-readiness` |
| `health` | `health` |
| `models` | `models evals`, `models resolve`, `models experiments`, `models outcomes`, `models registry list\|add\|update\|remove\|bind\|unbind\|observe\|eligibility\|verify-identity\|export` |
| `links` | `links add`, `links list`, `links remove`, `links trail` (no `links update`) |
| `report` | `report [--days <n>] [--tail <n>] [--json]` |
| `search` | `search <query>` |
| `explore` | `explore [--plan <id>] [--task <id>] [--scope <s>] [--plain]` |
| `spec` | `spec ingest` |
| `test-spec` | `test-spec status` |
| `import` | `import <repo-root>` |
| `synthesize` | `synthesize <repo-root>` |
| `local` | `local list`, `local link`, `local unlink`, `local import`, `local migrate` |
| `--help` | `--help` / `-h` on any node (no `help` verb) |
| `run` | `run start`, `run event`, `run finish`, `run show` |
| `workflow` | `workflow list`, `workflow list --local`, `workflow show <name>` |
| `feedback` | `feedback triage list`, `feedback triage show`, `feedback triage set` |
| `schema` | `schema` (also on `planar-agent` and `planar-watch`) |
| `update` | `update`, `update --check`, `update --version <tag>` |
## Domain: `closure`

Derived-closure extraction: given a task's touched `(repo, path)` seeds, walk
the source with tree-sitter and persist the files that task's work actually
reaches. `closure compute` does the walk and writes; `closure show` reads back
what was written.

Rows are classified by `role`:

| Role | Meaning |
|------|---------|
| `modify` | A seed path itself — the file the task touches directly. |
| `reference` | Reached from a seed by a direct source reference. |
| `transitive` | Reached only through another reference. |

**SQLite tables:** reads `task_touch_paths` for seeds and `projects.root_path`
to resolve them against a checkout; writes the closure rows `closure show`
reads back.

---

### `planar closure compute <task-id>`

**Synopsis:**
```
planar closure compute <task-id> [--scope <slug>] [--json]
```

**Description:** Run the extractor over a task's seeds and persist the closure.
Seeds come from `task_touch_paths`, so a task with no touched paths has nothing
to walk and is refused rather than silently writing an empty closure.

Seed paths are resolved against `projects.root_path`, which is why a project
registered with a path that does not match your checkout produces an empty or
wrong result — see the association-less-repo advisory under
[`task touches add`](#planar-task-touches-add-task-id-repo-slug---path-p).

**Scope guard:** Refuses when the operator's resolved write scope disagrees
with the task's, using the membership-aware comparison (see
[Cross-scope guard](#cross-scope-guard); strict equality here was widened to
match the other guarded verbs by decision 1121).

**Arguments:**

| Argument | Description |
|----------|-------------|
| `<task-id>` | The task whose seeds to walk (integer). |

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--scope <slug>` | Write-scope override for the cross-scope guard. | cwd-derived |
| `--json` | Emit the row-count summary as JSON. | off |

**Output:** A count summary — seeds walked, and rows written per role.

**Exit codes:**
- `0` — closure computed and persisted.
- `1` — the task has no seeds, or a query failed.
- `5` — cross-scope guard refusal.

---

### `planar closure show <task-id>`

**Synopsis:**
```
planar closure show <task-id> [--json]
```

**Description:** Read back a task's persisted closure rows. Read-only; it never
recomputes, so a task whose closure was never computed reads back empty rather
than triggering a walk.

**Arguments:**

| Argument | Description |
|----------|-------------|
| `<task-id>` | The task whose closure to read (integer). |

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--json` | Emit the rows as JSON. | off |

**Exit codes:**
- `0` — rows rendered (including none).
- `1` — query failure.

---

## Domain: `feedback`

Migration `00028_feedback_triage` stores deterministic operator triage for
findings associated with the plan whose slug is `planar-feedback`. One row
targets exactly one task or question. Partial unique indexes enforce one row per
finding; a delete trigger resets dependents of a deleted duplicate target to
`disposition=untriaged` and clears their duplicate reference without deleting
their evidence or reproduction state. This is local planning state:
`feedback triage` never writes agent tables and never posts to an external
system.

```text
planar feedback triage list [--plan <id>] [--severity <info|low|medium|high|critical>]
  [--disposition <value>] [--json]
planar feedback triage show <task:id|question:id> [--json]
planar feedback triage set <task:id|question:id> --severity <value>
  --disposition <value> --reproduction <value>
  [--duplicate-of <task:id|question:id>] [--evidence <redacted-text>]
  [--scope <slug>] [--json]
```

**Scope guard on `feedback triage set`:** it refuses when the operator's
resolved write scope disagrees with the OWNING entity's, using the
membership-aware comparison — an operator scope `assoc:<org>` covers a finding
on a `repo:<member>` task. `list` and `show` are reads and are unguarded. See
[Cross-scope guard](#cross-scope-guard); `feedback triage set` is one of the
ten verbs in that table.

Disposition values are `untriaged`, `needs-reproduction`, `accepted`,
`retained-question`, `dismissed`, `reported-external`, and `duplicate`.
Severity values are `info`, `low`, `medium`, `high`, and `critical`.
Reproduction values are `not-run`, `reproduced`, `not-reproduced`, and
`inconclusive`. `duplicate` requires `--duplicate-of`; all other dispositions
forbid it. Duplicate targets must already have triage state, belong to the same
feedback plan, and may not form self-links or cycles. `set` is scope guarded,
performs only the operator-confirmed local update, and never posts externally.

`list` may filter by numeric plan id, severity, and disposition; it emits an
ordered JSON array. `show` and `set` emit one object. The stable row fields are:

```json
{
  "id": 7,
  "finding": "task:42",
  "plan_id": 12,
  "severity": "high",
  "disposition": "accepted",
  "reproduction_status": "reproduced",
  "duplicate_of": null,
  "evidence_summary": "redacted retry trace",
  "created_at": "2026-07-13T12:00:00.000Z",
  "updated_at": "2026-07-13T12:00:00.000Z"
}
```

Only `set` writes, using an upsert keyed by the task/question finding. Tasks
must have `plan_id` pointing to the feedback plan. Questions must have exactly
one `derives-from` plan link, and that plan must be the feedback plan. Evidence
is stored as supplied in `evidence_summary`; callers are responsible for
passing redacted text.

**Exit codes:**

- `0` — read or operator-confirmed set succeeded; an empty list is `[]`.
- `1` — finding/triage row not found, missing or ambiguous feedback-plan
  membership, cross-plan duplicate, duplicate cycle, database, or other
  operational failure.
- `2` — malformed finding reference, invalid enum, missing required flag,
  invalid duplicate flag combination, or other CLI input failure.
- `5` — `set` refused a cross-scope write; change cwd or pass the matching
  explicit `--scope`.
