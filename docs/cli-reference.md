# Planar CLI Reference

Reference for every `planar` subcommand. Authoritative current surface for the installed binary. For machine-readable help, use `planar <subcommand> --help`.

**Source of truth:** schema across `src/migrations/0001_foundation.sql` through `src/migrations/0007_workbench.sql` (21 application tables). Every "schema effects" section below cites real columns from those migrations.

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

### Scope Shorthand

`--scope` accepts three forms wherever it appears:

| Form | Meaning |
|------|---------|
| `--scope repo` | Scope to the current repo (resolved from `cwd` against `projects.root_path`). |
| `--scope repo:<slug>` | Scope to the named project slug. |
| `--scope assoc:<slug>` | Scope to the named association slug. |
| `--scope global` | Global / personal scope (no repo or association filter). |

**Mutating commands** (`task add`, `plan create`, `question add`, `scenario add`, `decision add`, `artifact add`, `link`, `unlink`, `ext create`, `ext propagate`) resolve scope through the strict `ResolveForWrite` algorithm: explicit flag → cwd derivation with most-specific-wins → refuse with an `AmbiguousScopeError` listing candidate `--scope` values. Cwd is the only default; there is no ambient stack. The resolver never silently picks a default when multiple candidates tie at the best rank, nor when cwd lands at a workspace root with member projects (see the workspace-root refusal in [Scope in `docs/concepts.md`](./concepts.md#scope)). See that section for the full algorithm, the membership-aware cross-scope guard, and the specificity ranking.

Source annotations on success: `[from flag]`, `[from cwd]`. The resolved scope and source are printed on success (human output) and included as `scope_kind`, `scope_id`, and `scope_source` fields in `--json` output. To opt out of strict resolution entirely, see `--no-scope-check` in [Global Flags](#global-flags).

**Query commands** (`task list`, `plan list`, etc.) use `ResolveForRead`, which derives the in-scope set from cwd: at a workspace root the org plus every member project; at a member project root the project plus any cross-repo entities reachable via `touches` links. Outside any registered scope, reads refuse unless `--scope global` is passed explicitly. `--scope` on a query is a filter, not a strict pick.

### Exit Codes

| Code | Meaning |
|------|---------|
| `0` | Success. |
| `1` | User-fixable error: entity not found, validation failure, missing required argument. |
| `2` | System error: database open failure, I/O error, network error on sync. |
| `3` | Conflict: sync conflict detected; requires explicit `sync resolve`. |
| `64` | Usage error: bad flag combination, unrecognized subcommand (per `sysexits.h EX_USAGE`). |

### Capture Behavior

Every command that writes to the database appends a `session_entries` row to the current active session for the affected task (if the command is task-scoped). If no active session exists, one is created automatically with the current process's vendor string. This is the automatic capture described in the tech spec — agents do not need to call a separate save-state command.

Commands that are read-only (list, show, status) do not create sessions or session entries unless the command is explicitly capture-oriented (see [capture domain](#domain-capture)).

Vendor identity for auto-created sessions is taken from the `PLANAR_VENDOR` environment variable (default: `"cli"`). A vendor session id is taken from `PLANAR_VENDOR_SESSION_ID` if set.

**Session auto-creation policy:** one session per `(vendor, vendor_session_id)` tuple. If `vendor_session_id` is unset (env var missing), one session per process for the duration of the active task. The `vendor` is read from `$PLANAR_VENDOR`; `vendor_session_id` is read from `$PLANAR_VENDOR_SESSION_ID`. Both are unset at install time; vendor harnesses are responsible for setting them on invocation.

---

## Top-Level Usage

```
planar [GLOBAL FLAGS] <subcommand> [subcommand args]
```

### Global Flags

| Flag | Description | Default |
|------|-------------|---------|
| `--db <path>` | Path to the SQLite database. | `~/.planar/planar.db` |
| `--json` | Emit machine-readable newline-delimited JSON instead of human text. | off |
| `--quiet` / `-q` | Suppress informational output; only emit errors and explicit results. | off |
| `-v` | Enable info-level logging. | off |
| `-vv` | Enable debug-level logging. | off |
| `--no-scope-check` | Escape hatch. Downgrades the cross-scope guard refusal to a one-line stderr warning. When `ResolveForWrite` refuses at a workspace-root cwd (lone org candidate), this flag promotes the org to the resolved scope so the write lands at the org level. Other `AmbiguousScopeError` shapes (tied candidates, no cwd match) still fail — there is no ambient fallback target. Use for legacy scripts and one-off corrections; do not add to routine workflows. | off |
| `--color <mode>` | Color mode for `tree` and `list`-style renderers. `auto` emits ANSI only when stdout is a TTY; `always` emits unconditionally (except when `NO_COLOR` is set); `never` never emits. JSON output paths never colorize regardless of this flag. | `auto` |
| `--no-color` | Shorthand for `--color=never`. Wins over `--color=always` when both are passed (more restrictive choice wins). | off |

The `NO_COLOR` environment variable (any non-empty value) overrides `--color=always`. This follows the [no-color.org](https://no-color.org/) convention and protects operators who set the env var globally from downstream color sequences. To re-enable color in a shell where `NO_COLOR` is set, unset the variable rather than passing `--color=always`.

Global flags must appear before the subcommand. They are not repeated in per-command option tables below.

---

## Cross-scope guard

The cross-scope guard is a refusal mechanism that runs at the top of every mutating verb that takes an existing entity id, or that walks from a parent entity to derived rows. It is layered on top of the strict write-scope resolver described in [Scope Shorthand](#scope-shorthand): the resolver picks the operator's intended scope; the guard then compares that against the *target entity's* stored `(scope_kind, scope_id)` and refuses if they disagree. See [docs/concepts.md § Cross-scope guard](./concepts.md#cross-scope-guard) for the conceptual model.

### Resolution → guard pipeline

For every guarded verb:

1. Look up the target entity's stored scope. Global-scoped entities short-circuit and are accepted from any operator scope.
2. Resolve the operator's write scope through the cwd-primary algorithm (explicit `--scope` flag → cwd derivation, most-specific-wins → refuse with `AmbiguousScopeError`).
3. Compare the two scopes with the membership-aware coverage rule: equality matches, and an operator scope `assoc:<org>` covers any entity scoped to one of the org's member projects (via `project_associations`). Otherwise refuse with exit 1. The reverse direction (operator project, entity org) does not cover.

### Refusal message

The refusal is a multi-line message naming both scopes and listing three remediation paths:

```
<kind> <id> belongs to <entity-scope> but the resolved write scope is <op-scope>.

       This usually means your cwd is inside a different repo than the
       <kind>'s owning project/association. To proceed:

         a. cd into a directory inside <entity-scope>, OR
         b. pass --scope <entity-scope> explicitly, OR
         c. pass --no-scope-check to override (legacy escape hatch, not for routine use)

       Refusing cross-scope write without explicit operator intent
```

The exit code is `1` (user-fixable). No database writes occur before the refusal.

### Escape hatch

`--no-scope-check` (see [Global Flags](#global-flags)) downgrades the refusal to a one-line stderr warning and lets the write proceed:

```
warning: overriding scope mismatch (<kind> <id> belongs to <entity-scope> but resolved write scope is <op-scope>) due to --no-scope-check
```

Appropriate uses:

- Legacy scripts that cannot be updated immediately.
- One-off corrections after a scope-rewrite migration where the operator has verified the target.
- Manual ad-hoc fix-ups where the exact entity id is known to be correct.

Inappropriate uses: routine workflows, skill bodies, agent dispatch code, orchestrator loops. Reach for `--scope <slug>` first.

### Guarded verbs

Verbs that perform an explicit cross-scope check on every invocation. The "guards against" column names the entity kind whose stored scope is read for the comparison.

| Verb | Guards against | Notes |
|------|---------------|-------|
| `spec ingest <plan>` | `plan` | Bulk write of derived rows under the anchor plan. |
| `ext propagate <plan>` | `plan` | Walks the feature tree to create external counterparts. |
| `sync push <link\|kind:id>` | `plan` or `task` | Single-target push form; `--all` is unguarded. |
| `sync pull <link\|kind:id>` | `plan` or `task` | Single-target pull form; `--all` is unguarded. |
| `sync resolve <event-id>` | resolved via the event's target entity | Conflict resolution writes back to the target. |
| `plan update <plan-id>` | `plan` | |
| `plan step add <plan-id> <body>` | parent `plan` | New step inherits the parent plan's scope. |
| `plan step done <step-id>` | `plan_step` (inherits parent plan's scope) | |
| `plan step skip <step-id>` | `plan_step` | |
| `task update <task-id>` | `task` | |
| `task done <task-id>` | `task` | |
| `task reopen <task-id>` | `task` | |
| `task block <task-id> --on <task-id>` | `task` (both blocked and blocking) | Guard fires on both endpoints. |
| `question update <question-id>` | `question` | |
| `scenario update <scenario-id>` | `scenario` | |
| `decision update <decision-id>` | `decision` | |
| `decision supersede <old> --by <new>` | `decision` (both old and new) | Guard fires on both endpoints. |
| `artifact update <artifact-id>` | `artifact` | |
| `audit publish-decision <decision-id>` | `decision` | Posts to every external link reachable from the decision. |
| `ext create --from <kind:id>` | `plan` or `task` | The local entity the external counterpart will mirror. |
| `link <kind:id> --to ...` | local entity kind | `external_links` creation against a local entity. |
| `unlink <link-id>` | resolved via the link's local entity | Reads the `external_links` row to find the local endpoint. |
| `links update <link-id>` | resolved via the link's local entity | `external_links` field update. |

### Explicitly unguarded verbs

Verbs audited and deliberately left unguarded. The absence is recorded so future readers do not interpret it as an oversight.

| Verb class | Rationale |
|-----------|-----------|
| `*_link` (e.g. `task link`, `plan link`, `links add`, `links remove`, `task touches add`, `task touches remove`) | `entity_links` is cross-entity by design; polyrepo `touches`/`derives-from` edges legitimately cross scopes. |
| `*_add` / `*_create` (e.g. `task add`, `plan create`, `question add`, `scenario add`, `decision add`, `artifact add`, `ext register`) | New entity resolves its own `--scope` through the write resolver; `--plan` / `--parent` are references, not scope inheritance. |
| `sync push --all` / `sync pull --all` | Bulk fan-outs that the operator opts into explicitly via `--all`. |
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
planar init [--name <text>] [--db <path>]
```

**Description:** Idempotently ensure the config file exists (via `config init`), apply embedded migrations via goose (embed.FS, library mode) against the configured database (creating it if absent), then register the current working directory as a project if it is not already registered. Emits a summary of schema version and project id. Order: ensure config → apply migrations → create project row.

**Workspace-shape guardrail:** when cwd has no `.git` of its own but contains one or more immediate child directories that do, `planar init` refuses with a hint pointing at `planar workspace init`. A bare init in a polyrepo workspace directory would otherwise register a semantically-wrong project row for the workspace itself. Pass `--allow-no-repo` (alias `--force`) to override and register the non-repo cwd as a standalone project anyway. See [Domain: `workspace`](#domain-workspace) and [concepts.md § Workspace](concepts.md#workspace).

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--name <text>` | Human-readable project name. | Basename of current working directory. |
| `--db <path>` | (Also accepted as global flag.) Override database path. | `~/.planar/planar.db` |
| `--skip-project` | Apply migrations only; do not register a project. | off |
| `--allow-no-repo` | Proceed even when cwd has no `.git` but contains child repos (escape hatch for the workspace-shape guardrail). | off |
| `--force` | Alias for `--allow-no-repo`. | off |

**Output (human):**
```
planar initialized
  db:      ~/.planar/planar.db
  schema:  <integer version from schema_migrations>
  project: <slug> (id: <id>)
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
  project:billing  (root_path: /Users/mn/work/acme/billing)

To override, pass --scope <slug> to any verb, or cd into a
different registered scope.

(The active scope stack was removed in plan 153 M5; scope is now
derived from your current working directory.)
```

**Output (human, workspace root cwd):**
```
resolved scope (from cwd):
  org:acme  (root_path: /Users/mn/work/acme)
  project:billing  (root_path: /Users/mn/work/acme/billing)
  project:shipping  (root_path: /Users/mn/work/acme/shipping)

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
  "cwd": "/Users/mn/work/acme"
}
```

`source` is one of `"cwd"`, `"flag"`, or `"none"`. The outside-any-scope branch emits `"resolved_scopes": []` and `"source": "none"`.

**Schema effects:** Reads `associations`, `project_associations`, `projects`. No writes.

**Capture:** None (read-only).

---

### `planar scope use|pop|clear`

Removed in plan 153 M5. Invoking any of these verbs prints a corrective message and exits 1:

```
`planar scope use` was removed in plan 153 M5; the active scope stack is gone.
Pass --scope <slug> to individual verbs, or cd into a registered scope.
Run `planar scope show` to inspect the cwd-derived scope.
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

## Domain: `association` (alias: `assoc`)

Manages associations — the many-to-many tags that group repos into named scopes. Both `association` and `assoc` are valid subcommand names.

---

### `planar assoc list`

**Synopsis:**
```
planar assoc list [--kind <kind>] [--workbench]
```

**Description:** List all known associations.

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--kind <kind>` | Filter by association kind: `org`, `project`, `client`, `personal`, `ad-hoc`, `host`, `path`, `lang`. | all |
| `--workbench` | Only show workbench-enabled associations. | off |

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
planar assoc remove <slug> <repo-path>
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
planar assoc detect [--apply]
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
planar plan create <title> [--scope <scope>] [--parent <plan-id>] [--summary <text>]
```

**Description:** Create a new plan with the given title under the active (or specified) scope.

**Arguments:**

| Argument | Description |
|----------|-------------|
| `<title>` | Plan title (required, free-form text). |

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--scope <scope>` | Override scope for this entity. See [scope shorthand](#scope-shorthand). | Active scope. |
| `--parent <plan-id>` | Parent plan id for hierarchical plans. | none |
| `--summary <text>` | One-paragraph summary. May be a file path prefixed with `@`. | none |

**Output (human):**
```
plan 7: "Implement billing module"  [draft]  slug:implement-billing-module  (scope: association:3 [from active])
```

**Output (`--json`):**
```json
{"ok":true,"id":7,"title":"Implement billing module","slug":"implement-billing-module","status":"draft","scope_kind":"association","scope_id":3,"scope_source":"active"}
```

**Schema effects:** Inserts into `plans(scope_kind, scope_id, title, summary, status='draft', parent_plan_id)`.

**Capture:** Appends `session_entries` row with `prefix='action'` and body summarizing plan creation.

**Exit codes:**
- `1` — `--parent` plan id not found.
- `1` — scope not resolvable.

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
| `--scope <scope>` | Filter by scope. See [scope shorthand](#scope-shorthand). | Active scope. |
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
planar plan update <plan-id> [--title <text>] [--slug <slug>] [--summary <text>] [--status <status>]
```

**Description:** Update mutable fields on a plan.

**Scope guard:** Refuses when the operator's resolved write scope disagrees with the plan's stored scope. See [Cross-scope guard](#cross-scope-guard).

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--title <text>` | New title. | unchanged |
| `--slug <slug>` | New slug. Must remain unique within the plan's slug namespace. | unchanged |
| `--summary <text>` | New summary. May be `@<file>`. | unchanged |
| `--status <status>` | New status: `draft`, `active`, `paused`, `done`, `abandoned`. | unchanged |

**Schema effects:** Updates `plans(title, slug, summary, status, updated_at)`.

**Capture:** Appends `session_entries` row with `prefix='action'`.

**Exit codes:**
- `1` — plan id not found.
- `1` — invalid status value.

---

### `planar plan recompute-status`

**Synopsis:**
```
planar plan recompute-status (--plan <plan-id> | --all)
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
| `blocked` | Task status `blocked`. |

Same underlying selector as `planar-agent peek`, but returns the FULL bucket breakdown instead of just picking one row. This is the operator's read surface; agents call `planar-agent peek` / `pull`. There is no `planar agent` subcommand by design — agent observability lives on `planar-watch` (M8) and ritual writes live on `planar-agent`.

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
    "available": 3, "claimed": 1, "stale": 0, "blocked": 1, "done": 5,
    "note": "child-plan claim precedence not yet computed; see plan 85 plan_step precedence followup"
  }
}
```

`ClaimRow` is the canonical `agent_work_claims` row shape including the locality columns (`repo_root`, `branch`, `head_sha_at_claim`, `dirty_at_claim`) and worktree columns (`worktree_id`, `worktree_path`). The `summary.note` key is omitted when no parent (`plan` or `plan_step`) claim exists.

**Exit codes:**
- `0` — success, including empty / all-done plans.
- `1` — plan id not found, or invalid integer.

**Known limitation (followup):** the underlying selector walks `tasks.plan_id = ?` only. Multi-level claim precedence (a parent `plan_step` or `plan` claim covering every descendant task) is detected and surfaces in `summary.note` but does not yet rewrite the per-task buckets. Full recursive precedence per the tech spec § "Multi-level claim precedence" is followup work tracked on plan 85.

---

### `planar plan step add <plan-id> <body>`

**Synopsis:**
```
planar plan step add <plan-id> <body> [--after <ordinal>]
```

**Description:** Append a new step to a plan. By default inserts at the end. `--after <ordinal>` inserts after the specified ordinal, renumbering subsequent steps.

**Scope guard:** Refuses when the operator's resolved write scope disagrees with the parent plan's stored scope (the new step inherits its parent plan's scope). See [Cross-scope guard](#cross-scope-guard).

**Arguments:**

| Argument | Description |
|----------|-------------|
| `<plan-id>` | Target plan id. |
| `<body>` | Step description text. |

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--after <ordinal>` | Insert after this step ordinal. | appends at end |

**Output (human):**
```
step 3 added to plan 7
```

**Schema effects:** Inserts into `plan_steps(plan_id, ordinal, body, status='pending')`.

**Capture:** Appends `session_entries` row with `prefix='action'`.

**Exit codes:**
- `1` — plan id not found.

---

### `planar plan step done <step-id>`

**Synopsis:**
```
planar plan step done <step-id>
```

**Description:** Mark a plan step as done.

**Scope guard:** Refuses when the operator's resolved write scope disagrees with the parent plan's stored scope (plan steps inherit their parent plan's scope). See [Cross-scope guard](#cross-scope-guard).

**Schema effects:** Updates `plan_steps(status='done', updated_at)`.

**Capture:** Appends `session_entries` row with `prefix='action'`.

**Exit codes:**
- `1` — step id not found.

---

### `planar plan step skip <step-id>`

**Synopsis:**
```
planar plan step skip <step-id>
```

**Description:** Mark a plan step as skipped.

**Scope guard:** Refuses when the operator's resolved write scope disagrees with the parent plan's stored scope. See [Cross-scope guard](#cross-scope-guard).

**Schema effects:** Updates `plan_steps(status='skipped', updated_at)`.

**Capture:** Appends `session_entries` row with `prefix='action'`.

**Exit codes:**
- `1` — step id not found.

---

### `planar plan step link <step-id> <task-id>`

**Synopsis:**
```
planar plan step link <step-id> <task-id>
```

**Description:** Associate a plan step with the task that materializes it. Sets `plan_steps.task_id`. Use `entity_links` (via `plan link`) for richer relationships.

**Schema effects:** Updates `plan_steps(task_id)`.

**Exit codes:**
- `1` — step or task not found.

---

### `planar plan link <plan-id> <to-kind:to-id> --relationship <kind>`

**Synopsis:**
```
planar plan link <plan-id> <to-kind:to-id> --relationship <kind>
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
| `--relationship <kind>` | One of `derives-from`, `blocks`, `addresses`, `verifies`, `cites`, `supersedes`. | yes |

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
planar task add <title> [--plan <plan-id>] [--parent <task-id>] [--scope <scope>] [--priority <n>] [--body <text>] [--due <date>] [--next-action <text>]
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
| `--scope <scope>` | Override scope. See [scope shorthand](#scope-shorthand). | Active scope. |
| `--priority <n>` | Integer priority (lower is higher). | `100` |
| `--body <text>` | Task body / description. May be `@<file>`. | none |
| `--due <date>` | Due date in ISO 8601 format (e.g. `2026-05-15`). | none |
| `--next-action <text>` | The immediate next concrete action. Required for `resume validate` to pass. | none |
| `--no-auto-promote` | Skip the [plan-status auto-promotion invariant](concepts.md#plan) (plan 304) for this operation. Escape hatch for scripted migrations that don't intend the plan-level transition. | off |

**Output (human):**
```
task 42: "Implement payment gateway API"  [todo]  (priority: 50, plan: 7, scope: association:3 [from active])
```

**Output (`--json`):**
```json
{"ok":true,"id":42,"title":"Implement payment gateway API","status":"todo","plan_id":7,"priority":50,"scope_kind":"association","scope_id":3,"scope_source":"active"}
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
| `--scope <scope>` | Filter by scope. | Active scope. |
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
planar task update <task-id> [--title <text>] [--body <text>] [--status <status>] [--priority <n>] [--next-action <text>] [--due <date>] [--plan <plan-id>] [--force] [--reason <text>]
```

**Description:** Update mutable fields on a task.

**Scope guard:** Refuses when the operator's resolved write scope disagrees with the task's stored scope. See [Cross-scope guard](#cross-scope-guard).

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--title <text>` | New title. | unchanged |
| `--body <text>` | New body. May be `@<file>`. | unchanged |
| `--status <status>` | New status: `todo`, `doing`, `blocked`, `done`, `cancelled`. | unchanged |
| `--priority <n>` | New priority integer. | unchanged |
| `--next-action <text>` | Update the next concrete action. | unchanged |
| `--due <date>` | Update due date. | unchanged |
| `--plan <plan-id>` | Move task to a different plan (or `none` to detach). | unchanged |
| `--force` | Bypass the terminal-status guard. Required to move a `done` / `cancelled` task back to a non-terminal status; the transition is recorded in `task_reopens` with `source='task-update-force'`. Prefer `task reopen <id>` for the documented recovery path. | off |
| `--reason <text>` | Operator-supplied rationale recorded on the `task_reopens` audit row when `--force` triggers a terminal → non-terminal transition. | empty |
| `--no-auto-promote` | Skip the [plan-status auto-promotion invariant](concepts.md#plan) (plan 304) for this operation. Escape hatch for scripted migrations that don't intend the plan-level transition. | off |

**Schema effects:** Updates `tasks(title, body, status, priority, next_action, due_at, plan_id, updated_at)`. When `--force` triggers a terminal → non-terminal transition, also inserts into `task_reopens(task_id, from_status, to_status, source='task-update-force', reason)`.

**Capture:** Appends `session_entries` row with `prefix='action'`.

**Exit codes:**
- `1` — task not found.
- `1` — invalid status value.
- `1` — terminal-status transition attempted without `--force`.

---

### `planar task done <task-id>`

**Synopsis:**
```
planar task done <task-id>
```

**Description:** Mark a task as done. Shorthand for `task update --status done`.

**Scope guard:** Refuses when the operator's resolved write scope disagrees with the task's stored scope. See [Cross-scope guard](#cross-scope-guard).

**Schema effects:** Updates `tasks(status='done', updated_at)`.

**Capture:** Appends `session_entries` row with `prefix='action'`.

**Exit codes:**
- `1` — task not found.

---

### `planar task reopen <task-id>`

**Synopsis:**
```
planar task reopen <task-id> [--status todo|doing|blocked] [--reason <text>] [--scope <slug>]
```

**Description:** Reopen a task currently in a terminal status (`done` or `cancelled`). The dedicated recovery path for wrongly-marked tasks — see [`import`](#planar-import-repo-root) for the producer of the most common false-done case. Refuses if the task is not in a terminal status; use `task update --status <s>` for ordinary transitions.

**Scope guard:** Refuses when the operator's resolved write scope disagrees with the task's stored scope. See [Cross-scope guard](#cross-scope-guard).

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--status <status>` | Non-terminal status to move the task to: `todo`, `doing`, `blocked`. | `todo` |
| `--reason <text>` | Operator-supplied rationale recorded on the `task_reopens` audit row. | empty |
| `--scope <slug>` | Scope override for the cross-scope guard. | cwd-derived |

**Schema effects:**
- Updates `tasks(status, updated_at)`.
- Inserts into `task_reopens(task_id, from_status, to_status, source='task-reopen', reason)`.

**Capture:** Appends `session_entries` row with `prefix='action'`.

**Exit codes:**
- `1` — task not found.
- `1` — task is not currently in a terminal status.

---

### `planar task block <task-id> --on <task-id>`

**Synopsis:**
```
planar task block <task-id> --on <task-id>
```

**Description:** Mark a task as blocked and record the blocking relationship in `entity_links`. Sets the blocked task's status to `blocked`.

**Scope guard:** Refuses when either the blocked task or the blocking task is in a scope that disagrees with the operator's resolved write scope. Both endpoints are guarded. See [Cross-scope guard](#cross-scope-guard).

**Options:**

| Flag | Description | Required |
|------|-------------|----------|
| `--on <task-id>` | The task that is blocking. | yes |

**Schema effects:**
- Updates `tasks(status='blocked', updated_at)` for the blocked task.
- Inserts into `entity_links(from_kind='task', from_id=<task-id>, to_kind='task', to_id=<on-task-id>, relationship='blocks')`.

**Capture:** Appends `session_entries` row with `prefix='action'`.

**Exit codes:**
- `1` — either task id not found.

---

### `planar task link <task-id> <to-kind:to-id> --relationship <kind>`

**Synopsis:**
```
planar task link <task-id> <to-kind:to-id> --relationship <kind>
```

**Description:** Create an `entity_links` row from a task to another entity.

**Arguments:** Same pattern as `plan link`. Valid `to-kind` values: `plan`, `plan_step`, `task`, `question`, `test_scenario`, `artifact`, `decision`, `session`.

**Options:**

| Flag | Description | Required |
|------|-------------|----------|
| `--relationship <kind>` | One of `derives-from`, `blocks`, `addresses`, `verifies`, `cites`, `supersedes`. | yes |

**Schema effects:** Inserts into `entity_links(from_kind='task', from_id, to_kind, to_id, relationship)`.

**Capture:** Appends `session_entries` row with `prefix='action'`.

---

### `planar task touches add <task-id> <repo-slug>`

**Synopsis:**
```
planar task touches add <task-id> <repo-slug>
```

**Description:** Record that a task touches the given repo (cross-repo dependency). Inserts an `entity_links(relationship='touches', from_kind='task', to_kind='repo')` row. The repo slug must be registered via `planar init` (present in `projects`). If a touches link already exists between this task and this repo, the command surfaces a clean user error from the UNIQUE constraint — re-adding is not silently no-op'd; remove the link first with `planar task touches remove` if you want to verify or reset it.

**Arguments:**

| Argument | Description |
|----------|-------------|
| `<task-id>` | Task to annotate (required). |
| `<repo-slug>` | Repo slug to link as touched (required). |

**Output (`--json`):**
```json
{"ok":true,"task_id":42,"repo_id":7,"repo_slug":"acme/protos"}
```

**Schema effects:** Inserts into `entity_links(from_kind='task', from_id=<task-id>, to_kind='repo', to_id=<repo-id>, relationship='touches')`.

**Capture:** Appends `session_entries` row with `prefix='action'`.

**Exit codes:**
- `1` — task not found.
- `1` — repo slug not found (not registered).
- `1` — link already exists (duplicate constraint).

---

### `planar task touches remove <task-id> <repo-slug>`

**Synopsis:**
```
planar task touches remove <task-id> <repo-slug>
```

**Description:** Remove a touches link between a task and a repo. Deletes the `entity_links(relationship='touches')` row. Returns an error if no such link exists.

**Arguments:**

| Argument | Description |
|----------|-------------|
| `<task-id>` | Task to update (required). |
| `<repo-slug>` | Repo slug to remove from touches (required). |

**Output (`--json`):**
```json
{"ok":true,"task_id":42,"repo_id":7,"repo_slug":"acme/protos"}
```

**Schema effects:** Deletes from `entity_links(from_kind='task', from_id=<task-id>, to_kind='repo', to_id=<repo-id>, relationship='touches')`.

**Capture:** Appends `session_entries` row with `prefix='action'`.

**Exit codes:**
- `1` — task not found.
- `1` — repo slug not found.
- `1` — link does not exist.

---

## Domain: `question`

Questions represent open uncertainties surfaced during work. They carry a lifecycle (`open` → `answered` / `wontfix`) and can be linked to tasks, plans, and artifacts.

---

### `planar question add <title>`

**Synopsis:**
```
planar question add <title> [--body <text>] [--scope <scope>]
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
| `--scope <scope>` | Override scope. | Active scope. |

**Output (human):**
```
question 3: "What is the Stripe API rate limit?"  [open]  (scope: association:3 [from active])
```

**Output (`--json`):**
```json
{"ok":true,"id":3,"title":"What is the Stripe API rate limit?","status":"open","scope_kind":"association","scope_id":3,"scope_source":"active"}
```

**Schema effects:** Inserts into `questions(scope_kind, scope_id, title, body, status='open')`.

**Capture:** Appends `session_entries` row with `prefix='question'`.

**Exit codes:**
- `1` — scope not resolvable.

---

### `planar question answer <question-id> <answer>`

**Synopsis:**
```
planar question answer <question-id> <answer>
```

**Description:** Provide an answer to an open question, transitioning its status to `answered`. The `<answer>` argument may be inline text or `@<file>` to read from a file.

**Arguments:**

| Argument | Description |
|----------|-------------|
| `<question-id>` | Id of the question to answer. |
| `<answer>` | Answer text or `@<file>`. |

**Schema effects:** Updates `questions(status='answered', answer_body=<answer>, answered_at=now(), updated_at)`.

**Capture:** Appends `session_entries` row with `prefix='decision'` (answers are resolved decisions).

**Exit codes:**
- `1` — question not found.
- `1` — question is already answered or wontfix.

---

### `planar question wontfix <question-id>`

**Synopsis:**
```
planar question wontfix <question-id>
```

**Description:** Mark a question as not going to be answered (wontfix). Use for questions that are no longer relevant or explicitly deferred.

**Schema effects:** Updates `questions(status='wontfix', updated_at)`.

**Capture:** Appends `session_entries` row with `prefix='note'`.

**Exit codes:**
- `1` — question not found.

---

### `planar question list`

**Synopsis:**
```
planar question list [--scope <scope>] [--open] [--status <status>] [--touches <repo-slug>]
```

**Description:** List questions matching the given filters.

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--scope <scope>` | Filter by scope. | Active scope. |
| `--open` | Shorthand for `--status open`. | off |
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
planar question show <question-id>
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
planar question link <question-id> <to-kind:to-id> --relationship <kind>
```

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
planar scenario add <title> [--body <text>] [--related <artifact-id>] [--scope <scope>]
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
| `--scope <scope>` | Override scope. | Active scope. |

**Output (human):**
```
scenario 9: "Stripe webhook idempotency"  [draft]  (scope: association:3 [from active])
```

**Output (`--json`):**
```json
{"ok":true,"id":9,"title":"Stripe webhook idempotency","status":"draft","scope_kind":"association","scope_id":3,"scope_source":"active"}
```

**Schema effects:** Inserts into `test_scenarios(scope_kind, scope_id, title, body, status='draft', related_artifact_id)`.

**Capture:** Appends `session_entries` row with `prefix='action'`.

**Exit codes:**
- `1` — artifact id not found.

---

### `planar scenario verify <scenario-id> --outcome <outcome>`

**Synopsis:**
```
planar scenario verify <scenario-id> --outcome <outcome> [--summary <text>]
```

**Description:** Record the outcome of running a scenario. Transitions status to `verified` (on pass) or `failing` (on fail). Does not execute the scenario — execution is the agent's job.

**Arguments:**

| Argument | Description |
|----------|-------------|
| `<scenario-id>` | Scenario id. |

**Options:**

| Flag | Description | Required |
|------|-------------|----------|
| `--outcome <outcome>` | One of `pass`, `fail`, `error`, `skipped`. | yes |
| `--notes <text>` | Optional notes on the run. May be `@<file>`. | no |

**Schema effects:**
- Updates `test_scenarios(last_run_at=now(), last_outcome=<outcome>, updated_at)`.
- Status transitions: `pass` → `verified`; `fail` → `failing`; `error` stays at current status; `skipped` stays at current status.

**Capture:** Appends `session_entries` row with `prefix='observation'`.

**Exit codes:**
- `1` — scenario not found.
- `1` — outcome not in allowed values.

---

### `planar scenario list`

**Synopsis:**
```
planar scenario list [--scope <scope>] [--status <status>] [--related <artifact-id>] [--touches <repo-slug>]
```

**Description:** List test scenarios.

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--scope <scope>` | Filter by scope. | Active scope. |
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
planar scenario show <scenario-id>
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
planar scenario retire <scenario-id>
```

**Description:** Mark a scenario as retired (no longer relevant).

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
planar decision add <title> --body <text> [--rationale <text>] [--scope <scope>]
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
| `--scope <scope>` | Override scope. | Active scope. |

**Output (human):**
```
decision 5: "Use Stripe as payment processor"  [proposed]  (scope: association:3 [from active])
```

**Output (`--json`):**
```json
{"ok":true,"id":5,"title":"Use Stripe as payment processor","status":"proposed","scope_kind":"association","scope_id":3,"scope_source":"active"}
```

**Schema effects:** Inserts into `decisions(scope_kind, scope_id, title, body, rationale, status='proposed', session_id=<current session id>)`. If no active session exists when this command runs, one is auto-created per the [Capture Behavior](#capture-behavior) rule before insertion, so `decisions.session_id` is always non-null on entries created via the CLI.

**Capture:** Appends `session_entries` row with `prefix='decision'`.

---

### `planar decision accept <decision-id>`

**Synopsis:**
```
planar decision accept <decision-id>
```

**Description:** Mark a decision as accepted.

**Schema effects:** Updates `decisions(status='accepted', decided_at=now(), updated_at)`.

**Capture:** Appends `session_entries` row with `prefix='decision'`.

**Exit codes:**
- `1` — decision not found.

---

### `planar decision supersede <decision-id> --by <decision-id>`

**Synopsis:**
```
planar decision supersede <decision-id> --by <decision-id>
```

**Description:** Mark a decision as superseded by a newer decision.

**Scope guard:** Refuses when either the old decision or the new (`--by`) decision is in a scope that disagrees with the operator's resolved write scope. Both endpoints are guarded. See [Cross-scope guard](#cross-scope-guard).

**Options:**

| Flag | Description | Required |
|------|-------------|----------|
| `--by <decision-id>` | The newer decision that supersedes this one. | yes |

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
planar decision withdraw <decision-id>
```

**Description:** Mark a decision as withdrawn.

**Schema effects:** Updates `decisions(status='withdrawn', updated_at)`.

**Capture:** Appends `session_entries` row with `prefix='decision'`.

---

### `planar decision list`

**Synopsis:**
```
planar decision list [--scope <scope>] [--status <status>]
```

**Description:** List decisions.

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--scope <scope>` | Filter by scope. | Active scope. |
| `--status <status>` | Filter: `proposed`, `accepted`, `superseded`, `withdrawn`. Repeatable. | `proposed,accepted` |

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
planar decision show <decision-id>
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
planar artifact add <title> --kind <kind> [--body <text>] [--from-file <path>] [--source-path <path>] [--scope <scope>] [--plan <plan-id>]
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
| `--scope <scope>` | Override scope. | Active scope. |
| `--status <status>` | Initial status: `draft`, `active`. | `draft` |
| `--plan <plan-id>` | Attach the artifact to this plan via a `derives-from` entity link. Applied atomically in the same transaction as the artifact insert. | none |

**Output (human):**
```
artifact 3: "Billing Tech Spec"  [tech_spec, draft]  (scope: association:3 [from active])
```

**Output (`--json`):**
```json
{"ok":true,"id":3,"title":"Billing Tech Spec","kind":"tech_spec","status":"draft","scope_kind":"association","scope_id":3,"scope_source":"active"}
```

**Schema effects:** Inserts into `artifacts(scope_kind, scope_id, kind, title, body, source_path, status)`. When `--plan` is set, also inserts into `entity_links(from_kind='artifact', from_id=<new>, to_kind='plan', to_id=<plan-id>, relationship='derives-from')`.

**Capture:** Appends `session_entries` row with `prefix='action'`.

**Exit codes:**
- `1` — invalid kind value, `--from-file` and `--body` both specified, or plan not found.

---

### `planar artifact show <artifact-id>`

**Synopsis:**
```
planar artifact show <artifact-id>
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
planar artifact list [--scope <scope>] [--kind <kind>] [--status <status>]
```

**Description:** List artifacts.

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--scope <scope>` | Filter by scope. | Active scope. |
| `--kind <kind>` | Filter by kind. Repeatable. | all |
| `--status <status>` | Filter: `draft`, `active`, `superseded`, `retired`. Repeatable. | `draft,active` |

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
planar artifact update <artifact-id> [--title <text>] [--body <text>] [--status <status>] [--source-path <path>]
```

**Description:** Update mutable fields on an artifact.

**Scope guard:** Refuses when the operator's resolved write scope disagrees with the artifact's stored scope. See [Cross-scope guard](#cross-scope-guard).

**Schema effects:** Updates `artifacts(title, body, status, source_path, updated_at)`.

**Capture:** Appends `session_entries` row with `prefix='action'`.

**Exit codes:**
- `1` — artifact not found.
- `1` — invalid status value.

---

### `planar artifact link <artifact-id> <to-kind:to-id> --relationship <kind>`

**Synopsis:**
```
planar artifact link <artifact-id> <to-kind:to-id> --relationship <kind>
```

**Description:** Create an `entity_links` row from an artifact to another entity.

**Schema effects:** Inserts into `entity_links(from_kind='artifact', from_id, to_kind, to_id, relationship)`.

**Capture:** Appends `session_entries` row with `prefix='action'`.

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
planar demote <kind:id> [--from <association-slug>]
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
- `1` — user error (plan not found, bad argument, workbench root not writable).
- `2` — system error (DB, I/O).
- `3` — conflict(s) detected (status / sync / pull only; resolve to continue).

---

### `planar workbench push <plan>`

**Synopsis:**
```
planar workbench push <plan>
```

**Description:** Apply DB→FS changes for the named anchor plan. Each entity linked to the
plan is rendered as a Markdown file with YAML front matter. Files that match the last-synced
hash are skipped (no-op). Files are written atomically.

**Arguments:**

| Argument | Description |
|----------|-------------|
| `<plan>` | Anchor plan ID (numeric) or slug. |

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
- `1` — plan not found or is not a top-level plan.
- `2` — workbench root not writable or I/O error.

---

### `planar workbench pull <plan>`

**Synopsis:**
```
planar workbench pull <plan>
```

**Description:** Apply FS→DB changes for the named anchor plan. Each Markdown file in the
feature directory is parsed; if its content hash differs from the manifest the entity is
updated in the DB. New files with valid front matter are inserted as tasks and linked to the
anchor plan via `derives-from`. Deleted files (present in manifest, absent on disk) mark the
entity soft-deleted. Malformed files are reported but not auto-inserted or auto-deleted.

Conflicts (both FS and DB changed since last sync) are surfaced; they are not resolved
automatically. Use `workbench resolve` to settle them.

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
- `1` — plan not found.
- `2` — I/O error.
- `3` — one or more conflicts detected.

---

### `planar workbench status [<plan>]`

**Synopsis:**
```
planar workbench status [<plan>]
```

**Description:** Classify all (file, entity) pairs for one plan (or all plans with FS trees
if no plan argument is given) without applying any changes. Reports each file as one of:
`no-op`, `FS→DB`, `DB→FS`, `conflict`, `new-on-FS`, `deleted-on-FS`, `malformed`.

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
- `1` — plan not found.
- `3` — one or more conflicts detected.

---

### `planar workbench resolve <event-id> [--prefer fs|db]`

**Synopsis:**
```
planar workbench resolve <event-id> [--prefer fs|db]
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
planar workbench sync <plan>
```

**Description:** Full bidirectional reconciliation for the named anchor plan. Applies
non-conflicting FS→DB and DB→FS changes in a single pass. Conflicts are surfaced (exit 3)
and must be resolved with `workbench resolve` before the next sync will be clean.

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
- `1` — plan not found.
- `2` — I/O or DB error.
- `3` — one or more conflicts detected.

---

### `planar workbench archive <plan>`

**Synopsis:**
```
planar workbench archive <plan>
```

**Description:** Remove the FS tree for the named anchor plan's feature directory. The
database is not touched — all entity rows and manifest rows are retained. If the parent
association directory becomes empty after removal, it is also removed (best-effort).

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

### `planar workbench restore <plan>`

**Synopsis:**
```
planar workbench restore <plan>
```

**Description:** Recreate the FS tree from the DB for the named anchor plan. Idempotent
against existing trees — existing files are overwritten atomically if they differ from the
DB render. After restore the manifest and `.sync` file are fully up to date; a subsequent
`workbench status` reports all no-ops.

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

### `planar workbench list`

**Synopsis:**
```
planar workbench list
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

**Description:** Render the workbench files for the named anchor plan and push the rendered content to a registered external operational system (Jira, GitHub Issues, GitHub Projects) via the adapter layer. The destination system, credentials, and per-entity projection template come from the system registration (see `planar ext list` / `planar ext create`).

For richer per-entity counterpart creation (epics, issues, sub-issues with parent/child links) walking the full plan tree, use `planar ext propagate <plan-id> --system <slug>` instead. `workbench publish` pushes the rendered Markdown body; `ext propagate` creates one external counterpart per entity in the plan subtree.

**Arguments:**

| Argument | Description |
|----------|-------------|
| `<plan-id>` | Anchor plan ID (numeric) or slug. |

**Options:**

| Flag | Default | Description |
|------|---------|-------------|
| `--system <slug>` | _(required)_ | External system slug (must already be registered via `planar ext create`). |
| `--json` | `false` | Emit a JSON result envelope on stdout. |

**Schema effects:** None on the local database (other than recording the resulting external link via the adapter, if the adapter persists one). The destination is the external system.

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

This helper is consumed by `/pl-spec-draft` and `/pl-spec-ingest` to auto-register question entities during spec authoring and re-ingestion. Calling it directly is useful for auditing which open questions a spec body currently declares before running the ingest cycle.

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
planar workspace init [--name <text>] [--slug <text>] [--scan <N>] [--no-scan] [--enrich]
```

**Description:** Create an org association for the current working directory and register every immediate child directory containing a `.git` as a project member of that org. Refuses if cwd has its own `.git` (use `planar init` instead) or if cwd contains zero `.git` children (a workspace must contain repos). Idempotent on re-run: an existing org with the same slug is reused; existing projects keep their state; missing memberships are added.

After the org + projects are committed, init runs a pipeline pass that (1) builds the static routing table, (2) regenerates AGENTS.md from the template, and (3) installs `AGENTS.md` / `CLAUDE.md` symlinks at the workspace root (copy fallback on filesystems that reject symlinks). Pipeline failures do not roll back the DB writes; they surface as a stderr warning with a hint to re-run `planar workspace routing build && planar workspace regenerate`.

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--name <text>` | Human-readable org name. | cwd basename |
| `--slug <text>` | Org slug. | Derived from cwd basename. |
| `--scan <N>` | Walk N levels deep when scanning for child repos. | `1` (immediate children only) |
| `--no-scan` | Skip the post-init pipeline (routing build + regenerate + symlinks). | off |
| `--enrich` | Merge cached LLM enrichment results into the routing table (equivalent to `routing build --enrich`). Cannot combine with `--no-scan`. | off |

**Output (human):**
```
created org:work (Work)
  ├─ project:repo-a   [/Users/mn/work/repo-a]   (auto-created, member-of org:work)
  ├─ project:repo-b   [/Users/mn/work/repo-b]   (auto-created, member-of org:work)
  └─ project:repo-c   [/Users/mn/work/repo-c]   (auto-created, member-of org:work)

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
  "projects": [{"slug":"repo-a","path":"/Users/mn/work/repo-a","created":true,"membership_created":true}],
  "pipeline": {
    "skipped":     false,
    "routing":    {"project_count":3,"cross_repo_deps":1,"enrich_enabled":false,"enrich_misses":0},
    "regenerate": {"agents_path":"/Users/mn/.planar/workspaces/1/AGENTS.md","bytes":1842},
    "symlinks":   {"strategy":"symlink","installed":["AGENTS.md","CLAUDE.md"]}
  }
}
```

**Schema effects:** Inserts/preserves `associations(kind='org')`, `projects`, `project_associations`. Writes `~/.planar/workspaces/<org_id>/AGENTS.md` and `routing-table.json`. Installs symlinks (or copies) at the workspace root.

**Exit codes:**
- `0` — success.
- `1` — cwd has its own `.git`; or cwd contains zero `.git` children; or `--no-scan` was combined with `--enrich`.
- `2` — migration, DB, or pipeline I/O failure.

---

### `planar workspace doctor`

**Synopsis:**
```
planar workspace doctor
```

**Description:** Walk every `associations` row with `kind=org` and reconcile its on-disk state. For each org, doctor verifies the state directory exists (creating it if missing) and that the `AGENTS.md` / `CLAUDE.md` symlinks at the workspace root point at the canonical target (reinstalling them if not). Missing `AGENTS.md` or `routing-table.json` are reported as `missing` with a hint to run `planar workspace regenerate` — doctor does not regenerate content itself. Idempotent: a second invocation against the same fleet produces an `ok` summary per org. Orgs whose `config_json` has no `root_path` skip symlink repair with a warning rather than guessing from cwd.

**Output (human):**
```
fix: created state dir /Users/mn/.planar/workspaces/1
fix: reinstalled symlink /Users/mn/work/AGENTS.md → /Users/mn/.planar/workspaces/1/AGENTS.md
org:work repaired 2 issues
org:side ok
```

**Output (`--json`):**
```json
{"orgs":[{"slug":"work","org_id":1,"issues_found":2,"issues_repaired":[
  {"kind":"fix","detail":"created state dir /Users/mn/.planar/workspaces/1"},
  {"kind":"fix","detail":"reinstalled symlink /Users/mn/work/AGENTS.md → ..."}
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

**Description:** Scan the workspace's member projects and rewrite `<state-dir>/routing-table.json`. Deterministic and pure-Go by default: reads each project's README first paragraph (becomes `summary`), runs manifest detection for capability tags (loaded from `~/.planar/templates/workspace-capabilities.toml` when present), infers cross-repo dependencies from `go.mod` replace directives and `package.json` workspace deps, walks the repo for a language census and entry-point detection, and refreshes the `planar_focus` block via live DB queries (open tasks, open questions, active plans). Operator overrides from `<state-dir>/routing-table-overrides.json` are merged on every build and always win.

With `--enrich`, the builder additionally consults the workspace-enrichment cache at `~/.planar/cache/workspace-enrichment/<org_id>/` and merges any LLM result whose fingerprint matches the project's current content. Cache misses emit a per-project hint to run the `pl-workspace-scan` skill. If the workspace's `config.toml` declares an `enrich_command`, the Go side shells out to it on cache miss (stdin = Request JSON, stdout = Result JSON, bounded by `enrich_timeout_seconds`, default 30); validation failures are warnings, never build failures. Merge precedence (highest first): manual overrides → LLM enrichment → static signals.

**Output (human):**
```
built /Users/mn/.planar/workspaces/1/routing-table.json (3 projects, 1 cross-repo deps)
```

**Output (`--json`):**
```json
{"path":"/Users/mn/.planar/workspaces/1/routing-table.json","projects":3,"dependency_edges":1,"enrich_enabled":false,"enrich_misses":0}
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
    path:         /Users/mn/work/repo-a
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

**Description:** Render the canonical `AGENTS.md` for a workspace. Reads `<state-dir>/routing-table.json` (produced by `routing build`), re-fetches live cross-repo plans and open questions from the database (these go stale fast, so the regenerator queries them every run rather than trusting cached counts), renders the AGENTS.md template (operator-installed at `~/.planar/templates/doc-prompts/agents.md` or the embedded fallback), and atomically writes the result to `<state-dir>/AGENTS.md`. Plan 96's drift manifest (`<state-dir>/.manifest-docs`) is rebuilt alongside the AGENTS.md write so hand-edits surface as drift on the next manifest check. Errors with an explicit "run routing build first" hint when `routing-table.json` is missing.

**Output (human):**
```
regenerated AGENTS.md for org:work (3 projects, 1842 bytes)
```

**Output (`--json`):**
```json
{"agents_path":"/Users/mn/.planar/workspaces/1/AGENTS.md","project_count":3,"bytes_written":1842}
```

**Schema effects:** Reads `associations`, `plans`, `tasks`, `questions`. Writes `<state-dir>/AGENTS.md` and `<state-dir>/.manifest-docs` atomically. The symlinks at the workspace root are not touched (they already point at the canonical target).

**Exit codes:**
- `0` — success.
- `1` — workspace not resolvable; or `routing-table.json` missing.
- `2` — DB, template, or filesystem error.

---

## Domain: `ext`

Commands for registering and interacting with external operational plane systems (Jira, GitHub Issues, etc.).

---

### `planar ext register jira <slug>`

**Synopsis:**
```
planar ext register jira <slug> --base-url <url> --project <key> --auth-env <var>
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

### `planar ext register github <slug>`

**Synopsis:**
```
planar ext register github <slug> --project <owner/repo> [--auth-env <var>]
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

### `planar ext list`

**Synopsis:**
```
planar ext list
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

### `planar ext test <slug>`

**Synopsis:**
```
planar ext test <slug>
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

### `planar ext create <system-slug> --from <kind:id>`

**Synopsis:**
```
planar ext create <system-slug> --from <kind:id> [--type <issue-type>] [--role <link-role>] [--sync <direction>]
```

**Description:** Create a counterpart for an existing local entity on the named external system, then record the link. This is the automation entry point for surfacing local work to the operational plane.

**Scope guard:** Refuses when the operator's resolved write scope disagrees with the `--from` entity's stored scope. See [Cross-scope guard](#cross-scope-guard).

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--from <kind:id>` | Source local entity. | Required. |
| `--type <issue-type>` | External issue type (e.g. `Epic`, `Story` for Jira). | System default. |
| `--role <link-role>` | Link role: `mirror`, `parent`, `child`, `reference`. | `mirror` |
| `--sync <direction>` | Sync direction: `read-only`, `write-back`, `two-way`. | `two-way` |

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

### `planar ext propagate <plan>`

**Synopsis:**
```
planar ext propagate <plan> [--system <slug>] [--dry-run] [--restrategize [--yes]]
                            [--github-strategy <value>]
                            [--verify-counterparts [--unlink | --recreate]]
```

**Description:** Push a feature tree to the operational plane. Creates external counterparts (Epic/Story/Sub-task on Jira; parent-issue/sub-issues on GitHub) for the anchor plan and all descendant child plans and tasks that do not yet have a `mirror` link. The propagation strategy is selected per [ADR-0006](adr/0006-github-feature-mapping.md): Jira always uses the epic hierarchy; GitHub uses parent-issue (single-repo), Projects v2 (multi-repo), or zero-repo fallback.

**Strategy stickiness (Phase C):** The chosen strategy is cached on `external_links.config_json` of the anchor plan at first propagation. Subsequent reruns honor the cached strategy even if the repo count later changes. Strategy is not re-evaluated automatically; use `--restrategize` to rebuild.

**Scope guard:** Refuses when the operator's resolved write scope disagrees with the anchor plan's stored scope. See [Cross-scope guard](#cross-scope-guard).

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
| `--restrategize` | Force fresh strategy detection; prompts for confirmation if the strategy changes. On confirmation, prior counterparts are abandoned (NOT deleted from the remote) and `sync_events(outcome='strategy-abandoned')` rows are written for audit. Fresh propagation then proceeds under the new strategy. | `false` |
| `--github-strategy <value>` | Override ADR-0006 auto-detection at first propagation for GitHub systems. Accepted values: `parent-issue`, `projects-v2`, `tracking-issue`. The chosen value is cached on `external_links.config_json` identically to auto-detected strategies; subsequent propagations honor the cache. GitHub-only — rejected when the target system is not `github-issues`. Mutually exclusive with `--restrategize`. | (off) |
| `--yes` | Auto-confirm the `--restrategize` prompt without interactive input. No effect without `--restrategize`. | `false` |
| `--verify-counterparts` | Probe the remote to confirm every already-linked entity still exists. Missing counterparts (404) are reported as `Missing` and `sync_events(outcome='counterpart-missing')` rows are written. Off by default — probing on every run is expensive on large features. | `false` |
| `--unlink` | Remove `external_links` rows for missing counterparts (requires `--verify-counterparts`). The entity is then treated as "to create" on the next propagation. Mutually exclusive with `--recreate`. | `false` |
| `--recreate` | Remove the `external_links` row for missing counterparts and immediately re-create them (requires `--verify-counterparts`). Mutually exclusive with `--unlink`. | `false` |
| `--sync <direction>` | Sync direction applied to every `external_links` row created by this propagation. Accepted values: `read-only`, `write-back`, `two-way`. **Behavior change from prior versions:** the propagate flow previously defaulted to `two-way`; the new default is `read-only`. Users with downstream tooling that depended on the implicit two-way write must pass `--sync two-way` explicitly going forward. | `read-only` |

**Output (human):**
```
propagate plan:7 → my-jira (jira-epic): 9 created, 0 skipped, 0 failed
  created   plan:7 "Add Checkout RPC" → MOCK-1
  created   plan:8 "Protos Changes" → MOCK-2
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
{"ok":true,"plan_id":7,"system":"my-jira","strategy":"jira-epic","created":9,"skipped":0,"failed":0}
```

The JSON shape gains `verified`, `abandoned`, `partial`, `missing`, and `warnings` fields (all zero/empty on a clean propagation).

**Schema effects:** Inserts into `external_links(link_role='mirror')` and `sync_events(outcome='ok')` per created entity. Each entity's writes are in a separate transaction. `--restrategize` writes `sync_events(outcome='strategy-abandoned')` per abandoned counterpart and deletes the corresponding `external_links` rows. `--verify-counterparts` writes `sync_events(outcome='counterpart-missing')` for missing entities. Partial entity failures write `sync_events(outcome='partial')` for resumability.

**Capture:** Session entries not appended (propagation is a bulk operation; `sync_events` rows serve as the audit trail).

**Exit codes:**
- `0` — success (including a fully-skipped rerun or fully-verified rerun).
- `1` — plan not found, no registered system, entity-level failures, or missing counterparts reported without `--unlink` / `--recreate`.
- `2` — adapter or database failure.

---

## Domain: `link` / `unlink`

Links record relationships between local entities and external tickets. These are top-level commands matching the tech spec's literal CLI examples, not nested under `ext`.

---

### `planar link <kind:id> --to <system-slug>:<external-id>`

**Synopsis:**
```
planar link <kind:id> --to <system-slug>:<external-id> [--role <link-role>] [--sync <direction>]
```

**Description:** Manually record an `external_links` row linking a local entity to an already-existing external ticket. Use this when the external ticket was created outside of `ext create`. Does not push any data to the external system.

**Scope guard:** Refuses when the operator's resolved write scope disagrees with the local entity's stored scope. See [Cross-scope guard](#cross-scope-guard).

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
| `--propagate` | After creating the link, propagate the anchor plan of the linked entity to its registered external system. Runs the equivalent of `planar ext propagate` against the top-level plan. | `false` |

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
planar unlink <link-id>
```

**Description:** Remove an `external_links` row by link id. Any associated `sync_events` rows are also deleted (cascade).

**Scope guard:** Refuses when the operator's resolved write scope disagrees with the local entity referenced by the link. See [Cross-scope guard](#cross-scope-guard).

**Schema effects:**
- Deletes from `external_links(id)`.
- Cascades to `sync_events` via FK.

**Capture:** Appends `session_entries` row with `prefix='action'`.

**Exit codes:**
- `1` — link id not found.

---

## Domain: `sync`

Sync commands pull and push data between the local plane and registered external systems. Sync is always on-demand; no background daemon.

---

### `planar sync pull <target>`

**Synopsis:**
```
planar sync pull <link-id | kind:id | --all> [--system <slug>] [--scope <slug>]
```

**Description:** Pull remote state for one or more links. Updates `external_links.last_synced_at` and mirrors selected fields onto the local entity if `sync_direction` permits. Records a `sync_events` row per link touched.

**Scope guard:** Single-target invocations (`<link-id>` or `<kind:id>`) refuse when the operator's resolved write scope disagrees with the local entity referenced by any resolved link. `--all` invocations are not guarded (bulk fan-out is opt-in). See [Cross-scope guard](#cross-scope-guard).

**Arguments / Options:**

| Form | Description |
|------|-------------|
| `<link-id>` | Pull a specific link by id. |
| `<kind:id>` | Pull all links for the given local entity. |
| `--all` | Pull all links with `sync_direction` of `two-way` or `read-only`. |
| `--system <slug>` | Filter to links via a specific system (when used with `--all` or `kind:id`). |
| `--scope <slug>` | Filter to links within a specific scope. |

**Output (human):**
```
pulled 3 links
  link 7 (task:42 ↔ PROJ-1234): ok — title, status updated
  link 8 (plan:7 ↔ PROJ-100): noop — no remote changes
  link 9 (task:43 ↔ PROJ-1235): conflict — status diverged (local: done, remote: In Progress)
```

**Output (`--json`):** One object per link:
```json
{"link_id":7,"outcome":"ok","fields_changed":["title","status"]}
{"link_id":9,"outcome":"conflict","detail":"status: local=done remote=in-progress"}
```

**Schema effects:**
- Updates `external_links(last_synced_at, last_sync_status)` per link.
- Updates the local entity's mirrored fields on `ok`.
- Inserts into `sync_events(link_id, direction='pull', outcome, fields_changed, detail, at)` per link.

**Capture:** Appends `session_entries` row with `prefix='observation'`.

**Exit codes:**
- `0` — all links pulled (even if some were `noop`).
- `3` — at least one conflict detected.
- `2` — adapter error on one or more links.

---

### `planar sync push <target>`

**Synopsis:**
```
planar sync push <link-id | kind:id | --all> [--system <slug>]
```

**Description:** Push selected local fields to the remote system for one or more links. For comment and decision posts, appends rather than replaces. Includes the correlation footer on every push.

**Scope guard:** Single-target invocations (`<link-id>` or `<kind:id>`) refuse when the operator's resolved write scope disagrees with the local entity referenced by any resolved link. `--all` invocations are not guarded (bulk fan-out is opt-in). See [Cross-scope guard](#cross-scope-guard).

**Schema effects:**
- Calls adapter `update(external_id, fields)` or `comment(external_id, body)`.
- Inserts into `sync_events(link_id, direction='push', outcome, fields_changed, detail, at)`.
- Updates `external_links(last_synced_at, last_sync_status)`.

**Capture:** Appends `session_entries` row with `prefix='action'`.

**Exit codes:**
- `0` — all pushes succeeded.
- `2` — adapter error.

---

### `planar sync status`

**Synopsis:**
```
planar sync status [--scope <scope>] [--system <slug>]
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

### `planar sync resolve <event-id> --keep <side>`

**Synopsis:**
```
planar sync resolve <event-id> --keep <side>
```

**Description:** Resolve a sync conflict recorded in `sync_events`. `--keep local` keeps the local value and pushes it to the remote. `--keep remote` overwrites the local value with the remote value. Resolution is whole-entity; per-field resolution is not supported.

**Scope guard:** Refuses when the operator's resolved write scope disagrees with the local entity referenced by the link the event belongs to. See [Cross-scope guard](#cross-scope-guard).

**Arguments:**

| Argument | Description |
|----------|-------------|
| `<event-id>` | The `sync_events.id` of the conflict event to resolve. |

**Options:**

| Flag | Description | Required |
|------|-------------|----------|
| `--keep <side>` | `local` or `remote`. | yes |

**Output (human):**
```
conflict resolved: event 15 — kept local value for status
```

**Schema effects:**
- Reads `sync_events(outcome='conflict')` and associated `external_links`.
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
| `--budget <tokens>` | Token budget for the packet (recent activity is trimmed to fit). | `8000` |
| `--no-pull` | Skip the operational plane pull. Use with caution — packet may be stale. | off |

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
- `1` — task fails `resume validate` with `--no-pull` not set and a capture failure is detected.
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
  - operational sync stale (last pull: 3 days ago) → run: planar sync pull task:42
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

### `planar handoff validate <snapshot-id>`

**Synopsis:**
```
planar handoff validate <snapshot-id>
```

**Description:** Validate that the handoff anchored at the given snapshot is resume-ready. Checks the same criteria as `resume validate` plus snapshot presence and handoff status. On success, writes the `validated` state to the handoff row, making it eligible for `handoff consume`.

**Output:** Same structure as `resume validate` output.

**Schema effects:**
- Reads `context_snapshots`, `handoffs`, `tasks`.
- On success: updates `handoffs(status='validated', validated_at=strftime('%Y-%m-%dT%H:%M:%fZ','now'))` for the latest `pending` handoff anchored at `<snapshot-id>`.

**Capture:** None.

**Exit codes:**
- `0` — handoff is resume-ready; status updated to `validated`.
- `1` — validation failures found; status not changed.
- `1` — snapshot not found.
- `1` — no pending handoff found for this snapshot.

---

### `planar handoff list`

**Synopsis:**
```
planar handoff list [--status <status>] [--task <task-id>]
```

**Description:** List handoffs by status.

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--status <status>` | Filter: `pending`, `validated`, `consumed`, `abandoned`. Repeatable. | `pending` |
| `--task <task-id>` | Filter to handoffs for a specific task. | all |

**Output (human):**
```
id  snapshot  task  from-vendor  to-vendor  status   created-at
3   15        42    claude       codex      pending  2026-05-10T09:00Z
```

**Schema effects:** Reads `handoffs`, `context_snapshots`, `tasks`.

**Capture:** None.

---

### `planar handoff consume <handoff-id>`

**Synopsis:**
```
planar handoff consume <handoff-id> [--session <session-id>]
```

**Description:** Mark a handoff as consumed by the resuming session. Transitions `handoffs.status` from `pending` (or `validated`) to `consumed`. Typically called automatically by `resume` when it picks up a pending handoff, but exposed as an explicit command for agent scripts.

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--session <session-id>` | The session id of the resuming session. | Current session. |

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
planar capture session [--task <task-id>] [--vendor <vendor>] [--vendor-session-id <id>] [--model <model>]
```

**Description:** Explicitly open a new session row and make it the current active session for subsequent commands. Useful when automatic session creation behavior needs to be overridden (e.g. when starting a new agent process mid-task).

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

**Schema effects:** Inserts into `sessions(task_id, project_id, agent_id, vendor, vendor_session_id, model, started_at)`.

**Capture:** Appends `session_entries` row with `prefix='action'` marking session start.

---

### `planar capture end [<session-id>]`

**Synopsis:**
```
planar capture end [<session-id>] [--summary <text>]
```

**Description:** Close the current (or specified) session by setting `sessions.ended_at`. Does not capture a snapshot; use `handoff` for end-of-session snapshot capture. If `--summary` is supplied, also writes a human-readable summary of the session to `sessions.summary`.

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--summary <text>` | Human-readable session summary. May be `@<file>`. | none |

**Schema effects:** Updates `sessions(ended_at=now())`. If `--summary` is given, also updates `sessions(summary=<text>)`.

**Capture:** Appends final `session_entries` row with `prefix='note'` marking session end.

**Exit codes:**
- `1` — session not found or already ended.

---

### `planar capture note <body>`

**Synopsis:**
```
planar capture note <body> [--session <session-id>]
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
planar capture command <command> [--outcome <text>] [--session <session-id>]
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
planar capture file <path> [--role <description>] [--session <session-id>]
```

**Description:** Record that a file was read, written, or otherwise touched during the session, with an optional role description (e.g. "implementation target", "read for context", "avoided — too large").

**Schema effects:** Inserts into `session_entries(session_id, ordinal, prefix='file', body=<path + role>)`.

---

### `planar capture snapshot [<task-id>]`

**Synopsis:**
```
planar capture snapshot [<task-id>] [--note <text>]
```

**Description:** Produce a context snapshot for the current or named task mid-session, without creating a full handoff record. Useful for checkpointing state at meaningful points during long sessions.

**Schema effects:** Inserts into `context_snapshots(session_id, task_id, vendor, vendor_session_id, body, next_action)`. Reads `tasks.next_action` for the snapshot's `next_action` field.

**Capture:** Appends `session_entries` row with `prefix='note'`.

---

## Domain: `audit`

Audit commands produce cross-plane audit trails linking local sessions, decisions, and sync events to external tickets.

---

### `planar audit trail <link-id>`

**Synopsis:**
```
planar audit trail <link-id>
```

**Description:** Show every local session, decision, and sync event tied to the given external link, with timestamps and vendor identity. This is the inverse view: starting from an external ticket's link id, reconstruct the full history of local work that produced changes to it.

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
```

**Output (`--json`):**
```json
{
  "link_id":7,
  "entity_kind":"task","entity_id":42,
  "external_id":"PROJ-1234","system_slug":"acme-jira",
  "sessions":[{"id":101,"vendor":"claude","started_at":"...","summary":"..."},...],
  "decisions":[{"id":5,"title":"...","status":"accepted","decided_at":"..."},...],
  "sync_events":[{"id":15,"direction":"push","outcome":"ok","at":"..."},...]
}
```

**Schema effects:** Reads `external_links`, `sync_events`, `sessions`, `session_entries`, `decisions`, `entity_links`.

**Capture:** None.

**Exit codes:**
- `1` — link id not found.

---

### `planar audit session <session-id>`

**Synopsis:**
```
planar audit session <session-id>
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

## Domain: `health`

Health commands report the operational status of the Planar installation and the handoff readiness of in-flight tasks.

---

### `planar health`

**Synopsis:**
```
planar health [--json]
```

**Description:** Report database and handoff readiness health. Checks: database is reachable, schema version is current, `pragma integrity_check` passes, count of in-flight tasks not passing `resume validate`, count of pending handoffs older than the freshness window.

**Output (human):**
```
planar health

  db:            ~/.planar/planar.db  [ok]
  schema:        <version>  [current]
  integrity:     ok
  in-flight tasks:  5  (3 resumable, 2 NOT RESUMABLE)
  pending handoffs: 1  (0 stale)

overall: DEGRADED  (2 tasks not resumable)
```

**Output (`--json`):**
```json
{
  "db_path":"~/.planar/planar.db",
  "db_ok":true,
  "schema_current":true,
  "integrity_ok":true,
  "inflight_tasks":5,
  "resumable_tasks":3,
  "not_resumable_tasks":2,
  "pending_handoffs":1,
  "stale_handoffs":0,
  "overall":"degraded"
}
```

**Schema effects:** Reads `schema_migrations`, `tasks`, `context_snapshots`, `handoffs`.

**Capture:** None.

**Exit codes:**
- `0` — all checks pass.
- `1` — degraded (some tasks not resumable or stale handoffs).
- `2` — critical (database unreachable or integrity check failed).

---

## Domain: `dashboard`

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

The `stale` bucket aggregates both `status='stale'` rows and `status='active'` rows whose lease has expired without a reconcile pass.

**Schema effects:** Reads `plans`, `agent_work_claims`, `tasks`.

**Exit codes:**
- `0` — always (an empty dashboard is not an error).

---

## Domain: `links`

Internal cross-cutting entity relationships. The `entity_links` table stores typed relationships between any two Planar entities (e.g. a task cites an artifact, a plan blocks another plan). This domain is distinct from the top-level `link` / `unlink` commands, which operate on `external_links` (operational plane bindings to Jira, GitHub Issues, etc.).

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
| `--relationship <rel>` | Relationship type. Accepted values: `derives-from`, `blocks`, `addresses`, `verifies`, `cites`, `supersedes`, `touches`. | Required. |

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
31   to         blocks          task:38 "Implement retry logic"
```

**Output (`--json`):** One object per link:
```json
{"id":22,"from_kind":"task","from_id":42,"to_kind":"artifact","to_id":3,"relationship":"cites"}
{"id":31,"from_kind":"task","from_id":38,"to_kind":"task","to_id":42,"relationship":"blocks"}
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

### `planar links update <link-id>`

**Synopsis:**
```
planar links update <link-id> --sync <direction>
```

**Description:** Mutate the `sync_direction` column on an existing `external_links` row. Use this to change the sync direction for a link that was already created by `ext propagate`, `link`, or `ext create`. The change takes effect on the next `planar sync push` or `planar sync pull` invocation. An audit row is written to `sync_events` with `outcome='ok'` and a payload recording the old and new directions.

**Scope guard:** Refuses when the operator's resolved write scope disagrees with the local entity referenced by the link. See [Cross-scope guard](#cross-scope-guard).

**Arguments:**

| Argument | Description |
|----------|-------------|
| `<link-id>` | The `external_links.id` to update. |

**Options:**

| Flag | Description | Default |
|------|-------------|---------|
| `--sync <direction>` | New sync direction. Accepted values: `read-only`, `write-back`, `two-way`. | Required. |

**Output (human):**
```
link 7: sync_direction read-only → write-back
```

**Output (`--json`):**
```json
{"ok":true,"id":7,"prev":"read-only","new":"write-back"}
```

**Schema effects:** Updates `external_links(sync_direction)` for the given row. Inserts a `sync_events(outcome='ok')` audit row with the old and new direction in the payload. Both writes are in a single transaction.

**Capture:** Appends `session_entries` row with `prefix='action'`.

**Exit codes:**
- `0` — direction updated.
- `1` — link id not found, or `--sync` value is invalid.

---

## Domain: `help`

**Note:** `planar help` and `planar <command> --help` are provided by cobra; this section is preserved for discoverability.

---

### `planar help [<subcommand>]`

**Synopsis:**
```
planar help [<subcommand> [<sub-subcommand>]]
```

**Description:** Show help text. Without arguments, shows the top-level command list with one-line descriptions. With a subcommand argument, shows that subcommand's full usage. Equivalent to `planar <subcommand> --help`.

**Output:** Plain text to stdout. Not affected by `--json`.

**Exit codes:**
- `64` — subcommand not found.

---


## Domain: `spec`

Planning pipeline spec commands for decomposing workbench planning documents into a task graph.

---

### `planar spec ingest <plan>`

**Synopsis:**
```
planar spec ingest <plan> [--apply] [--apply-removals] [--format text|json] [--strict]
```

**Description:** Read `tech-spec.md`, `roadmap.md`, and (when present) `test-spec.md` from the anchor plan's workbench directory, compute the proposed diff against the current database state, and (optionally) commit additions and updates.

Default mode is **preview**: prints a tree-shaped diff and exits 0 without writing anything. `--apply` is required to commit changes.

A `coverage:` line follows the totals on every run. It reports how many tasks carry a `[slug:]`, how many slug-bearing tasks are verified by at least one test-spec scenario, and any orphan scenarios whose `**Verifies:**` line failed to parse. `--strict` promotes uncovered tasks and orphan scenarios from a printed warning into a non-zero exit.

`<plan>` may be a numeric plan id or a plan slug.

**Scope guard:** Refuses when the operator's resolved write scope disagrees with the anchor plan's stored scope (the verb materialises derived rows under the anchor's scope). See [Cross-scope guard](#cross-scope-guard).

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
| `--strict` | Reject the ingest (exit 1) when any slug-bearing task has no verifying scenario, or when any scenario has no parseable `**Verifies:**` line. | off |

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
  "summary": {"additions": 11, "updates": 0, "removals": 0}
}
```

**Schema effects:**

Reads:
- `plans` — anchor plan and existing child plans.
- `tasks` — existing tasks linked via `entity_links(relationship='derives-from')`.
- `decisions` — existing decisions linked via `entity_links(relationship='derives-from')`.
- `entity_links` — existing link rows for reconciliation.

Writes (only with `--apply`):
- `plans` — inserts child plans; updates anchor plan status (`draft` → `active` on first apply).
- `tasks` — inserts or updates tasks; cancels orphan tasks (only with `--apply-removals`).
- `decisions` — inserts or updates decisions.
- `test_scenarios` — inserts auto-drafted scenarios for non-trivial tasks.
- `entity_links` — inserts `derives-from` links (plan→anchor, task→plan, decision→anchor), `touches` links (task→repo), and `verifies` links (scenario→task).

**Capture:**
- Preview mode: one `session_entries` row with `prefix='read'` appended.
- Apply mode: one `session_entries` row with `prefix='action'` appended.

**Exit codes:**
- `0` — success (preview or apply).
- `1` — user error: plan not found, `--apply-removals` without `--apply`, malformed workbench files, `--strict` refused due to coverage gap.
- `2` — system error: database failure, workbench filesystem I/O failure.

---

## Domain: `test-spec`

Read-only inspectors for test-spec coverage. Where `spec ingest --strict` is the ingest-time gate that blocks an apply on a coverage gap, `test-spec status` is the human-facing view the planner runs during Phase 4 self-check.

### `planar test-spec status <plan>`

**Synopsis:**
```
planar test-spec status <plan> [--json]
```

**Description:** Print per-milestone test-spec coverage for an anchor plan. Each row reports the milestone's child plan, total tasks, slug-bearing tasks, covered tasks, and a four-bucket breakdown (happy / empty / error / edge) classified by scenario-title prefix. A summary line totals across milestones.

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
                            [--threshold <float>] [--roadmap <path>]
                            [--apply] [--refresh-status] [--scope <slug>]
                            [--no-status-inference] [--trust-status-inference]
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
| `--threshold <float>` | `0.7` | Confidence threshold for git-log correlation (0.0–1.0). Items whose best git-log match scores below this value are considered ambiguous. |
| `--roadmap <path>` | auto | Explicit path to the roadmap file. Overrides auto-discovery. Useful when the roadmap has a non-standard name or location. |
| `--apply` | off | Commit the import to the database. Without this flag, preview only. |
| `--refresh-status` | off | Re-run inference on already-imported tasks and update their status if the inferred status now differs (e.g. new commits match a previously-todo task). Status is the only field updated; title, body, and all other fields are left intact. No-op when status is already current. Has no effect without `--apply` unless combined with `--dry-run` to preview what would change. |
| `--scope <slug>` | cwd-derived | Scope for all created entities. Overrides cwd derivation for this invocation. |
| `--no-status-inference` | off | Default every imported task to `status=todo`, `signal=no-inference`, `confidence=0`. Skips layers 2-3 (branch + git-log correlation) of `adopter.Infer`; the checkbox layer still runs because checkbox state is operator-explicit. Use for greenfield, docs-only, or fresh-fork repos where status correlation is unreliable by construction. |
| `--trust-status-inference` | off | Explicit bypass for the >25% auto-done refusal (see below). Pass when you have a clean repo with a high genuinely-done count and have manually verified the inferred done marks. Mutually exclusive with `--no-status-inference`. |

**>25% auto-done refusal:** When `--threshold 0.0` disables the [confidence floor](#confidence-floor) and more than 25% of inferred tasks would land as `status=done` via git-log correlation, `import` refuses the import. The refusal text is:

```
warning: --threshold 0.0 would auto-mark <N>/<TOTAL> tasks (<P>%) as `status=done`
         based on git-log correlation. In docs-only or fresh repos this is
         almost always wrong. Refusing the import.

Options:
  --no-status-inference         skip inference entirely; default every task
                                to status=todo
  --trust-status-inference      explicit bypass; commit the done-marks (only
                                when you've verified them)
```

Operators who passed `--threshold 0.0` to bypass the confidence floor but ended up with the >25% refusal should reach for `--no-status-inference` first. Run `task reopen <id>` to recover any individually wrongly-marked tasks from an earlier import.

**Output (human, preview mode):**

```
<repo-root>/
  + plan       Phase 1 — Foundation         (4 tasks)
  +   task     Add migrations               [done — git-log match]
  +   task     Wire cobra commands          [todo]
  + plan       Phase 2 — Adapters           (2 tasks)
  +   task     Jira adapter                 [todo]
  +   task     GitHub adapter               [todo]
  + artifact   docs/tech-spec.md            [kind=tech_spec]
  + decision   Use pure-Go SQLite           [from: docs/adr/0005-go-as-runtime.md]
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
    {"title": "Use pure-Go SQLite", "source_path": "docs/adr/0005-go-as-runtime.md"}
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

The `tasks refreshed` line appears only when `--refresh-status` is set and at least one task status was updated.

**Output (`--apply --refresh-status`, when refreshes occur):**

```
applied: 1 plans, 0 tasks, 0 artifacts, 0 decisions, 5 skipped
anchor plan id: 87
tasks refreshed: 1
```

**Preview output with `--refresh-status` (before `--apply`):**

```
~   task     Implement login form    [todo → done, refreshed]
```

The `~` marker indicates a status-only update on an already-imported task.

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
- `planar ext propagate <plan>` — propagate the imported tree to Jira or GitHub Issues.
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
                                 [--no-status-inference] [--trust-status-inference]
                                 [--threshold <float>] [--code-layout <name>]
                                 [--treat-as-greenfield] [--treat-as-nongreenfield]
                                 [--accept-spec <slug>] [--no-forward-specs]
                                 [--literal] [--dry-run] [--json]
```

**Description:** Walk the repo at `<repo-root>`, run the deterministic floor (artifact discovery + `codeprobe.Probe` over the source tree), hand a fingerprinted `synthesis.Request` to the vendor skill via the on-disk cache, then on a cache-hit re-invocation validate and merge the LLM `synthesis.Result` against the deterministic baseline.

Default mode is **preview**: prints a tree-shaped diff and exits 0 without writing. `--apply` is required to commit additions and updates; `--apply --apply-removals` additionally soft-cancels removed entities.

The LLM never runs in Go. The Go side writes a `synthesis.Request` to `$PLANAR_HOME/cache/bootstrap-synthesis/<repo-slug>/_pending.json` and exits 0 with an "Awaiting LLM synthesis" notice. The vendor skill (`commands/claude/pl-synthesize.md`, `skills/codex/pl-synthesize.md`, `skills/copilot/pl-synthesize.md`) reads the Request, runs the LLM at temperature 0, and writes the Result to `<cache-dir>/<fingerprint>.json`. The operator re-runs `planar synthesize <repo-root>`; Go finds the cached Result, validates it, merges it with the deterministic baseline, and emits the preview.

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
| `--no-status-inference` | off | Default every task to `status=todo`. Skips status inference entirely. |
| `--trust-status-inference` | off | Bypass the >25% auto-done refusal. Mutually exclusive with `--no-status-inference`. |
| `--threshold <0..1>` | `0.7` | Confidence floor for ambiguous items. |
| `--code-layout <name>` | auto | Override layout detection. One of `swift`, `go`, `node`, `python`, `mixed`. |
| `--treat-as-greenfield` | off | Force greenfield mode even when code is detected. Forces every task to land `status=todo`. |
| `--treat-as-nongreenfield` | off | Bypass greenfield auto-detection. Use for non-conventional layouts where codeprobe under-detects. |
| `--accept-spec <slug>` | interactive | Non-interactive forward-spec selection; `all` accepts every proposed forward spec. |
| `--no-forward-specs` | off | Skip the forward-spec phase entirely. |
| `--literal` | off | Delegate to `import` (transcription). Useful when you started with `synthesize` but realize the repo is clean enough for transcription. |
| `--greenfield` | off | Deprecated alias for `--treat-as-greenfield`. |
| `--dry-run` | off | Emit the ImportPlan as JSON without writing, regardless of `--apply`. |
| `--json` | off | Machine-readable output. |

**Workflow:**

1. Operator runs `planar synthesize <repo-root>`.
2. Go side runs the deterministic floor (`adopter.Discover` + `adopter.ParseCorpus` + `codeprobe.Probe`).
3. Go side writes the `synthesis.Request` to `$PLANAR_HOME/cache/bootstrap-synthesis/<repo-slug>/_pending.json`.
4. Go side exits 0 with the "Awaiting LLM synthesis" message.
5. Vendor skill reads the Request, runs the LLM at temperature 0, and writes the Result to `<cache-dir>/<fingerprint>.json`.
6. Operator re-runs `planar synthesize <repo-root>`.
7. Go side reads the cached Result, runs `synthesis.Validate`, and merges it with the deterministic baseline.
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

**Apply layer.** The Apply path is **shared with `import`** (`src/internal/adopter/apply.go` + `diff.go`). Both verbs converge on the same downstream pipeline.

**Exit codes:**
- `0` — success (preview, dry-run, apply, or cache-miss "awaiting synthesis").
- `1` — user error: `<repo-root>` not found, conflicting flags, cached Result fails `synthesis.Validate`, confidence-floor refusal.
- `2` — system error: database failure, filesystem I/O failure, cache I/O failure.

**Related:**
- [`commands/claude/pl-synthesize.md`](../commands/claude/pl-synthesize.md) — Claude vendor skill body; load-bearing LLM contract.
- [`agents/synthesizer.md`](../agents/synthesizer.md) — vendor-neutral role spec.
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
4. Embedded defaults (shipped with the binary at `src/internal/config/defaults.toml`).

**Sensitive-data invariant:** keys whose names match `*_token`, `*_password`,
`*_secret`, `*_key`, or the bare names `token`, `password`, `secret` must not
carry literal values in the config file. Use the `*_env` convention to name the
environment variable instead.

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
planar config validate [<path>]
```

**Description:** Parse the config file (or `<path>` if supplied) and check:

- TOML syntax.
- Sensitive-data invariant: keys matching the secret-name denylist must not carry literal values.
- Cross-reference consistency (e.g. `auth = "token-env"` requires `token_env` to name an env var).
- Unknown keys produce a **warning**, not an error, preserving forward-compatibility.

Each issue is printed with its line number and key path.

**Arguments:**

| Argument | Description |
|----------|-------------|
| `<path>` | Optional path to validate instead of the resolved config path. |

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
- `1` — one or more errors found.
- `2` — file not found or unreadable.

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
(Jira issues, GitHub Issues, GitHub Projects). Templates are files only — no
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
planar templates show <set> <system> <kind>
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
planar templates render <set> <system> <kind> --entity <kind>:<id>
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
planar templates validate [<path>]
```

**Description:** Validate a single template file (if `<path>` is given) or all
templates under the resolved templates root (including embedded defaults). Checks
`text/template` compilability of every string field. Reports each issue with
the JSON path of the offending field.

**Arguments:**

| Argument | Description |
|----------|-------------|
| `<path>` | Optional. Filesystem path to a single template file. |

**Output:** One line per issue in the form `ISSUE: <path> [<json-path>]: <error>`.
Prints `templates validate: ok` on success.

**Schema effects:** None.

**Capture:** None.

**Exit codes:**
- `0` — no issues found.
- `1` — one or more validation issues found.
- `2` — cannot read templates root or individual file.

---

### `planar templates init`

**Synopsis:**
```
planar templates init
```

**Description:** Idempotently extract the embedded baseline templates to
`<templates-root>/default/<system>/<kind>.json`. Existing files are never
overwritten. Reports the list of files written. Also called automatically by
`planar init` (after `config init`, before `migrate apply`).

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
planar templates path [<set> <system> <kind>]
```

**Description:** With no arguments, print the resolved templates root directory.
With three arguments, print the resolved path of the specific template
(indicating which level of the fallback chain would serve it).

**Arguments:**

| Argument | Description |
|----------|-------------|
| `<set>` | Template set name (optional; requires all three or none). |
| `<system>` | System name. |
| `<kind>` | Template kind. |

**Output:**
```
/Users/alice/.planar/templates
```

**Schema effects:** None.

**Capture:** None.

**Exit codes:**
- `0` — success.
- `1` — template not found (three-argument form).
- `2` — cannot resolve templates root.

---

## Domain: `local`

User-local sandbox for personal skills and agents. Skills live as dir-shape sources at `~/.planar/local/skills/<name>/SKILL.md`; agents are flat at `~/.planar/local/agents/<name>.md`. Both are installed (symlink with copy fallback) into each vendor's install directory. See `docs/concepts.md § Local sandbox` for the design — including why Codex / Copilot install via directory symlinks rather than file symlinks.

### `planar local link [<name>]`

Walk `~/.planar/local/{skills,agents}/`, parse each source file's YAML frontmatter, and install per-vendor symlinks pointing at the source.

| Flag | Description |
|---|---|
| `--dry-run` | Preview the planned installs without touching the filesystem. |
| `--vendor <v>` | Restrict to one or more vendors (`claude`, `codex`, `copilot`). Repeatable; can be combined with the per-file `vendors:` frontmatter list (intersection). |

**Behavior:**
- Without `<name>`, links every source file under both `skills/` and `agents/`.
- With `<name>`, links only the matching source (errors if not found).
- Idempotent: a re-link of an unchanged source produces `unchanged` actions.
- Atomic: writes via temp-path + rename so the install file is never observed missing.
- Copy fallback fires automatically when `os.Symlink` errors (e.g. Windows without developer mode); a warning calls out that future edits require re-linking.

### `planar local migrate [--dry-run]`

Convert legacy flat sandbox skills (`~/.planar/local/skills/<name>.md`) into the dir-shape layout (`<name>/SKILL.md`) required by the dir-symlink installs.

| Flag | Description |
|---|---|
| `--dry-run` | Preview without renaming on disk. |

**Behavior:**
- Idempotent. Skills already in dir-shape are skipped silently.
- A collision (target `<name>/` exists but is not a matching skill dir) is skipped with a reason; nothing is overwritten.
- Agents (`~/.planar/local/agents/<name>.md`) are intentionally left alone — they stay flat.

### `planar local import <path>`

Import operator-authored skill or agent files from an external location into the sandbox and link them into every vendor surface. The operator points at a single flat `.md` file, a single dir-shape skill source (`foo/` containing `SKILL.md`), or a directory containing a mix of both. Each valid input is materialized in the sandbox in the correct shape — skills as `~/.planar/local/skills/<name>/SKILL.md` (auxiliary files inside dir-shape inputs travel along); agents as `~/.planar/local/agents/<name>.md`. Linking then runs via the same path as `planar local link`.

| Flag | Description |
|---|---|
| `--kind <skill\|agent>` | Target kind. Default `skill`. The frontmatter `kind:` field (if present) must agree with this flag. |
| `--force` | Overwrite an existing sandbox file of the same name. Without this, name collisions are skipped — the operator's prior work is never silently lost. |
| `--dry-run` | Preview the planned imports and links without writing. |
| `--no-link` | Import only; skip the link step. Useful when the operator wants to inspect the sandbox copy before linking. |

**Behavior:**
- A single flat `.md` file source imports that file (wrapped into `<name>/SKILL.md` for skills). A single dir-shape source (a directory whose top-level contains `SKILL.md`) imports that whole tree. A directory source containing a mix of `*.md` files and `<name>/SKILL.md` subdirs imports each top-level entry — other subdirectories and non-`.md` files are ignored.
- Each file's frontmatter is validated by the same parser as `planar local link` (YAML frontmatter required; `vendors:` if present must be a subset of `{claude, codex, copilot}`; `kind:` if present must match `--kind`).
- After a successful import, the link layer is invoked automatically (suppress with `--no-link`).
- `.link-manifest.json` files in the source directory are explicitly ignored so pointing the importer at the sandbox itself does not corrupt it.

### `planar local unlink <name> [--purge]`

Remove every per-vendor install recorded for `<name>` in the per-kind `.link-manifest.json`. With `--purge`, also delete the source file from `~/.planar/local/`.

### `planar local list [--vendor <v>] [--json]`

List every recorded install across both kinds and all vendors. Status column:

- `live` — install file exists and symlink (if any) resolves to the source.
- `broken` — install is a symlink but the source is gone, or the symlink points elsewhere.
- `missing` — install file is gone entirely.

`live`/`broken`/`missing` colorize via the standard palette (green/red/gray) when colors are enabled. The status text comes from the recorded manifest plus live filesystem checks; the manifest itself is not authoritative.

### `PLANAR_LOCAL_HOME` env var

Test hook: when set, `planar local` uses this directory as the operator's `$HOME` for resolving `~/.planar/local/` and the per-vendor install targets. Production use never sets this.

## Domain: `skills`

The `skills` domain renders generated vendor skill surfaces from the unified source tree (`skills/src/*.md`) and verifies that generated outputs are in sync with source. The vendor profile/model table source of truth is `src/configs/vendors.yaml` (embedded into the binary).

### `planar skills render [slug...]`

**Synopsis:**
```
planar skills render [slug...]
  [--src <dir>]
  [--out <dir>]
  [--check]
  [--diff]
```

**Description:** Render every unified skill source under `--src` (default: `skills/src`) into the per-vendor output trees under `--out` (default: `.`):

- `commands/claude/<slug>.md`
- `skills/codex/<slug>.md`
- `skills/copilot/<slug>.md`

With positional `slug` arguments, only those sources are written (the full source tree is still parsed first, so malformed sources fail-fast before any write).

When `agents/models.md` exists under `--out`, `render` also rewrites only its `## Tier Table` section from `src/configs/vendors.yaml`; surrounding prose is preserved.

**Options:**

| Flag | Default | Description |
|------|---------|-------------|
| `--src <dir>` | `skills/src` | Source directory containing unified skill markdown files. |
| `--out <dir>` | `.` | Output root for generated vendor trees. |
| `--check` | off | Read-only mode: render in-memory and compare with on-disk generated files; exits non-zero on drift. |
| `--diff` | off | Valid only with `--check`; emit unified diff text for content-drifted files. |

**`--check` drift categories:**

- `[content]` — file exists but bytes differ from the in-memory render
- `[missing]` — expected generated file is missing
- `[orphan]` — generated file exists with no matching source

**Examples:**
```
planar skills render
planar skills render pl-plan pl-task
planar skills render --check
planar skills render --check --diff
```

**Schema effects:** None (filesystem-only).

**Capture:** None.

**Exit codes:**
- `0` — success (`--check`: in sync).
- `1` — drift detected under `--check`.
- `2` — filesystem or parse/render failure.
- `64` — usage error (for example `--diff` without `--check`).

## Domain: `tree`

The `tree` domain provides a hierarchical view of Planar entities — plans, tasks, artifacts, decisions, scenarios, and questions — for one or all scopes. Read-only; no schema effects.

The walk follows `plans.parent_plan_id` for plan→plan, `tasks.plan_id` and `tasks.parent_task_id` for plan→task and task→subtask, and `entity_links(relationship='derives-from')` for artifacts / decisions / scenarios / questions attached to plans. Filters apply during the walk so excluded subtrees never enter the output.

The flag surface deliberately mirrors `tree(1)` wherever the semantic translates. Filesystem-specific flags from `tree(1)` are explicitly rejected at parse time rather than silently ignored — see [Deliberately omitted flags](#deliberately-omitted-flags).

### `planar tree`

**Synopsis:**
```
planar tree [--scope <scope> | --all-scopes]
            [-L <N> | --depth <N>]
            [-I <pattern>]... [-P <pattern>]... [--ignore-case]
            [--kind <list>]... [--status <list>]...
            [-r] [-t | -c | -U | --sort <id|updated|created|unsorted>]
            [--dirsfirst | --no-dirsfirst]
            [--noreport] [--prune]
            [-i | --no-indent] [--ascii] [--no-truncate]
            [-J | --json]
```

**Description:** Render a hierarchical view of the cwd-derived scope (default) or another scope, walking plans → tasks → derived artifacts/decisions/scenarios/questions.

**Options:**

| Flag | tree(1) equiv. | Default | Description |
|------|----------------|---------|-------------|
| `--scope <X>` | (Planar) | cwd-derived | Render this scope only. Accepts `global`, `repo`, `repo:<slug>`, `assoc:<slug>`. |
| `--all-scopes` | (Planar) | off | Render every scope as a separate section. Always renders the `global` section even when empty. Mutually exclusive with `--scope`. |
| `-L`, `--depth <N>` | `-L` | unbounded | Maximum recursion depth. Top-level plan is depth 0; child plan is depth 1; task under a top-level plan is depth 1; subtask is depth 2. |
| `-I`, `--ignore <pattern>` | `-I` | none | Glob pattern excluding entities whose title or slug matches. Repeatable. |
| `-P`, `--match <pattern>` | `-P` | none | Glob pattern restricting to entities whose title or slug matches (or whose descendants match). Repeatable. |
| `--ignore-case` | `--ignore-case` | off | Case-insensitive matching for `-I` / `-P`. |
| `--kind <list>` | (Planar) | all | Restrict entity kinds. Repeatable. Valid: `plan`, `task`, `artifact`, `decision`, `scenario`, `question`. |
| `--status <list>` | (Planar) | all | Restrict by status. Repeatable. |
| `-r`, `--reverse` | `-r` | off | Reverse sort order within each level. |
| `-t`, `--sort-updated` | `-t` | off | Sort by `updated_at`. |
| `-c`, `--sort-created` | `-c` | off | Sort by `created_at`. |
| `-U`, `--unsorted` | `-U` | off | Preserve DB insertion order. |
| `--sort <id\|updated\|created\|unsorted>` | (Planar) | `id` | Long-form sort selector. Mutually exclusive with `-t`/`-c`/`-U`. |
| `--dirsfirst` | `--dirsfirst` | **on** | Group plans first, then tasks, then everything else by id. |
| `--no-dirsfirst` | (Planar) | off | Interleave entity kinds by id (no grouping). |
| `--noreport` | `--noreport` | off | Suppress the summary footer (`N plans, M tasks, …`). |
| `--prune` | `--prune` | off | Hide empty branches (plans with zero descendants under the active filter). |
| `-i`, `--no-indent` | `-i` | off | Disable indentation; print flat tree (one entity per line, no branch glyphs). |
| `--ascii` | `--charset ASCII` | off | Use ASCII box-drawing (`+--`, `|`, `\--`). Default is Unicode (`├──`, `│`, `└──`). |
| `--no-truncate` | (Planar) | off | Do not truncate long entity titles. Default truncates at 80 chars with `…`. |
| `-J`, `--json` | `-J` | off | Emit nested JSON instead of indented text. |

#### Deliberately omitted flags

The following `tree(1)` flags are intentionally not supported. They are rejected at parse time with a clean error pointing at the Planar alternative (or noting no analog exists) rather than silently ignored:

| Flag | Reason |
|------|--------|
| `-a` | no concept of hidden entities |
| `-d` | use `--kind plan` |
| `-f` | entity references are `kind:id`; full path is redundant |
| `-s`, `-h`, `-p`, `-u`, `-g`, `-D` | no filesystem analog (use `-t` / `-c` for entity timestamps) |
| `--inodes`, `--device` | filesystem-specific |
| `-Q` | Planar titles are sanitized at insert; no quoting needed |
| `-X`, `-H` | use `--json` and pipe through a transformer |
| `-v` | no semver in entity titles |
| `--filelimit`, `--matchdirs` | out of scope; revisit on user signal |
| `-C` | color support deferred |
| `-o` | use shell redirect (`> file`) to write to a file |

**Output (human):**
```
assoc:project:planar
├── plan:1 [active]  Backlog
│   ├── plan:2 [active]  ext propagate --github-strategy override
│   │   ├── task:1  Add --github-strategy flag                       [todo, pri:10]
│   │   └── task:2  Plumb override                                   [todo, pri:20]
│   ├── artifact:1  Architecture overview                            [tech_spec, active]
│   └── question:3  cmd_scope.go prints errors twice                 [open]
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

`planar-agent` is the agent-callable coordination binary. Owns every write to `agent_work_claims` and `agent_actions`; operator-recovery verbs (`reconcile`, `abort`) live here too because both are `agent_*` table writers (the capability boundary tracks tables, not audience). See `docs/architecture.md` § "Three-binary architecture" for the binary split.

Schema-version handshake: `planar-agent` is a **consumer** of the schema, not its owner. Startup queries `schema_migrations.max(version)` and refuses with exit **7** when the live DB is older than the binary's embedded minimum. The remediation pointer ("run `planar init`") is printed to stderr.

### Verb surface (13 verbs)

```text
# Atomic operations — each wraps (claim lifecycle + action lifecycle +
# task status transition) in a single BEGIN IMMEDIATE transaction.
planar-agent pull       <plan-id> [--vendor-session <vendor:id>] [--role coder] [--ttl <duration>] [--purpose <text>] [--base-ref <git-ref>] [--worktree <id-or-path>] [--repo-root <path>] [--no-locality-probe] [--json]
planar-agent peek       <plan-id> [--json]
planar-agent complete   --claim <token> [--summary <text>] [--json]
planar-agent fail       --claim <token> --reason <text> [--json]
planar-agent release    --claim <token> [--reason <text>] [--json]
planar-agent block      --claim <token> --blocker <task-id> [--reason <text>] [--json]

# Claim primitives — for orchestrator-dispatch (caller already knows the
# target entity by id). claim does NOT auto-transition task status.
planar-agent claim      --entity task:<id>|plan:<id>|plan_step:<id> [--vendor-session <vendor:id>] [--role <r>] [--ttl <duration>] [--purpose <text>] [--worktree <id-or-path>] [--repo-root <path>] [--no-locality-probe] [--force] [--json]
planar-agent heartbeat  --claim <token> [--ttl <duration>] [--json]

# Nested action lifecycle — for sub-tool-calls or sub-phases inside a
# claim. Optional; lightweight claims skip these.
planar-agent action start  --claim <token> --kind <kind> [--entity <kind>:<id>] [--vendor-role <s>] [--repo-root <path>] [--no-locality-probe] [--json]
planar-agent action end    --action <id> [--outcome ok|error|aborted|timeout] [--summary <s>] [--json]

# Vendor hook ingestion — translates hook events into the primitives.
# M2 ships a skeleton handler; full adapter routing lands in M4.
planar-agent ingest     --vendor claude --event @<file|-> [--json]

# Operator recovery — agent_* table writers, which is why they live on
# planar-agent (not planar). The operator invokes them directly; vendor
# hooks never do.
planar-agent reconcile  [--dry-run] [--stale-after <duration>] [--json]
planar-agent abort      --claim <token> [--reason <text>] [--vendor <s>] [--vendor-session <vendor:id>] [--json]
```

**Duration grammar:** `--ttl`, `--stale-after`, and `--interval` accept either a bare integer (interpreted as seconds for the `--ttl` / `--stale-after` surface; `--interval` follows the same default for back-compat with the legacy parser) or a number with an ISO-style suffix: `ns`, `us`, `ms`, `s`, `m`, `h`. Examples: `--ttl 600` (10 minutes), `--ttl 10m` (same), `--ttl 1h`, `--interval 500ms`. The implementation is the shared `cli.duration` helper.

### Atomic operation transaction shapes

| Verb       | Transaction body |
|------------|------------------|
| `pull`     | SELECT next eligible task → INSERT `agent_work_claims (status=active)` → UPDATE `tasks.status='doing'` → INSERT `agent_actions`. Returns `{ok, no_work, claim_token, claim, task, action_id}`. No writes on no-eligible-task path. |
| `complete` | Verify claim active → UPDATE action ended_at + outcome='ok' → UPDATE task status='done' → UPDATE claim status='completed'. |
| `fail`     | Same as complete with outcome='error', task status='todo', claim status='aborted'. |
| `release`  | Same as fail with outcome='aborted', claim status='released'. (Distinct semantically from fail — "graceful give-up" vs "I tried and failed".) |
| `block`    | INSERT `entity_links(from=task, to=blocker, relationship='blocks')` → UPDATE task status='blocked' → UPDATE action ended_at + outcome='aborted' → UPDATE claim status='released'. |
| `peek`     | Read-only: same SELECT as step 1 of pull; no writes. |
| `reconcile`| SELECT expired active claims → UPDATE status='stale' → UPDATE orphaned actions ended_at + outcome='aborted'. Does NOT touch tasks.status. `--dry-run` returns candidates without writing. |
| `abort`    | UPDATE claim status='aborted' + released_at + release_reason → INSERT audit `agent_actions` row naming the aborting session. Does NOT touch tasks.status. |

All write verbs open `BEGIN IMMEDIATE` so the writer lock blocks any concurrent claim attempt on the same row. The status-transition guard (`policy.status.check`) is consulted before each `UPDATE tasks SET status` — refusal rolls the transaction back and the claim keeps its previous state.

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
| `reconcile`   | `{ok, claims_marked_stale, actions_closed, candidates?}` (`candidates` present only with `--dry-run`) |
| `abort`       | `{ok, claim_token, claim, aborting_session}` |

`ClaimRow` matches the `agent_work_claims` row shape with snake_case keys (including locality columns `repo_root`, `branch`, `head_sha_at_claim`, `dirty_at_claim` and worktree columns `worktree_id`, `worktree_path`). `Task` matches `planar task show --json`. `ActionRow` matches `agent_actions` (including locality columns `head_sha`, `dirty`).

### Locality flags

`pull`, `claim`, and `action start` accept:

- `--repo-root <path>` — absolute path of the checkout to probe locality against. Falls back to the process cwd when omitted.
- `--no-locality-probe` — short-circuit; records locality columns as NULL.

Per-action-kind defaults: planner / coder / reviewer / test_coder probe; heartbeat / tool_call skip. The probe runs `git symbolic-ref`, `git rev-parse HEAD`, and `git status --porcelain` against the resolved root; any subprocess failure (non-git directory, missing `git`, error exit) records `unknown` rather than refusing the claim.

### Entity-ref parser (`claim --entity`)

`--entity <ref>` accepts `task:<id>` / `plan:<id>` / `plan_step:<id>`. Other prefixes are rejected with a non-zero exit. Missing colon or non-integer id is rejected the same way. The strict parser matches the tech-spec § "claim --entity <ref> parser" contract.

### Exit codes

| Code | Meaning |
|------|---------|
| `0`  | Success. |
| `1`  | Operational failure (atomic op rolled back, invalid state, missing claim token). |
| `2`  | User-input failure (unknown flag, missing required, invalid value). |
| `7`  | Schema version mismatch — DB older than this binary's embedded minimum, or newer than its embedded max. Remediation: run `planar init`. |
| `64` | Not implemented yet (reserved for future verbs). |

### Capability boundary

A process invoked as `planar-agent` writes ONLY to `agent_work_claims`, `agent_actions`, and `tasks.status` (the last only as part of atomic coordinated operations with status guards). It NEVER writes to plan / decision / question / scenario / artifact / annotation. A vendor hook configured with only `planar-agent` on its PATH has bounded blast radius — it cannot touch planning state.

---

## Binary: `planar-watch`

`planar-watch` is the human-facing **read-only viewer** for live agent activity. Third binary in the three-binary architecture (plan 85 M8). See `docs/architecture.md` § "Three-binary architecture" for the binary split and `docs/workflows.md` § Recipe 19 for the end-to-end cockpit workflow.

Schema-version handshake: `planar-watch` is a **consumer** of the schema, not its owner. Startup queries `schema_migrations.max(version)` and refuses with exit **7** when the live DB is older than the binary's embedded minimum (same code `planar-agent` uses; remediation message "run `planar init`").

### Capability invariant

A process invoked as `planar-watch` performs **no writes**. Two defenses:

1. The command tree (`src/cmd/planar-watch/handlers/cmd.zig`) registers exactly six read verbs plus the conventional `version` / `completion` helpers. There is no write verb anywhere in the tree.
2. The bootstrap calls `runtime.ensureDbStrictReadOnly` which opens the DB via `sqlite3_open_v2(..., SQLITE_OPEN_READONLY, ...)`. The SQLite driver itself returns `SQLITE_READONLY` on any attempted `INSERT` / `UPDATE` / `DELETE` / DDL — verified by the `openReadOnly: write SQL is rejected at the driver layer` unit test in `src/db/sqlite.zig`.

A vendor hook or operator script configured with only `planar-watch` on its PATH cannot modify the database under any circumstances.

### Verbs

```
# Cross-cutting activity feed (default invocation; `planar-watch` with no
# args routes here).
planar-watch              # alias for `planar-watch feed`
planar-watch feed     [--follow]  [--vendor <v>] [--plan <id>] [--task <id>] [--since <ISO>] [--limit N] [--json] [--interval <D>]

# Snapshot: active (and stale) claims.
planar-watch ps       [--follow]  [--vendor <v>] [--plan <id>] [--stale]  [--json] [--interval <D>]

# Claim ledger (active | stale | all buckets).
planar-watch claims   [--follow]  [--vendor <v>] [--plan <id>] [--status active|stale|all] [--json] [--interval <D>]

# Action ledger (filterable by kind / entity / plan / task).
planar-watch actions  [--follow]  [--vendor <v>] [--kind <k>] [--entity <kind:id>] [--plan <id>] [--task <id>] [--limit N] [--json] [--interval <D>]

# Plans with in-flight work.
planar-watch plans    [--follow]  [--in-flight-only] [--json] [--interval <D>]

# Per-entity / per-claim history (union of actions + claim transitions).
planar-watch log      (--task <id> | --plan <id> | --entity <kind:id> | --session <id> | --claim <token>) [--limit N] [--json]

# Conventional helpers.
planar-watch version
planar-watch completion <bash|zsh|fish>
```

`--follow` (default off) turns each subcommand into a streaming view: the initial snapshot prints, then new events append as the underlying tables change. M8 ships **Tier 1** of the wake-tier ladder (poll every `--interval`, default `1s`; sub-second intervals available for tests). The watermark column set and the JSON event shape are part of the public contract — Tier 2 (kqueue / inotify on the SQLite `-wal` file) lands in a follow-up without changing either.

`--plan <id>` widens past the literal `entity_kind='plan'` match: feed / ps / claims / actions all return events whose entity is the plan itself, OR a task on the plan, OR a plan_step on the plan. This is what the operator means by "show me plan N" — task-on-plan events are usually the only ones a session actually generates.

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

planar-watch ps --json:
  { generated_at: ISO8601, active: [ClaimRow], stale: [ClaimRow] }

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
```

`ClaimRow` matches the canonical shape from `engine.runtime.agentactivity.json.writeClaim` (snake_case keys mirroring the `agent_work_claims` columns, including the locality columns `repo_root`, `branch`, `head_sha_at_claim`, `dirty_at_claim` and the worktree columns). `ActionRow` mirrors `agent_actions`. `Plan` matches `planar plan show --json`.

### Exit codes

| Code | Meaning |
|------|---------|
| 0 | Success (and the `--follow` graceful-SIGINT exit). |
| 1 | Generic failure (DB I/O error, malformed argument). |
| 2 | User-input failure (unknown flag, missing required filter on `log`). |
| 7 | Schema version mismatch (DB older than binary's embedded minimum, OR newer than its embedded max). Same code `planar` and `planar-agent` use. |

### `--follow` and SIGINT

Each `--follow` verb installs a SIGINT handler that flips an atomic flag. The poll loop checks the flag between iterations and exits cleanly with code **0** on the next tick. Pressing Ctrl-C is "stop watching" — a success outcome, not an error.

---

## Command Index

For quick reference, all documented commands grouped by domain:

| Domain | Commands |
|--------|----------|
| `init` | `init` (calls `config init` and `templates init` before applying migrations) |
| `config` | `config show`, `config show --effective`, `config show --raw`, `config show --defaults`, `config edit`, `config validate`, `config init`, `config path` |
| `templates` | `templates list`, `templates show`, `templates render`, `templates validate`, `templates init`, `templates path` |
| `tree` | `tree` |
| `skills` | `skills render [slug...]`, `skills render --check`, `skills render --check --diff` |
| `scope` | `scope show`, `scope suggest` (`scope use`/`pop`/`clear` removed in plan 153 M5) |
| `assoc` | `assoc list`, `assoc create`, `assoc add`, `assoc remove`, `assoc members`, `assoc detect` |
| `plan` | `plan create`, `plan show`, `plan list`, `plan update`, `plan step add`, `plan step done`, `plan step skip`, `plan step link`, `plan link` |
| `task` | `task add`, `task show`, `task list`, `task update`, `task edit`, `task view`, `task diff`, `task review`, `task done`, `task reopen`, `task block`, `task link`, `task touches add`, `task touches remove` |
| `question` | `question add`, `question answer`, `question wontfix`, `question list`, `question show`, `question edit`, `question view`, `question diff`, `question review`, `question link` |
| `scenario` | `scenario add`, `scenario verify`, `scenario list`, `scenario show`, `scenario edit`, `scenario view`, `scenario diff`, `scenario retire` |
| `decision` | `decision add`, `decision accept`, `decision supersede`, `decision withdraw`, `decision list`, `decision show`, `decision edit`, `decision view`, `decision diff` |
| `artifact` | `artifact add`, `artifact show`, `artifact list`, `artifact update`, `artifact edit`, `artifact view`, `artifact diff`, `artifact link` |
| `promote` | `promote`, `demote` |
| `workbench` | `workbench push`, `workbench pull`, `workbench status`, `workbench resolve`, `workbench sync`, `workbench archive`, `workbench restore`, `workbench list`, `workbench publish`, `workbench edit` |
| `workspace` | `workspace init`, `workspace doctor`, `workspace routing build`, `workspace routing show`, `workspace regenerate` |
| `ext` | `ext register jira`, `ext register github`, `ext list`, `ext test`, `ext create`, `ext propagate` |
| `link` | `link`, `unlink` |
| `sync` | `sync pull`, `sync push`, `sync status`, `sync resolve` |
| `resume` | `resume`, `resume validate` |
| `handoff` | `handoff`, `handoff validate`, `handoff list`, `handoff consume` |
| `capture` | `capture session`, `capture end`, `capture note`, `capture command`, `capture file`, `capture snapshot` |
| `audit` | `audit trail`, `audit session` |
| `health` | `health` |
| `links` | `links list`, `links remove` |
| `spec` | `spec ingest` |
| `test-spec` | `test-spec status` |
| `import` | `import <repo-root>` |
| `synthesize` | `synthesize <repo-root>` |
| `local` | `local list`, `local link`, `local unlink`, `local import`, `local migrate` |
| `help` | `help` |
