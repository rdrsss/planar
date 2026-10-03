# Planar Concepts

This document explains the core concepts in Planar. Read it after `planar init` and before doing substantive work — the vocabulary here maps directly to CLI commands and database tables. The domain concepts come first; the executables and supporting components follow in the second half of the page.

---

## Three threads

Three threads run through everything below:

- **Three orthogonal axes.** *Storage scope* (one SQLite DB per user; the workbench is a bidirectionally synced drafting filesystem at `$PLANAR_WORKBENCH_ROOT`). *Entity scope* (every entity carries `(scope_kind, scope_id)` ∈ `{repo, association, global}`). *Active scope* (derived from the current working directory, or passed per verb with `--scope`; there is no scope stack). See [Scope](#scope).
- **Three operational context planes.** *Local* (the SQLite store; working memory). *Workbench* (bidirectionally synced Markdown filesystem under `~/.planar/workbench/`; the drafting surface for active features; see [Workbench](#workbench)). *Operational* (Jira / GitHub Issues; the org's system of record; see [External Link](#external-link)). Each plane has its own audience and its own source-of-truth rules.
- **From-zero handoff.** A new agent process — different vendor, no prior session memory — must be able to resume an in-flight task with one command. The combined `handoff <task-id>` ritual creates and validates the resume packet atomically. `resume validate` is the CI gate. See [Session](#session).

---

## Context plane

The context plane is the durable working-memory layer that lets one workflow stage pass structured information to the next. It is distinct from the session timeline (`session_entries`) by deliberate design (decision 445): `session_entries` is a narrative record of what happened; `context_records` is working memory — typed, lifecycle-managed rows that the next stage reads and acts on. The two tables have different consumers, different lifecycles, and overloading the timeline with working-memory noise would force every downstream reader to filter it out forever.

### Tables

**`workflow_runs`** is the identity and audit record for one external workflow harness invocation. A row is opened by `planar-agent run start` before the Lua `run()` function is entered, and closed by `planar-agent run end` after it returns. The harness itself holds no DB handle (decision 444) — it shells those verbs exactly as it shells the coordination verbs (`pull`, `complete`, etc.). The row carries `plan_id`, `workflow_name`, a unique `run_identifier` (`run-<pid>-<nanos>`), `pid`, `repo_root`, and a `status` in `running | completed | failed | interrupted | abandoned`. Since migration 00039 `pid` is nullable: a pid-less run instead carries an `expires_at` lease deadline, extended by `planar-agent run heartbeat`. `abandoned` is written only by `planar-agent reconcile`, which pid-probes stalled pid-bound rows whose process is no longer alive and abandons a pid-less row once its lease has lapsed. Dry-run (`--dry-run`) creates no run row.

**`context_records`** is run-scoped working memory. Every record is keyed `(run_id, stage, session_id, claim_id)` and carries a `kind` (`finding`, `risk`, `artifact`, `followup`, `summary`, `capsule`) plus a free-text `body`. The `status` column (`active | consumed | superseded`) is the lifecycle signal. A nullable `compiled_from` column on `capsule` records stores the integer ids of the raw records the capsule distilled — full provenance without deletion.

### Accumulate → read → compose loop

**Accumulate.** A worker writes records via `planar-agent context add --claim <token> --kind <kind> --body <text>`. The claim token is the only envelope the worker needs to thread (decision 447): `planar-agent` stamps `run_id`, `stage`, `session_id`, and `task_id` server-side from the claim row. The claim row gains nullable `run_id` and `stage` columns (migration 00023), populated at `pull`/`claim` time when the orchestrator passes `--run <id> --stage <name>` (decision 450). Interactive claims leave these null; the context verb is a no-op for claims without a run row.

**Read.** A workflow script reads accumulated records from a prior stage via `ctx.context([stage])`. With no argument it returns all records for the current run; with a stage name it returns only that stage's records. The Lua return value is a 1-based sequence of tables, each carrying `id`, `run_id`, `stage`, `kind`, `body`, `status`, `compiled_from` (nil when absent), and `created_at`. Under the hood the external harness shells `planar-agent context list --run <run_db_id> [--stage <s>] --json` and maps the parsed JSON into the Lua table — no DB handle is opened.

**Compose.** `ctx.brief({...})` assembles a methodology-compliant coder brief and automatically injects the current run's context records into the "Prior-stage context" section. Any `capsule`-kind record is promoted to a compiled-capsule sub-section; remaining records render as a `kind: body` bullet list. The caller supplies the `problem_statement`, `claim_token`, `gates`, and optional `spec_citations`/`locked_decisions`; the context injection is automatic when `active_run` is non-null.

### Lifecycle: active → consumed | superseded, never deletion

Raw records start life as `active`. Stage close marks them `consumed` (records incorporated into the capsule) or `superseded` (records overridden by a later record in the same stage) and writes one compiled `capsule` record whose `compiled_from` column points back to the raw record ids (decision 446). Raw records are retained permanently — the audit trail is preserved for `pl-introspect` and journal-based resume. The three-value status is the machine-readable lifecycle signal; `compiled_from` is the provenance trace.

### Observability

`planar-watch run list [--plan <id>] [--status <s>]` lists runs. `planar-watch run show <id> [--json]` returns the full run row plus all `context_records`, grouped and ordered by stage then `created_at`. The JSON shape is `{run: RunRow, context_records: [...]}`.

**SQLite tables:** `workflow_runs` (migration 00022; lease columns in 00039), `context_records` (migration 00022), `agent_work_claims.run_id/stage` (migration 00023). **Primary verbs:** `planar-agent run start/end/heartbeat`, `planar-agent context add/capsule/list/resolve`, `ctx.context([stage])`, `ctx.brief({...})` (host functions on the external workflow harness), `planar-watch run list/show`. **Decisions:** 444 (run row owned by `planar-agent`; harness is DB-handle-free), 445 (separate table — timeline vs working memory), 446 (lifecycle not deletion; capsule provenance), 447 (claim is the correlation key), 450 (claims carry run/stage).

---

## Scope

Scope determines which entities are included in queries by default and where new entities are created when the target is unambiguous from the current working directory.

There are three scope kinds:

| Kind | Meaning |
|------|---------|
| `repo:<slug>` | Scoped to a registered project row in `projects`. Use this for work that belongs to one repository. |
| `assoc:<slug>` | Scoped to a named association — typically a `kind=org` workspace, a `client`, an `ad-hoc` grouping, or a `personal` bucket. |
| `global` | No project or association filter — personal, cross-cutting entities. |

The `repo:` prefix is required for project rows. A bare `--scope <slug>` is
parsed as an association slug for compatibility with existing workspace flows.

Scope is a pure function of `(--scope flag, cwd, db schema)`. There is no ambient stack and no per-process session state to forget about: every invocation resolves from the same two inputs.

### Cwd-derivation: the primary signal

Both the read and write resolvers begin by walking from the current working directory up the filesystem. `derive_from_cwd` (`src/engine/identity/scope.cppm`) collects every registered scope whose root path is a prefix of cwd. Two kinds of root path are matched:

- A `projects.root_path`. The cwd is inside a registered repo; the resulting candidate can be the concrete `repo:<slug>` scope, and any association memberships for that project can also contribute association candidates.
- An `associations.config_json.root_path` for `kind in ('org', 'client', 'personal', 'ad-hoc')`. The cwd is at (or inside) a workspace root registered via `planar workspace init` or the equivalent assoc creation flow.

Each match becomes a candidate carrying the association id, kind, and the root path that fired. When two project roots both match cwd, the longer `projects.root_path` wins: a cwd under `~/work/root/modules/nested/` resolves to the nested project instead of the containing root project, while a cwd under `~/work/root/src/` resolves to the root project.

### Specificity ranking

```
narrowest first:
  1. project association
  2. ad-hoc, personal
  3. client
  4. repo
  5. org
  6. host, path, lang  (auto-detected technical / fallback association kinds)
```

A repo scope outranks an `org` whose membership contains that project, so a cwd inside `~/work/repo-a/` resolves to `repo:repo-a` even when `org:work` also matches. Association candidates of kind `project` rank above raw repo candidates for compatibility with older project-association flows. At the workspace root (`~/work/`, no project root_path contains it) only `org:work` matches, so the org wins, and a write there lands at `assoc:work` — it does **not** refuse.

**This ranking applies to reads and writes alike as of task 6746.** It did not before: the write path mapped the cwd's project to its single association and so could never yield a `repo:` scope at all, while `scope show` — which renders the read resolution — reported one. The two disagreed from the same working directory, which meant the verb operators use to ask "where will this write land?" answered wrong. `resolve_for_write` and `resolve_read_scope_set` now share the ranking, and their agreement is pinned by a test.

### Write resolution

Writes resolve through `resolve_for_write`. The first step that yields a
single scope wins:

1. **Explicit flag.** If `--scope` is passed, it is threaded through
   **verbatim** — not validated against the database. The flag is the
   operator's stated intent. An unresolvable slug surfaces later, from
   whichever verb tries to use it, as `SlugNotFound` (exit 1).
2. **Meta-workspace arm.** Standing exactly on a registered meta-workspace
   root, where the cwd names the org and the root repo equally well, the write
   refuses rather than pick (exit 5). Inside a member repo of a meta
   workspace, the write lands on that concrete repo.
3. **Cwd derivation, most-specific-wins.** The same candidate set, specificity
   ranking and longest-root tie-breaker the read path uses (task 6746). A
   member repo therefore outranks an org that contains it: a write from inside
   `~/work/repo-a/` lands at `repo:repo-a`, not at `assoc:work`.
4. **Otherwise, `global`.** If the cwd matches no registered root, or two
   candidates tie at the best rank, the write lands at **global scope, exit
   0**. There is no refusal here.

**Step 4 is a real footgun and is documented because it is true, not because
it is good.** A write from an unregistered directory succeeds silently at
global scope, while the READ path refuses the same cwd outright:

```
$ cd /tmp/nowhere && planar plan list
error: cwd is not inside any registered Planar scope; cd into a registered
       scope or pass --scope global
(exit 1)

$ cd /tmp/nowhere && planar plan create "probe"
scope:    global
(exit 0)
```

Reads are the strict side, not writes. Editions of this document before
2026-09-11 asserted the reverse ("No silent default; no fallback to ambient
state") and named two error kinds — `AmbiguousScopeError` and
`OutsideRegisteredScopeError` — that exist nowhere in the source. The real
enum is `scope_error { query_failed, invalid_path, slug_not_found,
scope_mismatch }`.

**One registered-project exception.** A project registered with **no
association** does not resolve to `repo:<slug>`; it yields no scope, with the
reason `project_unassociated`. `plan create` turns that into an exit-5
refusal naming the `assoc create` / `assoc add` remedy; `task add` and
`scenario add` deliberately do not refuse and file under global. That split is
pinned by tests and is not an oversight.

### Workspace-root refusal — DOES NOT EXIST

Editions of this document before 2026-09-11 quoted a refusal here:

> you are in a workspace root with 3 member projects, but no specific project
> scope was passed. Choose one with --scope: …

**No such refusal fires, and no such message exists in the binary**
(`grep -rn "you are in a workspace root" src/` returns nothing). Measured in a
scratch arena — two git repos under a workspace root, `workspace init --scan
1` creating `org:work` with both as members — a `plan create` at that root
succeeds at **`org:work`**, exit 0, via step 3 above.

Whether that *should* refuse is open: the fan-out argument in the old text is
sound, and silently picking is the lectio incident pattern. But the refusal
was never implemented, and documenting it as though it were left operators
believing in a guard they did not have.

### Read resolution

Reads use `resolve_read_scope_set`. The contract:

- If `--scope` is set, parse and return that single resolved scope.
- Otherwise run `derive_from_cwd` and apply the same specificity ranking as the write path. Reads return a set of resolved scopes, not a single scope:
  - At a workspace root, the set is the org plus every member project (the operator's expectation of "show me everything under this workspace").
  - At a member project root or subdirectory, the set is the most specific registered repo. Longer `projects.root_path` matches beat shorter parent roots, so nested repos do not leak parent-repo work.
- If cwd matches zero registered scopes and no flag is passed, refuse with a clear message instructing the operator to `cd` into a registered scope or pass `--scope global` for the global slice. There is no silent fallback.

This composes naturally with `plan list`, `task list`, `question list`, `scenario list`, `decision list`, `artifact list`, `search`, and `tree` — these query verbs see the same cwd-derived set.

### Cross-scope guard

Layered on top of the write resolver, the [cross-scope guard](#cross-scope-guard) compares the operator's resolved scope against a target entity's stored `(scope_kind, scope_id)` and refuses if they disagree.

**It runs on ten verbs, not on every mutation** — see [§ Cross-scope guard](#cross-scope-guard) below for the measured list. All ten are membership-aware: an operator scope `assoc:<org>` covers an entity scoped to one of the org's member projects, via `project_associations`. The reverse direction — operator project, entity org — refuses with exit 5.

### Removed: the active scope stack

Earlier releases maintained a per-database `active_scope` table and exposed `planar scope use`, `planar scope pop`, and `planar scope clear` to manipulate it. Plan 153 M5 dropped the table (migration `migrations/00009_drop_active_scope.up.sql`) and removed the verbs; concurrent sessions sharing one database can no longer trample each other through stack manipulation. Operators who habitually typed those verbs get a retired-verb notice pointing at `planar scope show`, at **exit 2** (measured at task 6676; earlier editions of this page said exit 1). This holds for every arm — a trailing slug (`scope use someslug`) and an unknown flag (`scope use --bogus`) both still print the notice, because the retired stubs declare `allow_extras` so the handler runs before CLI11 can reject anything (task 6446). `planar scope push` and `planar scope swap` never existed at all.

**SQLite tables:** `associations`, `project_associations`, `projects`. **Primary verbs:** `planar scope show` (derived view), `planar scope suggest`. Set the scope for any verb by `cd`-ing into the target or passing `--scope <slug>`.

---

## Cross-scope guard

The cross-scope guard is a refusal mechanism that fires when the operator's resolved write scope disagrees with the target entity's stored `(scope_kind, scope_id)`. Guarded verbs read the entity's scope, resolve the operator's scope through the strict write resolver above, compare the two, and refuse with exit 5 if they differ.

### Why it exists

The motivating incident: an operator running from `~/work/lectio/` invoked `planar spec ingest 88` against a plan that belonged to a different project's scope. The verb materialised 126 child entities — plans, tasks, decisions, scenarios — under the cwd's association rather than the anchor plan's. The rows were recoverable but the silent cross-scope materialisation was the bug. The guard exists so verbs that walk *from* a parent entity *to* its children, or that mutate a specific existing entity, refuse to proceed when operator intent and entity provenance disagree.

### Which verbs are guarded

**Measured against the source at task 6075 (2026-09-11).** Ten verbs, eight
call sites (`sync push` and `sync pull` share one; `decision accept` and
`decision withdraw` share their transition helper):

| Verb | Comparison |
|------|-----------|
| `spec ingest <plan> --apply` | membership-aware |
| `feedback triage set` | membership-aware |
| `audit publish-decision` | membership-aware |
| `planar-ext sync push <link\|kind:id>` | membership-aware |
| `planar-ext sync pull <link\|kind:id>` | membership-aware |
| `planar-ext sync resolve <event-id>` | membership-aware |
| `decision accept` | membership-aware |
| `decision withdraw` | membership-aware |
| `task update` | membership-aware |
| `closure compute` | membership-aware |

The two classes the guard was designed around — bulk-write-from-parent (the lectio incident pattern) and mutating-an-existing-entity — describe its *intent*. They do not describe its coverage. Most mutating-existing-entity verbs are **not** guarded: `plan update`, `plan step add/done/skip`, `task done/reopen/block`, `question edit`, `scenario edit`, `decision edit`, `decision supersede`, `artifact update`, `annotate update`, `planar-ext ext create --from`, `planar-ext ext propagate-one`, `link`, `unlink`. Earlier editions of this document listed those as guarded; they never were.

`planar links update` appears in older editions of both documents. That verb does not exist — `planar schema` declares `links add`, `links list`, `links remove`, `links trail` only, and invoking `links update` fails at parse time with exit 2.

### One comparison, since decision 1121

Every guarded call site uses the cmd-layer `guard_with_membership`.

This was not always true. `task update` and `closure compute` called `engine::identity::check_scope_guard` directly — strict equality after `assoc:` normalization — so an `assoc:<org>` → `repo:<member>` write was **accepted** by `spec ingest --apply` and **refused** by `task update`, from the identical working directory. Task 6075 measured the split; decision 1121 (task 6735) resolved it by widening those two to match the other eight.

The reasoning: strict equality could not distinguish "my own member repo" from "an unrelated repo" — it refused both identically. That is not a stricter reading of the guard's purpose but a blind one. The guard exists to stop a write leaking *sideways* (repo A mutating an entity owned by unrelated repo B), not to stop an association acting on its own member, which is the ordinary case the association scope exists to serve.

### Membership-aware coverage

For all ten guarded verbs, the comparison is not strict equality. An operator scope `assoc:<org>` covers any entity whose stored scope is a project belonging to the org via `project_associations`. From a workspace-root cwd with `--scope assoc:work`, those verbs accept writes targeting `repo:repo-a`, `repo:repo-b`, and so on — the org operator is "above" its member projects. The reverse direction (operator `repo:repo-a`, entity in `assoc:work`) still refuses with exit 5.

### `--scope` does not mean the same thing on every verb

The guard's documented remedy — "pass `--scope <entity-scope>`" — assumes `--scope` selects the operator's **write scope**. Measured at task 6075, it carries four distinct meanings:

| Meaning | Effect | Verbs |
|---------|--------|-------|
| Write-scope selector | Sets the scope the write resolves under. | the create/add verbs, `task update`, `closure compute`, `spec ingest`, `feedback triage set`, `audit publish-decision`, `decision accept`, `decision withdraw` |
| **Patch field** | **Reassigns the entity's stored scope — it moves the row.** | `plan update`, `artifact update`, `annotate update` |
| Read filter | Restricts which rows are listed. | the `list` verbs, `search`, `tree`, `dashboard`, `health` |
| Inert | Accepted, parsed, discarded. | `planar-ext ext propagate` |

On `plan update`, `artifact update` and `annotate update` there is therefore **no way to authorize a cross-scope write with `--scope`** — it performs the move instead. Those verbs are unguarded today so nothing refuses, but the documented escape hatch does not exist for them and could not be offered without a new flag.
### Which verbs are deliberately not guarded

Several verb classes were audited and explicitly left unguarded; the absence is not an oversight:

- **All `*_link` and `links add/remove` verbs.** `entity_links` is cross-entity by design — the polyrepo `touches`/`derives-from` story depends on edges crossing scope boundaries.
- **All `*_add` / `*_create` verbs.** A newly-created entity has its own `--scope` resolved through the write resolver; `--plan` or `--parent` on a create verb is a reference, not scope inheritance.
- **`sync push --all` / `sync pull --all`.** Bulk fan-outs that the operator opts into explicitly.
- **`planar-ext ext propagate`.** `external_links` carries no scope column, so there is nothing to compare against. The verb accepts `--scope` and discards it; the handler carries an explicit `(void)flag_string(args, "--scope");` with a comment saying so. Older editions of `docs/cli-reference.md` listed this verb as guarded — it never was.
- **All read-only verbs.** `show`, `list`, `status`, `audit trail`, `tree` — reads do not corrupt state and the audit-from-anywhere case is the common case.
- **Identity-bucket verbs** (`assoc`, `init`, `promote`/`demote`, `scope`, `workspace`). Associations *are* scope; `promote`/`demote` deliberately cross scopes (that is the verb's job).
- **Operator-state verbs** (`handoff`, `capture`, `resume`). These manage vendor-session rows, not project-scoped entities. The legitimate polyrepo handoff workflow is "a session inside repo A captures a handoff that references a task in repo B".

### No escape hatch

There is no flag that downgrades a cross-scope-guard refusal to a warning. `--no-scope-check` does not exist on the current binary — `planar schema` declares it on no command, and passing it fails at parse time with exit 2 (`error: <cmd>: The following argument was not expected: --no-scope-check`). (The engine-layer `guard_write` primitive under `src/engine/identity/scope.cppm` does carry a `no_scope_check` bypass parameter and is unit-tested, but no `cmd/` handler ever calls it with `true`, so no verb can reach the bypass from the CLI.)

The only remedies for a cross-scope-guard refusal are `--scope <slug>` to assert explicit intent, or `cd` into the entity's owning repo so cwd derivation resolves correctly. A genuine mismatch — neither of those applied — fails outright. Note that the `--scope` remedy applies only to verbs where `--scope` selects the write scope; see [§ `--scope` does not mean the same thing on every verb](#--scope-does-not-mean-the-same-thing-on-every-verb) above.

See [docs/cli-reference.md § Cross-scope guard](cli-reference.md#cross-scope-guard) for the per-verb listing and the exact refusal message format.

---

## Association

An association is a named grouping of projects (repos). A single repo can belong to multiple associations. Associations are the main organizational primitive for work that spans more than one repo.

Associations have a `kind` that describes how they were formed:

| Kind | Meaning |
|------|---------|
| `org` | GitHub org or similar top-level org grouping |
| `project` | A cross-repo product or feature initiative |
| `client` | External client engagement |
| `personal` | Personal projects or scratch work |
| `ad-hoc` | Temporary grouping |
| `host` / `path` / `lang` | Auto-detected from git remote host, parent directory, or language ecosystem |

The `slug` on an association is the stable identifier used in scope references, config overrides, and workbench directory names. It must match `[a-z0-9:._-]+`.

`planar assoc detect` auto-detects associations from the current repo's git remote and parent path. `planar assoc create` creates one manually; `planar assoc add` adds a repo to it.

**SQLite table:** `associations`, `project_associations`. **Primary verbs:** `planar assoc create`, `planar assoc add`, `planar assoc list`, `planar assoc members`, `planar assoc detect`.

---

## Project

A project is a registered local working directory — what you'd call a "repo." It is identified by its `root_path` on disk and has a `slug` that is used in scope references (`--scope repo:<slug>`).

`planar init` registers the current directory as a project (or upgrades an existing registration) and applies any pending schema migrations. You only run it once per repo. Registration does not automatically create an association; human output gives the exact `planar assoc create` and `planar assoc add` commands for that next step.

Projects are read-only after `init` — the project record is not meant to be updated or deleted. Associations are the mechanism for grouping projects.

Use `--scope repo:<slug>` when a plan, task, question, scenario, decision, or artifact belongs to the repository itself. Use `--scope assoc:<workspace>` for cross-repo coordination work that intentionally sits above any one repository. A root repo and a nested repo can both be registered projects; cwd matching picks the longest root path so nested work does not collapse into the containing repo.

`planar plan create` refuses an implicit write from a registered project that has no association, because treating the missing association as global would hide an ownership mistake. Add the project to an association first, or pass `--scope global` explicitly when the plan is intentionally global.

**SQLite table:** `projects`. **Primary verbs:** `planar init` (there is no `project` domain; registered projects are visible through `planar assoc members <slug>`, `planar scope show`, and `planar tree --all-scopes`).

---

## Workspace

A workspace is a polyrepo grouping treated as a first-class operational surface. Mechanically it is an `associations` row of `kind=org` together with its `project_associations` members — no new table, no new schema. The default shape is a directory on disk (`~/work/`, `~/projects/`, etc.) that contains several sibling git repos. A git-backed meta repo can opt in with `planar workspace init --meta-repo`; that registers the root repo plus nested repos/submodules as member projects and records `workspace_shape = "meta-repo"` in the org config. Both shapes use the same canonical AGENTS.md content under the workspace state directory, but only sibling workspaces install root-level `AGENTS.md` / `CLAUDE.md` links.

### Why the concept exists

Cross-repo work cannot land cleanly in any single project's scope. When a feature touches `repo-a` and `repo-b`, the planning artifacts, decisions, and open questions belong above the repo level. The workspace surface gives that "above-the-repo" content a stable home: an org association for the scope and a state directory for the generated content. Agents working in any member repo can read the workspace's AGENTS.md without having to navigate up.

### State directory and symlinks

The canonical content for a workspace lives at `~/.planar/workspaces/<org_id>/`:

- `AGENTS.md` — generated; the human-readable routing surface.
- `routing-table.json` — generated; structured project map (capabilities, dependencies, summaries, open-work counts).
- `config.toml` — optional; per-workspace settings (`enrich_command`, etc.).
- `routing-table-overrides.json` — optional; operator overrides merged on every routing build.

For sibling workspaces, two symlinks at the workspace root (`<workspace-root>/AGENTS.md`, `<workspace-root>/CLAUDE.md`) point at the same canonical `AGENTS.md` target so Codex / Copilot (which read `AGENTS.md`) and Claude Code (which reads `CLAUDE.md`) see identical content. On filesystems that reject symlinks the installer falls back to a regular-file copy and records the degraded mode so regeneration rewrites the copy.

For meta workspaces, the root is itself a versioned repository. Planar still writes canonical generated content under `~/.planar/workspaces/<org_id>/`, but it does not create, symlink, copy, overwrite, or repair root-level `AGENTS.md` / `CLAUDE.md`. Existing files at the meta root remain repo-owned; missing files remain missing.

`workspace doctor` is fail-closed around this policy. It repairs root guidance links only after it has read a valid non-meta workspace config. Missing, malformed, or unknown `config_json` shape is reported as an issue and root guidance repair is skipped, so an uncertain meta workspace is not accidentally rewritten as if it were a sibling workspace.

### Two-pass routing

The routing table is built in two passes. The static pass is always-on and deterministic: README first paragraph, manifest detection for capability tags, dependency inference from `go.mod` replace / `package.json` workspace deps, language census, and live Planar focus queries (open tasks, open questions, active plans). The LLM enrichment pass is opt-in via `pl-workspace-scan --enrich` or `planar workspace routing build --enrich`; it merges cached LLM results into the table, keyed by a content fingerprint so unchanged repos do not re-spend tokens. Manual overrides always win over enrichment, which always wins over static signals.

### Bare-init guardrail

`planar init` refuses when cwd has no `.git` but contains child repos — without the guardrail, a bare init in `~/work/` would register a semantically-wrong project row for the workspace directory itself. The refusal points at `planar workspace init`; `--allow-no-repo` is the escape hatch for the rare standalone non-repo case. Conversely, bare `planar workspace init` refuses from a git root so ordinary single repos still use `planar init`; meta repos must pass `--meta-repo` explicitly. In meta mode, reusing an existing org slug is allowed only when that org's recorded root matches cwd; a different recorded root is refused and the existing org config is not rewritten.

See [docs/architecture.md § Workspace State Directory Model](architecture.md#workspace-state-directory-model) for the full layout and [docs/workflows.md § Recipe 12](workflows.md#recipe-12--working-in-a-polyrepo-workspace) for the end-to-end recipe.

**SQLite tables:** `associations` (the `kind=org` row), `project_associations` (member projects). **Primary verbs:** `planar workspace init`, `planar workspace doctor`, `planar workspace routing build`, `planar workspace routing show`, `planar workspace regenerate`.

---

## Transcription vs Synthesis

Two verbs onboard an existing repo into Planar. They share the downstream `/pl-spec-ingest` pipeline but enter from different contracts; the choice is load-bearing.

- `import` is a **transcription** verb. It reads the repo's existing planning docs and emits them as Planar artifacts as-is. Bullets in a roadmap become tasks verbatim, frontmatter dictates classification, and status inference is bounded by an explicit confidence floor. Reach for it when the repo's planning material is clean, structured, current, and largely correlates with shipped code.

- `synthesize` is a **synthesis** verb. It reads both the existing docs *and* the source tree as input, then produces fresh `product_spec` / `tech_spec` / `roadmap` artifacts via an LLM pass. The original docs are preserved on the same anchor plan as `kind=research` reference artifacts — superseded but not deleted. Reach for it when the planning material is scattered across multiple drafts, mid-evolution, or contradicted by reality (e.g. a roadmap claims a milestone is done but no source files back the claim).

The load-bearing rule for `synthesize` is that **code presence beats text claims**. A task the LLM proposes with `status != "todo"` must cite a `code_evidence` path that exists in the probed source tree; the binary's `validate_result()` step (`src/engine/synthesize/synthesize.cpp`) refuses results that violate the invariant. A roadmap line that says "M3 is finished" is treated as TODO unless source files, tests, or CI configs corroborate the claim. The greenfield case (no source detected) collapses naturally onto all-todo output.

### Picking between the two

| Repo shape | Pick |
|---|---|
| Clean structured docs + recent, code matches docs | `import` |
| Multiple roadmaps, ambiguous statuses, partial implementation | `synthesize` |
| Docs-only, no source code yet (greenfield) | `synthesize` |
| `docs/` + complete code + tests with reliable status correlation | `import` (consider `--interpret` for LLM polish) |

Both verbs land in the same downstream pipeline: artifacts in the workbench, review by the operator, then `/pl-spec-ingest` to decompose into the plan / task graph. The only divergence is at the entry point — what counts as the authoritative planning material.

### Concrete examples

**Greenfield (docs-only).** Your repo has `docs/product-roadmap.md` describing five phases but no source code yet — just the planning material and a README. Run `synthesize`: codeprobe finds zero source-file evidence, the request is flagged greenfield, and every task lands `status=todo`. Run `import` instead and git-log correlation matches the roadmap-adding commits to dozens of phase-1 task titles, marking 100+ tasks done because the commits *added* the roadmaps. The synthesis path makes the false-done problem structurally impossible.

**Docs-with-code (clean).** Your repo has conventional `docs/<project>_<kind>.md` naming, a working `Sources/` tree, and tests under `Tests/`. Run `import` for a faithful transcription that preserves the existing structure exactly. Run `synthesize` for an LLM-curated reorganization grounded in code-evidence: tasks are graded by what the source tree actually shows, and `code_evidence` citations annotate every non-todo task. Both produce similar plans; the choice is whether you want the repo's own structure or a fresh LLM read.

**Mid-evolution (docs lie).** Your roadmap claims Phase 2 is done; your code shows `internal/sessions/` is empty. Run `synthesize`: Phase 2 lands `status=active` (or todo) despite the roadmap claim because the EvidenceMap has `source=none, tests=none` for that area. The original Phase 2 done-claim survives as a `kind=research` reference artifact on the anchor plan, so operators see both narratives — the synthesized version is primary, the original docs are reference. Run `import` here and you'd transcribe the doc-claim verbatim, propagating the false-done into Planar's data model.

See [docs/cli-reference.md](cli-reference.md) for the full `import` and `synthesize` flag tables, and [docs/workflows.md](workflows.md) for end-to-end onboarding recipes for each repo shape ([Recipe 7](workflows.md#recipe-7--adopt-an-existing-repo-into-planar), [Recipe 7a](workflows.md#recipe-7a--greenfield-onboard-with-synthesize), [Recipe 7b](workflows.md#recipe-7b--docs-with-code-onboard-with-synthesize), [Recipe 7c](workflows.md#recipe-7c--mid-evolution-onboard-with-synthesize)).

---

## Plan

> Every status lifecycle on this page, plus the engine roll-ups and multi-verb workflows, is drawn as a diagram in [lifecycles.md](lifecycles.md).

A plan is the anchor unit of work. It is a structured intent — a named body of work with a status lifecycle. Plans are hierarchical: a child plan has a `parent_plan_id` and belongs to its parent's feature tree.

### Status lifecycle

```
draft → active ⇄ paused
          ↓
        done  (terminal)
          |
       abandoned  (terminal)
```

Legal transitions (enforced by `check_transition` in `src/engine/planning/transitions.cppm`):

| From | To |
|------|----|
| `draft` | `active` |
| `active` | `paused`, `done`, `abandoned` |
| `paused` | `active` |
| `done` | — terminal; no operator escape path |
| `abandoned` | — terminal; no operator escape path |

- `draft`: planning documents are being authored. The workbench tree exists but tasks may not yet be created.
- `active`: tasks are being executed.
- `paused`: work is interrupted; resumable via `plan update --status active`.
- `done`: all tasks complete. Terminal for operator transitions.
- `abandoned`: work stopped without completion. Terminal for operator transitions.

`recompute_status` in `src/engine/planning/plan.cpp` (triggered by task writes) deliberately bypasses this check — it is an engine-internal aggregate roll-up whose target is computed by `compute_target` and can only emit transitions the matrix considers valid. Operator overrides go through `plan update --status <s>`.

A plan has a filesystem-safe `slug` unique within its parent scope, used in workbench directory names.

The anchor plan for a feature is the top-level plan with no `parent_plan_id`. Child plans are used for sub-features or roadmap milestones within a larger feature.

**Status auto-promotion (plan 304).** Plan status is a function of task status, enforced at task-write time. The auto-promotion invariant fires inside every task add / update / done / reopen / cancel / block transaction and applies a transition matrix that flips the plan based on the post-write task aggregate. The matrix lives in `agents/methodology.md` § Plan-status invariant.

Two operator-visible consequences:

- **Child plans auto-promote.** A child plan flips `draft → active` when any task starts, and `active → done` when every task is terminal (done or cancelled). The orchestrator no longer needs to walk child plans manually — `planar task done <id>` on the last task flips the parent child plan to `done` in the same transaction.
- **Anchor plans don't auto-promote to done.** The anchor's `done` transition is a release-gate decision; the invariant only auto-flips anchors to `active`. Operators close an anchor explicitly with `planar plan update <id> --status done`. A plan's status also depends only on its OWN tasks — a child plan being done does NOT propagate to its parent anchor's status.
- **Paused and abandoned are operator overrides.** Both are no-ops for the recompute. Re-engage with `plan update --status active`.

The opt-out is `--no-auto-promote` on the task verbs, used by migrations and scripted bulk edits that don't intend the plan-level transition.

Each roll-up transition writes an `audit_log` row with `verb='status_change'` and a free-text summary; read it back with `planar audit trail --kind plan <plan-id>`. (Earlier editions of this page described a `session_entries` note beginning with a `plan_status: <id>` sentinel; no such sentinel is written anywhere in `src/` — see `docs/lifecycles.md` § 7.)

**SQLite table:** `plans`. **Primary verbs:** `planar plan create`, `planar plan show`, `planar plan list`, `planar plan update --status active|paused|done|abandoned`, `planar plan closeout`. (There are no `plan active` / `plan done` / `plan abandon` verbs; each fails at parse time with exit 2.)

### Closeout gate

`planar plan closeout <plan-id> [--dry-run] [--check-merge] [--json]` is the operator delivery-evidence gate for explicitly closing a plan.

**DB-evidence (hard gate — all must pass before apply writes anything):**

1. All tasks on the plan and its descendants are `done` or `cancelled`. Cancelled tasks are terminal and do **not** block — they count toward the audit summary.
2. All descendant plans (recursively via `parent_plan_id`) are `done` or `abandoned`.
3. No `active`, non-expired `agent_work_claims` exist on the plan's tasks. Expired/stale claims are advisory warnings.

**Finalization tasks (advisory labeling):** The hard-evidence section reports a `finalization_tasks` count — tasks whose slug begins with `finalize-`, `merge-`, or `reconcile-`. These are tasks the janitor/orchestrator creates to track merge or reconciliation work as part of Phase 3.7 Finalization. The count is informational only; finalization tasks follow the same terminal rules as any other task and do NOT change the gate logic.

**Git-evidence (advisory — reported, never blocks):** Best-effort ancestry checks from `agent_work_claims` locality columns (`repo_root`, `branch`, `head_sha_at_claim`). When no locality data is recorded, the section reports `"no commit attribution — inconclusive (hardens once session-commit capture is wired)"`. Git failures (not a repo, git missing, branch absent) produce descriptive notes but never prevent apply.

**Epic-branch merge check (`--check-merge`, advisory):** When supplied, reports an `epic_merge` roll-up: for each distinct contributing branch from `agent_work_claims`, checks whether it is merged to the detected target branch. The roll-up is `null` when no locality data exists; absent branches (deleted post-merge) are inconclusive and excluded from the count. Never blocks apply.

**Who can call it:** This is an **operator verb** on the `planar` binary, not `planar-agent`. The **janitor** is the authorized agent caller — it runs `planar plan closeout` on behalf of the operator after delivery evidence is verified (Phase 3.7 Finalization). Coders use `planar-agent complete` to close tasks and claims, never plans.

**Apply semantics:** Without `--dry-run`, passing the hard gate marks the plan `done` directly — bypassing the `recompute-status` anchor cap. This is intentional: `plan closeout` is the explicit operator release-gate for anchor plans. `--dry-run` evaluates and reports without writing. **Only apply mode exits non-zero when the hard gate is blocked** — `--dry-run` always exits 0, because a preview must let its caller read `ready` / `blocked_by` from the report and decide. Editions of this page before 2026-09-12 said both modes refuse; they never did (task 6319).

---

## Task

A task is the leaf unit of work. It is attached to a plan via `plan_id` and optionally to a parent task via `parent_task_id` (subtasks). Tasks are the entities that agents implement.

### Status lifecycle

```
        ┌──────────────────────┐
        ↓                      |
todo ⇄ doing ⇄ blocked → done  (terminal)
  ↖___________↙  ↓
  ↘     ↓        ↓
cancelled (terminal)  cancelled (terminal)
```

`blocked → todo` was added at task 6441. Every other exit from `blocked`
already existed, but none of them meant "the blocker cleared and this is
queued again": `doing` claims work is in progress, and `done`/`cancelled` are
terminal. The sanctioned recovery had been to cancel the task and then
`reopen --reason` it — writing a cancellation that never semantically happened
into the audit trail, for an ordinary lifecycle event a dependency-bearing
plan hits every time a blocker closes.

Legal transitions (enforced by `check_transition` in `src/engine/planning/transitions.cppm`):

| From | To | Notes |
|------|----|-------|
| `todo` | `doing`, `blocked`, `cancelled` | |
| `doing` | `todo`, `blocked`, `done`, `cancelled` | |
| `blocked` | `todo`, `doing`, `done`, `cancelled` | `todo` requeues an unblocked task without claiming work started (task 6441). |
| `done` | `todo`, `doing`, `blocked` | Only via `task reopen --reason` or `task update --force` |
| `cancelled` | `todo`, `doing`, `blocked` | Only via `task reopen --reason` or `task update --force` |

- `todo`: task is queued, not yet started.
- `doing`: an agent is actively working on it.
- `blocked`: work is stalled; requires `next_action` — `planar resume validate` refuses a resume packet without it.
- `done`: task is complete. Terminal for bare `task update`; escape via `task reopen --reason <why>`.
- `cancelled`: task was deliberately dropped. Terminal for bare `task update`; escape via `task reopen --reason <why>`.

**Verb-gated escape from terminal status.** `planar task reopen <id> [--status todo|doing|blocked] --reason <why>` performs the terminal → open move that bare `task update --status` refuses, and records a `task_reopens` audit row. `task update --force` is the operator override that also performs the move and records a `task_reopens` row with `source='task-update-force'`. Both paths bypass the matrix explicitly; the bypass is the documented exception, not the default.

**Automatic `blocked -> todo` when the last blocker closes** (task 6754,
decision 1122). Moving a task to `done` or `cancelled` -- through
`planar task done`, `planar task cancel`, or `planar task update --status`
alike -- scans the tasks that `depends-on` it. Any such dependent that is
still `blocked` AND has no remaining blocker outside `done`/`cancelled` is
moved to `todo` in the same operation. The rule is ALL-CLEAR, not any-clear: a
dependent with two blockers stays `blocked` until both are terminal. A
dependent an operator has already moved off `blocked` (to `doing`, say) is
left alone. The automatic move records its own audit row with the summary
`unblocked: task <blocker-id> is terminal`, so it stays distinguishable from
an operator's own `blocked -> todo` after the fact. This is the lifecycle
event `blocked -> todo` was added for at task 6441; before 6754 it had to be
performed by hand on every dependent, and in practice was not, leaving stale
`blocked` rows behind closed blockers.

**Identity transition** (`from == to`) is accepted silently by all arms — a redundant `--status doing` on a `doing` task is a no-op, not a refusal.

Tasks carry a `title`, an optional `body` (Markdown), a `next_action` field for handoff continuity, and a `scope_kind`/`scope_id` pair that records which scope they belong to.

### Claim-atomic operator transitions

Operator-driven status transitions (`task done`, `task block`, `task reopen`, `task update --status`) are **claim-atomic**: when the task has an active work claim (`agent_work_claims.status = 'active'` and `lease_expires_at >= now()`), the operator verb refuses the status flip and exits non-zero with a message identifying the active claim.

This closes the TOCTOU window where an operator `task done` would strand a live agent lease mid-flight. The correct closure path for a claimed task is the agent terminal verb (`planar-agent complete | fail | release`). The operator override is `--force`, which bypasses the claim guard AND the status-transition matrix — use it only when the agent is known to be no longer active (e.g. the process crashed without releasing its claim).

Expired claims (`lease_expires_at < now()`) are NOT active and do not trigger the guard. The check is real-time on every operator status-flip.

**SQLite table:** `tasks`. **Primary verbs:** `planar task add`, `planar task list`, `planar task show`, `planar task update --status doing` (there is no `task doing` verb), `planar task done`, `planar task block`, `planar task cancel`, `planar task reopen`.

### Claim-owned task state and recovery

Agent dispatch has one ownership ritual: acquire a claim, heartbeat it at
TTL/2, then invoke exactly one terminal verb. Both `planar-agent pull <plan>`
and the default `planar-agent claim --entity task:<id>` atomically acquire the
claim and move an eligible task from `todo` to `doing`. A direct claim also
opens a `claim_check` action in that transaction. The marker proves which
claim owned the status transition; it is not a synthetic completion event.
`--no-transition` retains the primitive claim-only behavior and creates no
marker.

Normal closure is atomic: `complete`, `fail`, `release`, or `block` updates the
claim, its action, and the task together. `fail` restores the task to `todo`
and records one closed failure category: `usage_limit`, `context_limit`,
`output_limit`, `tool_failure`, `validation`, or `unknown`. The default is
`unknown`. Successful, released, and blocked claims do not acquire a category.

Recovery preserves that ownership proof. `abort` can restore a default direct
claim's `doing` task to `todo` and close its `claim_check` marker only when no
live replacement claim owns the task. `reconcile` performs the equivalent
restoration for expired pull or direct claims with action evidence, and leaves
primitive claims alone. Both operations make the claim transition, task reset,
marker closure, and optional failure classification in their existing
transaction. Always preview a sweep with `planar-agent reconcile --dry-run
--json`; recovery never mutates a live, unexpired claim.

---

## Dispatch shapes

The orchestrator's Phase 3 dispatch gate offers six named **dispatch shapes** — each a distinct point on the *grouping* × *reviewer disposition* matrix. The shape picks how many tasks land in one coder cycle AND when (or whether) the reviewer is dispatched. The operator picks one shape per `/pl-orchestrator` invocation at the gate; the choice is recorded as a `session_entries` row (with `prefix='note'` + the sentinel body line `dispatch_shape: <shape>`) for the audit trail.

| Shape              | Grouping                     | Reviewer disposition  | Pick when |
|--------------------|------------------------------|-----------------------|-----------|
| `strict`           | One cycle per task           | Per task              | Fine-grained history; spec/schema changes; logic changes. |
| `grouped`          | Orchestrator-picked grouping | Per group             | Tasks share file scope; one review covers them all. |
| `single`           | All in one cycle             | Once                  | Tiny features where decomposition is theatre. |
| `barrel-grouped`   | Per milestone (locked in)    | Per group             | Throughput + per-group review. Alias for `grouped` with the milestone heuristic. |
| `barrel-deferred`  | Per milestone (default)      | Deferred at boundary  | Throughput + late review safety net. `--barrel-deferred-at plan` for once-per-plan. |
| `barrel-bypass`    | Per milestone (default)      | None                  | Maximum throughput. Quality gates ARE the review signal. |

The three `barrel-*` shapes formalize what was previously an emergent shortcut: barrel through milestones back-to-back, accept the gates as the entire signal, and defer (or skip) the reviewer. Naming them turns the shortcut into a contract.

**Phase 3.5 (test-coder dispatch) fires across all shapes** when uncovered slugs intersect the cycle's slugs. `barrel-bypass` bypasses the *reviewer*, not the *coverage gate*. The test-coder's `failure-surfaced` outcome halts the cycle regardless of mode.

**Iteration-5 cap.** Applies per reviewer dispatch under `strict`/`grouped`/`single`/`barrel-grouped`. Applies to the boundary reviewer dispatch under `barrel-deferred` (on the union diff). Undefined under `barrel-bypass` (no reviewer → no `request-changes` → no iteration).

**Audit trail.** Every cycle emits a `session_entries` row with `prefix='note'` and a structured body that begins with the sentinel line `dispatch_shape: <shape>`. The `session_entries.prefix` CHECK constraint allows a fixed set (`action / observation / decision / question / file / command / note / error / read`); the dispatch convention reuses `note` with the sentinel body as the grep-recoverable alternative — the same sentinel-in-note pattern plan 304 originally specified for plan-status flips (which the C++ roll-up records in `audit_log` instead).

```
dispatch_shape: <one of the six>
reviewer_disposition: <dispatched|skipped-by-profile|deferred|bypassed>
cycle_scope: plan:N milestone:M | task:T...
tasks: [<id>, <id>, ...]
claim_tokens: [<token>, <token>, ...]
model_tiers: {<task-id>: <tier>, ...}
model_choice: {"<task-id>":{"tier":"<tier>","candidate":"<model-id>","work_type":"<work-type>"}, ...}
```

Recover the per-cycle disposition with `planar audit trail --kind plan <plan-id> --grep "^dispatch_shape:"`. `model_tiers` records the confirmed tier assignment, and `model_choice` records the concrete routed candidate and work type used by `planar models evals`. No schema change; the sentinel-body convention is the contract.

**Pick-when summary:** when in doubt, pick `strict`. Move up the table (toward throughput) when you have high confidence in the gates and the spec, or when the diff cadence makes per-cycle reviewer dispatch wasteful. The orchestrator never picks a barrel mode silently — every shape change is an explicit operator choice at the gate.

For the canonical contract see [`agents/methodology.md` §Barrel modes](../agents/methodology.md#barrel-modes). For the skill-flag surface see [`skills/src/pl-orchestrator.md`](../skills/src/pl-orchestrator.md).

**SQLite tables:** none beyond `session_entries`. **Primary entry points:** `/pl-orchestrator` (the gate), its barrel-modes contract, `planar audit trail --kind plan <plan-id>` (the forensic surface).

---

## Orchestration strategy

An orchestration strategy is the operator-facing dispatch frame for a plan. It bundles five underlying axes into one named choice the operator confirms at the strategy gate — Phase 3's first sub-step, run **before** the existing dispatch-shape gate. Strategy answers "what is the overall methodology for this plan?" Dispatch shape (the next gate, nested under it) answers "within that strategy, how do I batch *this cycle's* work?"

### The named strategies and isolation choice

| Strategy | One-line description |
|----------|----------------------|
| `classic` | Sequential cycles, reviewer per cycle. Default isolation is `pwd` on the current branch; `worktree` isolation is selectable when rollback or pwd hygiene matters. |
| `parallel-fanout` | Fan out to N parallel coders on the parallel-eligible subset of the plan's open tasks; each in its own worktree off the shared epic branch; staged into dependency-respecting waves; one consolidated reviewer pass at fan-in. **Model-runnable** via the spawn-free `workflows/parallel-dispatch.lua` seam (plan 760), an optional deterministic helper — the model orchestrator, a host-native workflow, or a background agent may run this path (decision 1007). |
| `isolated-sequential` | Descriptive alias for `classic` + `worktree` isolation: one cycle worktree per task, reviewer per cycle. |
| `barrel-deferred` | Coder cycles run back-to-back; reviewer fires once at a milestone or plan boundary on the union diff. Supports both `pwd` and `worktree` isolation. |
| `barrel-bypass` | No reviewer dispatch at all. Quality gates (`make fmt-check` + `make build` + `make test` twice + `make coverage` + `make cli-usage-check` + render check + remaining validators) are the entire signal. Supports both `pwd` and `worktree` isolation. |

The model-driven `/pl-orchestrator` skill runs the sequential strategies in either `pwd` or `worktree` isolation and runs `parallel-fanout` in worktrees. Worktree bookkeeping is driven via the spawn-free `workflows/parallel-dispatch.lua` seam: `cycle_plan` computes one sequential lane; `plan`/`waves` compute fan-out lanes. The seam only computes and hands back — it is an optional deterministic helper, not the only permitted path. The runner that acts on the hand-back, running the git worktree/branch/merge ops and spawning the coders, may be the model orchestrator, a host-native workflow, or a background agent (decision 1007, plan 1033). In-flight worktree execution is watched through the existing `planar-watch ps --plan <id>` surface (claims + each claim's `worktree_path`); there is no dedicated wave/barrier view (a recorded non-goal).

### Declaring what a task touches

Parallel eligibility is computed from what each task **declares** it touches. Rule 2 treats an empty touch set as "touches everything," so an undeclared task is never eligible. That default is deliberate: the failure mode of omission is *safe* — an undeclared task serializes rather than falsely parallelizing.

It is also the binding constraint in practice. Nothing populated `task_touch_paths` but hand declaration, so most open tasks were serialized for lack of a declaration rather than for genuine conflict — and the rows are load-bearing twice over, since [closure extraction](planar-spec-v0.1.md#1-the-central-term-context-closure) uses them as its seeds and reports `NoSeeds` without them.

`planar task touches infer <task-id>` closes that gap. It reads the task's own title, body, and next_action, resolves the path-shaped tokens against the repo tree, and proposes `task_touch_paths` rows. It **previews by default**; `--apply` writes.

**Inference biases toward over-declaration** (decision 906). A directory token expands to its files, a bare basename yields every match rather than one guess, and an unplaceable token is reported rather than dropped. The reason is that the two error directions are not symmetric:

| Direction | Cost | Recoverable |
|-----------|------|-------------|
| Over-declare | Throughput — the task serializes when it might have run in parallel. | Yes: declare more precisely. |
| Under-declare | Correctness — two tasks marked eligible, fanned into separate worktrees, both editing the same file, colliding at fan-in. | No: both cycles are already spent. |

Inference cannot tell which of the two it produced. The operator can, which is why nothing is written without confirmation. Expect the preview to include files a task merely *cites* rather than edits — pruning those in review is the intended workflow, not a defect.

**Proposing wide is not the same as writing wide.** The bias above governs what inference *proposes*; what it *writes* is narrower. Only exact path matches are written by default. Directory and basename expansions are shown with their expansion size and withheld unless `--wide` is passed, because measurement showed they reduce eligibility rather than increasing it:

| Policy | Parallel-eligible (46 tasks, six plans) |
|--------|------------------------------------------|
| Nothing declared | 0 |
| Exact matches only | **14** |
| Exact + wide expansions | 13 |

The over-declare row above is understated for wide sets. Rule 2 drops **both** sides of an overlap, so an over-declared task removes its *peers* from the eligible set as well as itself — while an undeclared task removes only itself. In one plan, four tasks each mentioned `skills/src/` in prose; expanding it gave all four the same 35 paths, and they mutually overlapped *and* dragged down the one task with seven genuinely distinct real paths. One eligible task became zero.

So over-declaration is recoverable only while it stays narrow enough not to intersect everything. Past that point the cost propagates across the plan rather than staying with the declaring task.

Note that inference improves rule 2 only. Dependency edges (rule 1) are not inferred: two tasks can touch genuinely disjoint files and still be ordered, as when one imports a module the other creates. Declare those with `planar task block <task> --on <blocker>`.

### The five underlying axes

Every strategy is a row in the axis table — locked values for each:

| Axis | Values | What it controls |
|------|--------|------------------|
| `isolation` | `in-pwd`, `worktree` | Where the coder runs — operator's checkout vs. a dedicated worktree. |
| `branch_model` | `current-branch`, `epic-child` | Where commits land — current branch vs. `cycle/<plan-slug>/<task-slug>` off `epic/<plan-slug>`. |
| `concurrency` | `sequential`, `fan-out` | How cycles batch — one at a time vs. N parallel coders per cycle. |
| `reviewer_cadence` | `per-cycle`, `per-fanin`, `at-boundary`, `gates-only` | When the reviewer runs. |
| `test_coder_cadence` | `per-cycle`, `per-fanin`, `at-boundary`, `none` | When the test-coder (Phase 3.5) runs. |

Advanced operators can compose a custom strategy with `--strategy custom` plus per-axis flags (`--isolation`, `--branch-model`, `--concurrency`, `--reviewer-cadence`, `--test-coder-cadence`). The named bundles are the recommended common cases; the axis flags are the escape hatch.

### The two gates

Phase 3 runs the strategy gate first, then the dispatch-shape gate nested under the chosen strategy:

1. **Strategy + isolation gate.** The model orchestrator surfaces its recommendation + a one-line rationale + the runnable strategy menu (`classic` / `barrel-deferred` / `barrel-bypass` / `parallel-fanout`) and the isolation choice (`pwd` / `worktree`) where applicable. Operator confirms or overrides. Skipped only when `--strategy <name>` plus any needed axis flags was passed at invocation. The recommendation algorithm is plan-shape-driven with status-quo bias — see `agents/methodology.md` § Recommendation algorithm.
2. **Dispatch-shape gate.** Constrained by the strategy: `parallel-fanout` forces the `fan-out` shape; `barrel-deferred` and `barrel-bypass` force their matching shapes; `classic` keeps the full strict / grouped / single menu.

Neither gate has an auto-default — the recommendation never silently turns into an action. `classic` is the continuity guarantee: an operator who always picks (or accepts the recommendation of) `classic` sees no behavioral change relative to today.

### Provider-scoped capacity containment

Parallel waves are bounded before dispatch. When a lane terminates with
`usage_limit`, `context_limit`, or `output_limit`, the orchestrator opens an
in-memory circuit breaker only for that lane's provider. It preserves landed
work, allows already-running lanes to finish normally, continues eligible
lanes on unaffected providers, and starts no later lane on the affected
provider until the operator explicitly resets dispatch and confirms a new
maximum wave size. `tool_failure`, `validation`, and `unknown` remain
non-systemic lane failures.

`workflows/parallel-dispatch.lua --phase capacity_reconcile` computes this
partition from supplied lane outcomes and returns `landed`, `running`,
`unaffected`, `provider_blocked`, `retryable`, `abandoned`, and `unfinished`
sets plus exact resume and recovery reads. It is not a daemon or provider API:
the phase uses only `flow.*`, persists no breaker, performs no spawn, and never
aborts or reconciles a claim. A dead lane remains an explicit operator recovery
choice after claim inspection and a reconciliation dry-run.

### Persistence

No new schema beyond the existing `agent_actions.metadata` JSON column from migration 00016. The chosen strategy + axes ride on the dispatch row there, and the "last-used strategy for this plan" lookup that drives the recommendation algorithm's stickiness rule is a single indexed read against the most recent dispatch entry's metadata.

For the canonical axis table, named bundles, invalid-combination list, and recommendation algorithm see `agents/methodology.md` § Orchestration strategies. For the "pick a strategy" recipe and a worked `parallel-fanout` example see [`docs/workflows.md` §Recipe 21](workflows.md#recipe-21--pick-an-orchestration-strategy-for-a-plan) and [§Recipe 22](workflows.md#recipe-22--orchestrate-a-multi-task-plan-with-parallel-coders).

**SQLite tables:** none — strategy is metadata on the dispatch row. **Primary entry points:** `/pl-orchestrator` (the gate), its orchestration-strategies contract.

---

## Worktree

A Planar worktree is a git working tree created for an isolated coder cycle. It is a real `git worktree add` checkout — Planar does not reinvent the git primitive, it just owns the path convention and the persistence of which claim owns which worktree.

**Worktree lifecycle — creation, the epic/cycle branch model, fan-in merge, failed-lane retention, and full teardown on plan completion — is driven by the model orchestrator, a host-native workflow, or a background agent (decision 1007) for both sequential worktree isolation and `parallel-fanout`**. The deterministic wave/lane/merge/teardown computation lives in the spawn-free `workflows/parallel-dispatch.lua` seam, an optional deterministic helper: `cycle_plan` computes one sequential lane, and `plan`/`waves` compute fan-out lanes. Whichever of the three is driving still runs the git ops and spawns the coders itself. Parallel eligibility applies only to `parallel-fanout`; a single sequential worktree lane does not require `task_touches`.

### Topology — epic + child, main checkout stays on master

Worktrees come in two shapes, both rooted at the task's owning repo (not the operator's cwd repo, which may differ under a polyrepo workspace):

| Worktree | Path | Branch | Lifetime |
|----------|------|--------|----------|
| Epic (integration) | `<repo>/.worktrees/epic/<plan-slug>/` | `epic/<plan-slug>` | Created on first dispatch of any task in the plan; persists for the plan's duration; removed after the operator merges the epic into master. |
| Cycle (per-cycle working tree) | `<repo>/.worktrees/cycle/<plan-slug>/<task-slug>/` | `cycle/<plan-slug>/<task-slug>` | Created at cycle dispatch; removed after reviewer approval. |

**Topology invariant: the main checkout stays on master throughout worktree-isolated orchestration.** Both the epic branch and each cycle's child branch live in their own worktrees off the main checkout. The fan-in merge runs inside the *epic* worktree (`cd <repo>/.worktrees/epic/<plan-slug>/`), never in the main checkout. This is what preserves the "operator pwd stays clean" promise that motivates the worktree strategies.

The `epic/` and `cycle/` prefixes are **disjoint top-level branch namespaces** by design: git refuses any ref whose path is a strict prefix of another existing ref, so the older bare `<plan-slug>` + `<plan-slug>/<task-slug>` pairing would collide on plans whose slug appears in a task slug. The prefixes guarantee no ref-hierarchy collision.

### Persistence on `agent_work_claims.worktree_path`

When the orchestrator (or harness) dispatches into a worktree, it persists the absolute path on the claim row's `worktree_path` column (introduced in migration 00015 — see [`docs/architecture.md` §Application tables](architecture.md#application-tables)). Under `parallel-fanout` each fanned-out coder acquires its own claim with `planar-agent pull --worktree <path>`, so the N concurrent lanes are each observable through the persisted path. The persistence model is deliberately claim-attached, not a standalone `worktrees` table:

- **Resume reads it.** `planar resume <task>` surfaces `active_claim.worktree_path` in its JSON output and as a `cd:` line in the text packet so a cold-start resumer can `cd` into the same checkout the prior session was running in.
- **`planar-watch` surfaces it.** Every claim-bearing view (`claims`, `log`, `feed`, `ps`) and `planar dashboard --agents` include the column.
- **`planar-agent pull` and `claim --entity` accept `--worktree <path>`** as the canonical write path.

The standalone-entity alternative remains available — the forward-compat `validate_worktree_id` hook in `src/engine/runtime/agentactivity.cpp` is the seam — but the claim-attached model satisfies every current use case (dispatch persistence, resume recovery, observability).

### Which strategies use worktrees

| Strategy | Worktree? |
|----------|-----------|
| `classic` | Operator choice: `pwd` by default, or one cycle worktree per task. |
| `parallel-fanout` | Yes — N cycle worktrees per wave off the persistent epic worktree, dispatched concurrently. *(model-runnable via `workflows/parallel-dispatch.lua`, plan 760)* |
| `isolated-sequential` | Yes — alias for `classic` + `worktree`. |
| `barrel-deferred` | Operator choice: `pwd` by default, or sequential cycle worktrees with reviewer at boundary. |
| `barrel-bypass` | Operator choice: `pwd` by default, or sequential cycle worktrees with gates-only review. |

### Scope inside a worktree

Two invariants govern scope behavior when cwd is inside a worktree:

1. **The parent repo dictates the scope.** A worktree at `<repo>/.worktrees/{epic,cycle}/...` resolves to the same association as `<repo>`. Worktrees are not separately scoped; they inherit. Reads (`planar plan list`, `planar task show`, `planar-watch *`) work transparently from inside a worktree.
2. **Planning verbs are refused from inside worktrees.** Verbs that mutate planning state (`plan create/update`, `task add/update/done/touches`, `question`, `decision`, `artifact add/update`, `scenario`, `spec ingest`, `link/unlink/links`, `assoc`, `promote`, `demote`, `init`) refuse with a distinct exit code (8) and a message pointing at the parent repo's cwd. `task done` is refused on purpose — coders use `planar-agent complete --claim <token>`, the atomic terminal verb. `--scope <slug>` does NOT override the refusal; the rule is about *where the verb runs*, not which scope it targets.

For the canonical path scheme, branch scheme, lifecycle, the six parallelizability rules, and the conflict-resolution taxonomy see `agents/methodology.md` § Worktrees. For the recovery recipe when a coder dies mid-cycle see [`docs/workflows.md` §Recipe 23](workflows.md#recipe-23--recover-a-dead-coder-from-its-worktree).

**SQLite tables:** `agent_work_claims` (`worktree_path` column, migration 00015). **Primary entry points:** `planar-agent pull --worktree <path>`, `planar resume <task>`, `planar-watch claims`, `planar dashboard --agents`.

---

## Question

A question is an open inquiry attached to a scope, plan, or task. Agents record open questions rather than proceeding with uncertain assumptions. Questions gate on explicit answers before a task can be marked done.

### Status lifecycle

```
open → answered  (terminal)
     → wontfix   (terminal)
```

Legal transitions: `open → {answered, wontfix}` only. Both `answered` and `wontfix` are terminal — there is no `question reopen` verb. The `answered` status requires both `answer_body` and `answered_at` — the schema enforces this with a CHECK constraint.

### Sources

A question entity can be created through two equivalent paths: (1) interactively with `planar question add "…" --plan <id>` at any time during a session, or (2) automatically by `/pl-spec-draft` when it seeds a workbench — the planner scans every drafted artifact for `## Open questions` sections and registers each H3 child heading as a question entity. Both paths produce an identical `questions` row; the two sources are interchangeable and resolve through the same lifecycle. `/pl-spec-ingest` reconciles spec-body question items against existing entities on every run, surfacing drift warnings when the spec and the entity table diverge. See the "Reviewing open questions" recipe in `docs/workflows.md` for a full walkthrough.

**SQLite table:** `questions`. **Primary verbs:** `planar question add`, `planar question list`, `planar question answer`, `planar question wontfix`. **Related verb:** `planar workbench extract-questions` (reads spec bodies; used internally by the planning skills).

---

## Scenario

A scenario is a verification test case attached to a spec, plan, or task. Scenarios are hand-written by the operator, imported from `test-spec.md` by the ingestor, or auto-drafted by the ingestor for non-trivial roadmap tasks.

The ingestor imports scenario sections from `test-spec.md` into `test_scenarios` rows and links them to covered tasks with `entity_links(relationship='verifies')`. During apply, a newly added roadmap task with at least two bullet lines in its body is treated as non-trivial and receives an auto-drafted `Verify: <task title>` scenario.

### Status lifecycle

```
draft → ready → verified  ⇄  failing
    ↘     ↓        ↓             ↓
      retired (terminal, from any non-terminal state)
```

Legal transitions (enforced by `check_transition` in `src/engine/planning/transitions.cppm`):

| From | To |
|------|----|
| `draft` | `ready`, `retired` |
| `ready` | `verified`, `failing`, `retired` |
| `verified` | `failing`, `retired` |
| `failing` | `verified`, `retired` |
| `retired` | — terminal |

Note: `scenario verify --outcome pass` on a `draft` scenario auto-walks `draft → ready → verified` internally (two policy-checked hops), so the operator workflow `scenario add → scenario verify` works without an explicit `scenario ready` step. There is no `scenario ready` CLI verb.

**SQLite table:** `test_scenarios`. **Primary verbs:** `planar scenario add`, `planar scenario list`, `planar scenario verify`, `planar scenario show`, `planar scenario retire`.

---

## Decision

A decision is a recorded design choice. Decisions have a status lifecycle:

```
proposed → accepted
         → superseded  (terminal)
         → withdrawn   (terminal)

accepted → superseded  (terminal)
         → withdrawn   (terminal)
```

Legal transitions (enforced by `check_transition` in `src/engine/planning/transitions.cppm`):

| From | To |
|------|----|
| `proposed` | `accepted`, `superseded`, `withdrawn` |
| `accepted` | `superseded`, `withdrawn` |
| `superseded` | — terminal |
| `withdrawn` | — terminal |

Architecture decision records (ADRs) are artifacts of `kind=adr`, not decision rows — the `decisions` table is for in-flight design choices made during feature work. A decision row points to the session in which it was made and optionally to a plan.

**SQLite table:** `decisions`. **Primary verbs:** `planar decision add`, `planar decision list`, `planar decision accept`, `planar decision supersede`, `planar decision withdraw`.

---

## Artifact

An artifact is a long-form prose document attached to a plan. Artifacts are the canonical location for specs, ADRs, design notes, and generated outputs.

### Artifact kinds

| Kind | Meaning |
|------|---------|
| `tech_spec` | Technical specification |
| `product_spec` | Product intent and user stories |
| `adr` | Architecture decision record |
| `design_note` | Design exploration or spike output |
| `summary` | Session summary or retrospective |
| `readme` | README-style overview |
| `roadmap` | Flat milestone list (consumed by the ingestor) |
| `test_spec` | Test strategy and scenario coverage plan |
| `generated` | Machine-generated output (diffs, reports) |
| `other` | Catch-all |
| `research` | Academic-tone investigation note. Has its own template. |
| `getting_started` | Onboarding doc, imperative tone. |
| `changelog_entry` | Single changelog entry; aggregated into a published changelog by the regenerator. |
| `glossary_term` | Single term definition; aggregated into a glossary page. |

Artifacts are stored as Markdown in the `body` column and mirrored to the workbench filesystem as `.md` files under the plan's workbench directory.

### Status lifecycle

```
draft ⇄ active → superseded  (terminal)
                → retired     (terminal)
```

Legal transitions (enforced by `check_transition` in `src/engine/planning/transitions.cppm`):

| From | To |
|------|----|
| `draft` | `active` |
| `active` | `draft`, `superseded`, `retired` |
| `superseded` | — terminal |
| `retired` | — terminal |

**SQLite table:** `artifacts`. **Primary verbs:** `planar artifact add`, `planar artifact show`, `planar artifact list`, `planar artifact update`.

---

## Annotation

An annotation is a line-anchored review note attached to a file path (and optional line range), captured during a code review or agent pass. Annotations carry optional `commit_sha` and `text_hash` fields so the anchor can be verified against current workspace state via `annotate verify`.

### Status lifecycle (retention-tier model)

```
active → resolved  → archived  (sole final state)
       → dismissed → archived  (sole final state)
       → archived             (direct)
```

`archived` is the **single final retention state** (plan 692). `resolved` and `dismissed` are *outcome states*: they record how an annotation was disposed of, but they are not final — both may still progress to `archived` via the retention tier. `archived` has no outgoing edges.

Legal transitions (enforced by `check_transition` with `transition_kind::annotation`):

| From | To |
|------|----|
| `active` | `resolved`, `dismissed`, `archived` |
| `resolved` | `archived` (retention-tier progression) |
| `dismissed` | `archived` (retention-tier progression) |
| `archived` | — sole final state |

All other moves are illegal: `resolved → dismissed`, `dismissed → resolved`, `resolved → active`, `dismissed → active`, `archived → anything`. Identity (`from == to`) is a no-op.

`annotate sweep --since-days <n>` selects `resolved`/`dismissed` rows older than the cutoff and archives them (resolved→archived and dismissed→archived are both legal), making sweep the primary housekeeping path for outcome rows that have aged past their review window.

**SQLite tables:** `annotations`, `annotation_tags`, `annotation_source_identity`, `annotation_operation_receipts`. **Primary verbs:** `planar annotate add`, `planar annotate resolve|dismiss|archive`, `planar annotate bulk-resolve|bulk-dismiss|bulk-archive`, `planar annotate sweep`, `planar annotate verify`.

---

## Workbench

The workbench is the bidirectionally synced filesystem view of an anchor plan and all its entities. Each anchor plan gets a directory at `$PLANAR_WORKBENCH_ROOT/<assoc-slug>/p<id>-<slug>/` (default root: `~/.planar/workbench/`).

The workbench is the primary drafting surface for agents and users. The planner agent writes documents into it. The user reads and edits them. The ingestor reads them back to decompose into tasks. Agents working on tasks write their outputs as artifact files in the workbench.

Sync is always explicit:
- `planar workbench push <plan>` — DB → FS
- `planar workbench pull <plan>` — FS → DB
- `planar workbench sync <plan>` — bidirectional

When a feature is complete, `planar workbench archive <plan>` removes the FS tree (the DB retains everything). `planar workbench restore <plan>` recreates it.

### Terminal-status filter

Push, restore, and the new `gc` verb honor a status-based filter so the workbench filesystem mirrors active work rather than accumulating audit-trail files for terminal entities (cancelled tasks, abandoned plans, superseded decisions, etc.).

- **Default mode is `failures`.** Failure terminals (`tasks.cancelled`, `plans.abandoned`, `decisions.{superseded,withdrawn}`, `questions.wontfix`, `test_scenarios.retired`, `artifacts.{superseded,retired}`) are filtered out of the FS write set. Success terminals (`tasks.done`, `questions.answered`, `test_scenarios.verified`) stay visible as checkpoint artifacts.
- **`--filter-mode all`** extends the filter to success terminals. Useful in maintenance-mode repos where every historical entity clutters the view.
- **`workbench pull` does NOT filter.** Edits to terminal-backed FS files (e.g. updating a cancelled task's body to record WHY it was cancelled) are always ingested into the DB. Status never transitions on pull, so accepting body updates carries no integrity risk.
- **`workbench gc <plan>`** removes FS files whose backing entity is terminal. Defaults to apply but refuses with exit 1 when any to-be-removed file has FS-content drift from its DB-stored hash; `--yes` overrides. Flags: `--dry-run`, `--yes`, `--filter-mode`, `--all-scopes`, `--json`.
- **`workbench push --apply-cleanup`** is narrow-scope: only removes pre-existing FS files for entities this push enumerated and would have filtered. Plan-wide / workspace-wide cleanup is `workbench gc` / `gc --all-scopes`.

`push --apply-cleanup` and `push --filter-mode all` are mutually exclusive (the modes express opposite intents — clean up vs. include everything).

**SQLite table:** `workbench_sync_state` (tracks per-file sync state). **Primary verbs:** `planar workbench push`, `planar workbench pull`, `planar workbench sync`, `planar workbench status`, `planar workbench archive`, `planar workbench restore`, `planar workbench gc`, `planar workbench publish`. **Primary engine module:** `src/engine/workbench/terminal.cppm` (the exhaustive `switch`-backed status classifier — a future status added by a migration is a compile-time error here).

---

## Entity Link

An entity link is a typed cross-entity relationship. Any two entities of any kind can be linked. Links are stored in the `entity_links` table.

### Relationship types

| Type | Meaning |
|------|---------|
| `derives-from` | This entity was derived from the referenced entity |
| `depends-on` | This entity cannot proceed until the referenced entity is resolved |
| `addresses` | This entity addresses (resolves or mitigates) the referenced entity |
| `verifies` | This entity (typically a scenario) verifies the referenced entity |
| `cites` | This entity references the referenced entity for context |
| `supersedes` | This entity replaces the referenced entity |
| `touches` | This entity modifies or depends on the referenced entity (typically a task touching a repo) |

The `touches` relationship is particularly important for ext-sync: it records which repos a task touches, which is the input to the GitHub strategy-selection algorithm.

**SQLite table:** `entity_links`. **Primary verbs:** `planar links add`, `planar links list`, `planar links remove`.

---

## External Link

An external link binds a local entity to a ticket in an external system (Jira, GitHub Issues). External links are recorded in `external_links`.

The `link_role` column distinguishes the relationship kind:

| Role | Meaning |
|------|---------|
| `mirror` | This external item was created by Planar's `ext propagate`/`ext propagate-one` and tracks the local entity one-to-one |
| `reference` | This external item was created independently; the link is informational |

Each `external_links` row also carries a `config_json` blob used by the ext-sync engine to cache per-feature propagation state (selected GitHub strategy, etc.). This is what makes strategy selection sticky across re-propagation runs.

`planar link <entity> --to <system-slug>:<external-id>` creates a reference link manually. `planar-ext ext propagate-one` creates a mirror link automatically for one entity; the whole-tree `planar-ext ext propagate <plan>` does the same for every entity in the feature tree (see `docs/cli-reference.md`).

**SQLite table:** `external_links`, `external_systems`, `sync_events`. **Primary verbs:** `planar link`, `planar unlink`, `planar-ext ext propagate`, `planar-ext ext create`, `planar-ext sync pull`, `planar-ext sync push`.

---

## Session

A session is a durable agent-execution record. One session = one vendor's episode of work, scoped to a task and project. Sessions are the backbone of agent continuity — they make it possible to resume work after compaction, a vendor switch, or a process restart.

Every command that writes to the database appends a `session_entries` row to the current active session for the affected task. If no session exists, one is auto-created using the `PLANAR_VENDOR` environment variable.

A **handoff** record is written at the end of a session via `planar handoff`. It includes a `context_snapshots` row (the resume packet) and the metadata needed for the receiving agent to pick up where the previous one left off.

`planar resume <task-id>` assembles the resume packet — task state, recent session entries, open questions, decisions made, and next action — into a structured prompt the new agent session reads at startup.

**SQLite tables:** `sessions`, `session_entries`, `context_snapshots`, `handoffs`. **Primary verbs:** `planar capture session`, `planar capture end`, `planar capture note`, `planar handoff`, `planar resume`.

---

## Templates layer

Planar's template plane is the **external-system propagation** templates: JSON documents that render a Planar entity into the payload a target system expects (GitHub Issues, Jira). They resolve through a three-level fallback chain — the operator's chosen set in `~/.planar/templates/<set>/<system>/<kind>.json`, the on-disk baseline in `~/.planar/templates/default/<system>/<kind>.json`, then the set embedded in the binary — and are surfaced by `planar templates {list, show, render, validate, init, path}`.

**SQLite tables:** none — templates are filesystem assets plus an embedded fallback set. **Primary entry points:** `load_template()` in `src/engine/config/templates.cpp` (resolve + load), `src/engine/templates/render.cpp` (render), `src/engine/templates/validate.cpp` (lint).

<!-- surface-lint-ignore surface-path-missing: names the deleted-with-zig/ reader path this removed section never used, for history -->
> **Removed (2026-08-07).** An earlier revision of this section described a second, unrelated templates layer: per-entity-kind Markdown files under `templates/entity/` that seeded new entities created through the editor-first `add` verbs, with a `{{.Title}}` placeholder language and an install-time validator. **That layer never existed in this binary.** The files and this documentation both arrived in the Go→Zig bootstrap (`bfa3abc`); the reader was never ported, and `src/cmd/planar/editflow.zig` has never contained the word "template". `templates/entity/` has been deleted rather than left installed and inert. See planar task 5918.

## Model routing

Which model an agent role spawns is **config-driven and unified** (plan 540, extended plan 899). The Planar config (`~/.planar/config.toml`, embedded defaults in `src/engine/config/defaults.toml`) carries:

- `[models.<vendor>]` — per-vendor **tier maps**: the canonical `small` / `medium` / `large` tiers → a **scalar-or-list** candidate value (e.g. `[models.claude] medium = "claude-sonnet-5-5"`). A scalar is one candidate; an ordered list (`large = ["gpt-5.6-sol", "gpt-5.5"]`) names several — `list[0]` is always the **tier default**, the model any caller gets when it resolves a bare `(vendor, tier)`/`(vendor, role)` pair with no work type in hand. Every existing scalar config resolves unchanged.
- `[routing.<vendor>.<tier>]` — a **work-type → candidate** map (plan 899 D4/D9/D10/D11): each key is one of `schema | engine | architectural | cli | feature | mechanical` and its value is a candidate **model id string** naming one member of that tier's candidate list (never a list index — index values silently re-route when the list is reordered). Ships as an embedded default (only `mechanical` is routed by default, to the tier default) and is fully operator-overridable. `planar config validate` rejects a routing entry naming a model id absent from the matching tier's candidate list.
- `[roles]` — **role → tier** (e.g. `coder = "medium"`, `reviewer = "large"` — the "sonnet coder, opus reviewer" default).
- `[role_vendors]` — optional **role → vendor** override; unset roles use `[defaults].vendor`.

A single **shared resolver** (`src/engine/config/effective.cppm` plus `src/engine/models/`) composes these into a concrete `(vendor, model)`. The tier-only path (`resolveTier`, `resolveRole`, `resolveRoleAuto`) is unchanged and always returns the tier default (`list[0]`). A parallel `resolve(role, work_type)` entry point (`resolveTierWorkType` / `resolveRoleWorkType` / `resolveRoleAutoWorkType`) additionally consults the routing map: a hit returns the named candidate, a miss (or a stale routing target absent from the candidate list) falls back to the tier default. Every consumer resolves through one of these — there are no parallel per-tool model tables:

- **`planar models`** — `resolve --role <role> [--task <id>|--plan <id>]` (the tier a role gets from its packet, or the static fallback with its reason), `registry list|add|update|remove|bind|unbind|observe|eligibility|verify-identity|export` (the opaque candidate registry), `experiments` and `outcomes` (routing evidence), and `evals` (below). The plan-540 discovery family — `list`, `routing`, `candidates`, `refresh`, `apply` — was removed with the curated catalog; `planar config show --effective` is where the resolved `models.*` / `routing.*` / `roles.*` keys are inspected.
- **The Tier Table** (hand-maintained in `agents/models.md`; Planar does not generate it) + rendered skill/agent `model:` fields — rendered skill/agent `model:` fields come from scriptorium's render step, which always renders `list[0]` for a candidate-list tier (static surfaces show the tier default; per-task routing is runtime-only).
- **External workflow harnesses** — shell `planar models resolve --role <role> --json` per role (no engine handle), falling back to compiled defaults when `planar` is unreachable.
- **Orchestrator dispatch (Phase 3)** — the dispatch preview's routed-model column classifies each task's work type and calls `resolve(role, work_type)` to show the routed candidate alongside the tier column; the operator may override either before confirming. The confirmed `{tier, candidate, work_type}` triple persists per task in the dispatch session entry's `model_choice` map (a convention extension, no schema change — see `agents/orchestrator.md` dispatch step 8a and `skills/src/pl-orchestrator.md` § Dispatch preview and model tiers).
- **`planar models evals`** — read-only routing evaluation. With `--vendor` and the cohort flags it ranks candidates in one exact cohort by the 95% Wilson lower bound over declared-experiment evidence; without them it falls back to the legacy scorecard mined from dispatch notes (`dispatch_shape:` / `model_choice:`), terminal claim status, and test-coder action outcomes. Quality-gate pass/fail is not persisted today, so the legacy path reports that signal as unsourced rather than guessing. It writes nothing; applying a recommendation is a separate operator-gated edit to `agents/models.md`.

Planar does not discover or validate provider model lists: candidate ids are opaque strings recorded as agents report them, and spawn verification belongs to the host adapter. See `docs/cli-reference.md` § Domain `config` (Model routing) and § Domain `models`, and the `pl-models-config` skill.

## Color output

**There is none.** Measured 2026-09-11 (task 6675): `planar plan list` emits
**zero** ANSI escape bytes, `planar schema` declares no `--color` / `--no-color`
flag on any command, and all three spellings fail at parse time with exit 2:

```
$ planar plan list --color   # cli-lint-ignore: the flag's ABSENCE is the point
error: plan list: The following argument was not expected: --color
(exit 2)
```

`NO_COLOR` is not read, because there is nothing to suppress.

Editions of this page before 2026-09-11 described a six-family ANSI palette
keyed by `(entity_kind, status)`, a five-level `--color=auto|always|never`
precedence table, a structural `--json` bypass, and a "drift gate" unit test
asserting a palette entry for every status. **None of that exists.** The
entry-point it named — `src/cmd/planar/output.zig` — went with the Zig tree at
the M10 cutover, and no C++ equivalent was written.

The design is preserved below as a *proposal*, not as documentation, because it
is a reasonable design and re-deriving it would be waste. Anyone implementing
it should treat the table as a starting point and re-check it against the
current status vocabulary, which has changed since it was written.

<details>
<summary>Unimplemented palette proposal</summary>

Scope: `planar tree`, the per-entity `list` verbs, and the child-plan summary
in `plan show` would colorize the status column only — titles, IDs, dates and
relationship arrows stay plain. The map is keyed by `(entity_kind, status)`, so
the same status text on different entities can take different colors.

| Family | Color | Statuses |
|---|---|---|
| Live in-flight | bold cyan | `plan.active`, `task.doing`, `plan_step.in-progress` |
| Ready / open | default | `task.todo`, `question.open`, `decision.proposed`, `plan.draft`, `artifact.draft`, `scenario.draft`, `annotation.active`, `plan_step.pending` |
| Done / accepted | green | `plan.done`, `task.done`, `question.answered`, `decision.accepted`, `scenario.verified`, `annotation.resolved`, `scenario_outcome.pass`, `artifact.active`, `plan_step.done` |
| Caution | yellow | `plan.paused`, `task.blocked`, `scenario.failing`, `scenario.ready` |
| Hard fail | red | `scenario_outcome.error`, `scenario_outcome.fail`, `question.wontfix` |
| Terminal-dim | gray | `plan.abandoned`, `task.cancelled`, `annotation.dismissed`, `annotation.archived`, `artifact.superseded`, `artifact.retired`, `decision.superseded`, `decision.withdrawn`, `scenario.retired`, `scenario_outcome.skipped`, `plan_step.skipped` |

Proposed mode precedence, highest first: `NO_COLOR` (any non-empty value) →
`--no-color` / `--color=never` → `--color=always` → `--color=auto` with a TTY
stdout → otherwise off.

Two properties worth keeping if this is ever built: the `--json` path should
bypass the color helper **structurally** rather than by consulting a toggle, so
piped JSON is byte-identical regardless of mode; and a drift test should walk
every domain's status constant and assert a palette entry, so a migration that
adds a status cannot silently ship uncolored.

</details>

**SQLite tables:** none. **Primary entry points:** none — nothing implements this.

## Local sandbox

A user-local authoring surface for personal skills and agents under `~/.planar/local/`. The sandbox is one-way: the operator drops a single skill (as a `<name>/SKILL.md` directory) or agent (as a flat `<name>.md` file), then `planar local link` fans installs out to each vendor's surface (`~/.claude/commands/`, `~/.codex/skills/`, `~/.copilot/skills/`). Edits to the source SKILL.md are instantly live in every vendor — see *Per-vendor install layout* below.

**Directory layout.**

```
~/.planar/local/
├── skills/
│   ├── fixup-protos/
│   │   ├── SKILL.md              # the source-of-truth file
│   │   └── helper.sh             # optional auxiliary files travel with the skill
│   └── .link-manifest.json       # written by `planar local link`
└── agents/
    └── pedantic-reviewer.md      # agents stay flat — no vendor loader for agents
```

Skills are dir-shape because the dir-symlink installs into Codex / Copilot need a real directory to symlink. Auxiliary files inside the skill directory (helper scripts, data files, icons) travel with the skill — they appear inside the vendor install directory via the same directory symlink. Agents stay flat: they install into `~/.planar/agents/` which has no vendor loader, so the directory wrapping buys nothing.

A file's parent directory tree is authoritative for its kind: a `<name>/SKILL.md` under `skills/` is a skill regardless of what its frontmatter says. The `kind:` field in frontmatter (when present) must match the directory, or the file is rejected as malformed.

**Migrating from the legacy flat layout.** A pre-reshape sandbox stored each skill as a flat `~/.planar/local/skills/<name>.md`. Run `planar local migrate` to convert these to the dir-shape `<name>/SKILL.md` layout. `planar local link` flags any remaining flat skill files with a warning pointing at the migrate verb.

**Frontmatter schema.** The frontmatter mirrors the unified canonical skill
convention authored under the repo's `skills/src/`. Vendor projections are
generated at install time and are not an authoring surface. Sandbox-specific
keys are `shadow:` and `vendors:`; vendors ignore them.

```yaml
---
description: "Rebuild and re-import protobuf bindings in the current repo"
argument-hint: "<optional usage hint>"
tier: medium                    # small | medium | large; maps to model
model: claude-opus-5-5          # optional explicit model override
shadow: false                   # true → link without the local- prefix (shadows canonical)
vendors:                        # subset of {claude, codex, copilot};
  - claude                      #   defaults to all three if omitted
  - codex
---

# Body content
The agent should...
```

**Link semantics.** Linked filenames default to `local-<source-name>` so sandbox installs are visibly user-authored and never collide with canonical skills. With `shadow: true`, the prefix is dropped and the install replaces the same-named canonical install (with a warning at link time naming what's being shadowed). The link package writes a `.link-manifest.json` per `<kind>/` directory recording the per-vendor targets so unlinking is fast and self-documenting.

**Per-vendor install layout.** Each vendor's discovery loader is shape-specific, so the link layer installs three different shapes:

| Vendor  | Install entry                                       | Shape | Edit-and-live |
|---------|-----------------------------------------------------|-------|---------------|
| claude  | `~/.claude/commands/local-<name>.md`                | file symlink → `<src>/<name>/SKILL.md` | yes |
| codex   | `~/.codex/skills/local-<name>`                      | **directory symlink** → `<src>/<name>/` | yes |
| copilot | `~/.copilot/skills/local-<name>`                    | **directory symlink** → `<src>/<name>/` | yes |
| agents  | `~/.planar/agents/local-<name>.md`                  | file symlink → `<src>/<name>.md` | yes |

The dir-symlink shape for Codex and Copilot is load-bearing. Empirically their discovery loaders stat each entry in the skills directory and read the SKILL.md inside — a symlinked SKILL.md *inside* a real directory is treated as missing, while a symlinked *directory* pointing at a real source dir is followed correctly. The sandbox keeps skills as `<name>/SKILL.md` source dirs so the dir-symlink resolves to a real file.

`planar local unlink` removes the symlink itself; the underlying source directory at `~/.planar/local/skills/<name>/` is left untouched. Use `unlink --purge` to also delete the source.

**Promotion is manual.** No `planar local promote` shortcut. A skill earning a place in the canonical repo means going through the normal git contribution flow: copy the file into `skills/src/`, run `make install-full` (which shells the scriptorium binary — `scriptorium render --config scriptorium.yaml` — at install time), commit, push, and let `scriptorium check` (run against an out-of-tree staging dir) plus any remaining relevant validators gate it. The absence of a shortcut is deliberate — canonical and sandbox have different bars.

**SQLite tables:** none — the sandbox is filesystem state. **Primary entry points** (under `src/engine/local/`): `walk_sandbox` and `migrate` in `manifest.cppm`, `link`, `unlink`, `list` and `reconcile` in `link.cppm`, `import_sources` in `importer.cppm`. CLI surface: `planar local {list, link, unlink, import, migrate}`; the `pl-local repair` workflow uses `planar local link --reconcile`, not a separate repair verb.

## Test spec

The fourth planning document, alongside product-spec, tech-spec, and roadmap. The planner emits a `test-spec.md` for every new feature; the ingestor decomposes its `## Scenarios` section into `test_scenarios` rows with `verifies` edges to the tasks each scenario covers.

**Authoring shape.** Each scenario is a **flat `### Scenario: <title>` H3** — one H3 per scenario. The four return-path buckets (happy / empty-null / error / edge) are a **coverage-reasoning lens**, not document structure: name the bucket in the scenario title, e.g. `### Scenario: Happy path — export returns CSV rows` or `### Scenario: Error return — export fails on missing header`. Do NOT use `### <bucket>` H3 group headers containing `#### Scenario:` H4 children — that layout is silently ambiguous and was the root cause of #87.

The four lenses, each asking a different question:

- **Happy path** — valid input, meaningful output. The function does the thing.
- **Empty / null return** — valid input, legitimately empty output (no rows, nil pointer, "not found"). A correctness path, not an error path. Easy to skip; often hides the subtlest bugs (conflating "no results" with "error").
- **Error return** — operation cannot proceed. The function returns a non-nil error.
- **Edge case** — boundary conditions (zero / one / max inputs, off-by-one, concurrent access).

Each `### Scenario: …` H3 carries leading `**Verifies:** task:N, task:M` / `**Kind:** unit|integration` / `**Acceptance:** <observable result>` lines. The ingestor extracts these into structured fields on the `test_scenarios` row.

**Accepted lenience.** `planar spec ingest` (preview and apply) also tolerates two non-canonical forms so older specs do not require a rewrite: (a) `#### Scenario:` H4 items nested under `### <bucket>` H3 group headers, and (b) bulleted `## Decisions` entries (`- **Title.** body`). Both are parsed and imported correctly, but the canonical/preferred forms — flat `### Scenario:` H3 scenarios and `### <title>` H3 decisions — should be used for new authoring.

**Preview warning.** `planar spec ingest <plan>` (preview mode, no `--apply`) now emits a stderr warning when a non-empty `## Decisions`, `## Open Questions`, or `## Scenarios` section produces zero extracted entities. Run the preview before `--apply` to catch parsing mismatches early:

```
planar spec ingest <plan>
```

**Coverage-gap checklist.** Below the scenarios, the operator confirms per-function compliance with each bucket via Markdown checkboxes. Gaps marked N/A require a one-line justification so the reviewer can confirm the absence is deliberate.

**Cross-references.** The test-spec carries `verifies: [artifact:<product-spec-id>]` in its frontmatter so the cross-reference machinery tracks which user stories the test plan covers. Scenarios cite tasks via `**Verifies:** task:<id>` *or* `**Verifies:** task:<slug>`. The slug form (plan 286) is the canonical citation chain: scenarios drafted before tasks exist still resolve at apply time, because the ingestor looks up `tasks.slug` against the `[slug: …]` annotations on the roadmap bullets. Unresolvable slugs are a hard error at apply — the operator either adds the missing `[slug:]` to the roadmap or removes the citation.

**Coverage gate.** Before ingestion, `planar spec ingest <plan> --strict --json`
is the authoritative workbench-draft oracle. Preview is the default because
`--apply` is absent. Its `coverage` object reports task/slug totals,
`uncovered_task_slugs`, and `orphan_scenarios` (no parseable `**Verifies:**`
line); the top-level `slug_collisions` array reports slugs already held by live
tasks. A non-zero exit or any uncovered, orphan, or collision finding blocks
ingestion. `planar test-spec status <plan> --json` instead queries live
`tasks`, `test_scenarios`, and `entity_links`; it becomes authoritative only
after apply. Before apply, its legitimate zero totals do not prove draft
coverage. After apply it provides the per-milestone four-bucket breakdown
(happy / empty / error / edge) used by test-coder cycles and reviewers.

**Planning loop integration.** The planner authors the test-spec in Phase 4 of its authoring pipeline (see [`agents/planner.md` §Authoring phases](../agents/planner.md#authoring-phases)). Phase 4 is purely adversarial: what could go wrong, what scenarios prove this works, what scenarios prove it doesn't. The planner explicitly does NOT propose implementations of the tests — that's the [test-coder](#test-coder)'s job (see below).

**Orchestrator integration.** When dispatched tasks have `[slug:]` annotations on their roadmap bullets and the test-spec cites those slugs via `task:<slug>`, the orchestrator's Phase 3.5 dispatches the test-coder agent. The gating oracle is `planar test-spec status <plan> --json` — the orchestrator does not re-implement coverage calculation. The reviewer then runs `planar test-spec status` against the post-diff DB; any slug claimed by the brief that still appears in the uncovered set is a `request-changes` finding citing the verb output verbatim.

**SQLite tables:** none beyond the existing `test_scenarios` and `entity_links`. **Primary entry points:** `parse_test_spec` in `src/engine/ingest/parse.cppm` (parses the body), `workbench.LoadCrossRefs` (reads the cross-reference edges), the `test_spec` artifact kind in `artifacts.kind`.

## Test-coder

The fourth agent role, dispatched between the coder and the reviewer in Phase
3.5. The test-coder, like the orchestrator/coder/reviewer/janitor roles
around it, lives in this repo's `agents/`; this section describes the contract
planar's `planar test-spec status` gating verb and scenario/entity_links
schema exist to support. Reads the test-spec and the coder's diff; produces a test-only
diff that closes uncovered slugs. Never modifies a failing test to make it
pass — surfaces failures with a classification (`test-wrong-author-error` /
`code-wrong-bug-surfaced` / `ambiguous-operator-decide`).

**Cognitive-split rationale.** A coder writing tests for their own feature has the wrong incentive: make-the-green-test-pass shapes both the feature and the test. A test-coder reading the test-spec and the coder's already-committed feature has no incentive to make tests easy to pass — only to verify the cited scenarios. The two roles enforce different mental models. The load-bearing clause is **tests-may-be-elevating-bugs**: when a new test fails on first run, the test-coder does NOT modify the test, it classifies the failure and reports it. A red test is signal, not noise.

**Position in the loop.** Phase 3.5 fires *only* when (a) the cycle's dispatched tasks carry `[slug: …]` annotations AND (b) `planar test-spec status --json` reports `uncovered_task_slugs` intersecting the cycle's slugs. Slug-less tasks are out of scope by construction. The decision taxonomy is:

- `expanded` — test diff covers cited scenarios; all new tests pass. Orchestrator stages the diff alongside the coder's; reviewer sees the union.
- `no-expansion-needed` — cited scenarios already verified by the coder's diff or pre-existing tests. Reviewer sees the coder's diff alone.
- `failure-surfaced` — at least one new test fails on first run. Orchestrator escalates to the operator with the test-coder's classification; reviewer NOT dispatched until the operator resolves.
- `abort` — cannot satisfy the brief.

**Iteration cap.** The test-coder cycle has its own cap (default 2; the work shape is "expand or don't" rather than "iterate to convergence"). Independent of the coder/reviewer's 5-iteration cap.

**Manual invocation.** Operators can invoke `/pl-test-coder <task-id>` directly to backfill coverage on an already-committed change set, or `/pl-test-coder <plan-id> --plan` to run against every cited scenario in a plan. Useful after authoring a new test-spec for an older feature.

**SQLite tables:** none. **Primary entry points:** `agents/test-coder.md` and `skills/src/pl-test-coder.md`, `planar test-spec status` (gating verb), `planar spec ingest --strict` (ingest-time gate). Vendor projections are generated at install time.

## Usage Introspection Privacy Model

Planar's usage-introspection loop (capture → report → introspect) applies a two-tier privacy model. The two tiers provide different guarantees and must not be conflated.

### Tier 1: Structurally-redacted diagnostic bundle

`planar report [--json]` is **privacy-safe by query construction**. The aggregate queries in `src/engine/introspect/introspect.cpp` select only counts, error categories, verb paths, statuses, and timestamps from the observability tables. They never select `title`, `body`, `summary`, scope slugs, file paths, or any column that could carry operator-authored or PII-adjacent text. This guarantee is testable with sentinel fixtures and holds with no human in the loop.

The `cli_invocations` table enforces the same guarantee at the write site: the capture hook serializes flag **names** and positional **arity** only (`args_shape`). There is no code path that writes an argument value into the table. A future query bug cannot leak an argument value from this table because argument values are never there to leak.

### Tier 2: Preview-gated finding text

Findings filed by the introspector (`planar question add` / `planar task add` on the feedback plan) may legitimately reference verb paths and error categories in their body. Their only guarantee is the **mandatory preview gate** in `pl-report-issue` — the operator personally reviews every byte of issue body text before it posts to GitHub. Skills and docs must present the bundle as machine-safe and the finding embed as operator-reviewed, never the reverse.

### Transcript mining: cross-vendor and ephemeral by design

Transcript adapters recognize supported Claude, Codex, and Copilot local
session schemas; the opt-in CLI log remains the authoritative source for
invocations it contains. Each adapter immediately normalizes records to
vendor, verb path, category, count, and time range. **Transcript text
(operator messages, assistant responses, tool output prose), argument values,
entity titles, scope slugs, and raw transcript paths are never persisted to a
Planar entity, SQLite table, snapshot, or failure output.** Unknown schema
versions and malformed records are skipped with counted warnings. A missing or
disabled source is reported in `signal_coverage`, not conflated with an
observed zero.

The default `pl-introspect` run is a read-only preview. Only a separately
confirmed apply phase creates or reuses the feedback plan and files approved,
deduplicated findings. Cancellation before the gate writes nothing; mixed
apply results retain successful independent findings and return exact recovery
for the failures.

### Opt-in capture

`[introspection].cli_log = false` by default. No `cli_invocations` rows are written until the operator sets `cli_log = true` in `~/.planar/config.toml`. The report verb distinguishes "logging disabled" from "no activity in the window" — the operator is never shown fabricated zeros. The always-on observability tables (`agent_actions`, `sync_events`, `agent_work_claims`, `handoffs`) render normally regardless of the `cli_log` setting.

**SQLite tables:** `cli_invocations` (opt-in; args shape only), `agent_actions`, `sync_events`, `agent_work_claims`, `handoffs` (always-on, read by `report`). **Primary entry points:** `planar report [--json]` (diagnostic bundle), `skills/src/pl-introspect.md` (introspection skill), `agents/introspector.md` (agent role spec).

## Feedback triage

Feedback findings remain ordinary task or question rows on a feedback plan;
their review state is kept separately in `feedback_triage`. One row targets
exactly one task or question and records severity
(`info|low|medium|high|critical`), disposition, reproduction status, optional
same-plan duplicate target, and redacted evidence. External issue identity
continues to live in `external_links`, so local triage never predicts or
duplicates publication state.

`pl-feedback-triage` is preview-first. Its `feedback-triager` specialist may
recommend `duplicate`, `accepted`, `needs-reproduction`,
`retained-question`, `dismissed`, or—only after verified publication—
`reported-external`. The caller shows the proposed entity, relationship, and
`planar feedback triage set` changes and waits for explicit row-level approval.
An initial `--apply` request is not confirmation. Optional external reporting
then enters `pl-report-issue`, which shows the complete issue body and requires
a second approval. Declining that gate posts nothing and leaves completed
local triage intact.

Multi-finding application is not atomic across independent findings. Verified
completed rows remain applied if another target fails; the operator receives
`outcome=partial`, action counts, and an idempotent inspection or retry command
for each failure.

**SQLite tables:** `feedback_triage`, plus existing finding entities and
`external_links`. **Primary entry points:** `planar feedback triage
list|show|set`, [`skills/src/pl-feedback-triage.md`](../skills/src/pl-feedback-triage.md),
and [`agents/feedback-triager.md`](../agents/feedback-triager.md).

---

## Cross-references

Every artifact, task, scenario, decision, and question can carry outgoing edges of three relationship kinds: **`verifies`** (this entity verifies another — used by `test_scenarios` to point at tasks), **`cites`** (this entity references another for context but does not depend on it), and **`derives-from`** (this entity derives from another — a tech-spec derives from a product-spec). All three are stored as rows in the `entity_links` table.

**Frontmatter surface.** The workbench `front_matter` struct (`src/engine/workbench/parse.cppm`) exposes three optional list fields:

```yaml
---
entity_kind: artifact
entity_id: 42
artifact_kind: test_spec
verifies:
  - artifact:131   # the product-spec
cites:
  - plan:277       # a related plan
derives-from:
  - artifact:132   # the tech-spec
---
```

Each entry is a `"<kind>:<id>"` reference parsed into an `entity_ref` struct. Custom YAML marshalling (`UnmarshalYAML` / `MarshalYAML`) round-trips the string form. Malformed entries (empty kind, non-integer id, etc.) error at parse time so drift surfaces immediately.

**Push and pull.** On `workbench push`, the renderer reads the entity's `entity_links` rows and populates the three frontmatter lists. On `workbench pull`, the parser reads the frontmatter lists and reconciles them against `entity_links` (additive in v1; orphan-edge removal is a follow-up). Same pattern as the `touches:` field used by cross-repo tasks.

**Authoring discipline.** The planner emits frontmatter cross-references when relationships are obvious (test-spec verifies product-spec; tech-spec derives from product-spec). Operators can hand-edit the lists at any time via `<entity> edit <id>`. The new entries become `entity_links` rows on the next pull.

**Why frontmatter, not a body section.** A first design appended a `## Cross-references` Markdown section to artifact bodies. That broke the ingestor — the roadmap parser iterates every `## …` H2 as a milestone, and the injected section shifted milestone counts. Frontmatter avoids the collision. The `workbench.RenderCrossRefsSection` helper exists for callers (e.g. `planar tree` or `planar <entity> view`) that want a human-readable rendering, but the body itself stays clean.

**SQLite tables:** `entity_links` (existing; widened CHECK accepts `verifies` / `cites` / `derives-from` since plan 4). **Primary entry points:** `workbench.LoadCrossRefs(db, kind, id)`, `workbench.RenderCrossRefsSection`, `FrontMatter.Verifies` / `Cites` / `DerivesFrom`.

---

The remaining sections describe Planar's executables and supporting components: the binary set, the host build and test queue, the deterministic workflow engine, an unrelated external workflow harness, and the unimplemented interactive cockpit.

## Binaries

Planar ships as five executables. Four are **planning-state executables**, each with a disjoint capability boundary over the shared SQLite DB enforced **by the verb set the binary registers** (not by runtime ACLs). The boundary is a compile-time and install-time property: the binary on PATH literally has no verb for the work it is not allowed to do. This makes vendor-hook blast radius bounded — a hook configured with only `planar-agent` on its PATH cannot mutate planning state regardless of how it is invoked.

The fifth, `planar-execute`, is **not** a planning-state executable: it is the deterministic, spawn-free Lua workflow engine (plan 633) and holds no DB handle at all. A caller invokes `planar-execute run <wf.lua> --phase <name>` to run a deterministic workflow over an allowlisted host surface (`cli`/`git`/`fs`/`flow`/`ctx`) and collect its JSON result; it reaches Planar state only by shelling the planning-state binaries. It exposes no model-spawning host function, so it is a workflow *runner*, not a harness. See [the workflow-engine section](#deterministic-workflow-engine) below; do not conflate it with an external full-harness project described under [External workflow harness control plane](#external-workflow-harness-control-plane).

| Binary | Audience | Writes to |
|---|---|---|
| `planar` | Operator (human + scripts) | Planning entities (`plans`, `tasks.status` via manual transitions, `decisions`, `questions`, `scenarios`, `artifacts`, `annotations`, …) — everything **except** `agent_work_claims`. It does not write `agent_actions` either, save for one best-effort exception: the entity-create provenance hook (plan 467 D2/D3) appends a `created <entity>` action when `decision`/`question`/`artifact add` runs under an active agent claim; with no active claim it is a silent no-op. Also registers the `explore` leaf, which prints help (there is no cockpit — see [§ Interactive cockpit](#interactive-cockpit)). |
| `planar-agent` | Agent (vendor hook, orchestrator dispatch) + operator recovery | `agent_actions`, `agent_work_claims`, `tasks.status` (the last only as part of atomic coordinated operations: `pull`, `complete`, `fail`, `release`, `block`), `workflow_runs` (via `run start`/`end`/`heartbeat`), `context_records` (via `context add`/`capsule`/`resolve`), and the `routing_dispatch_previews` / `routing_dispatch_snapshots` authorization tables (via `dispatch preview`/`confirm`). It also writes `queue_entries` and `queue_history` in `planar.db`, the host-wide build and test queue (via `queue run` and `queue cancel`). **Never** to plan / decision / question / scenario / artifact / annotation. |
| `planar-watch` | Operator (live view) + scripts (`--json`) | **Nothing.** Opens SQLite via `file:?mode=ro` so the driver itself rejects every write SQL string. |
| `planar-ext` | Operator + ext-sync agent (operational plane) | Exactly `external_links`, `external_systems`, `sync_events`. Planning tables (`plans`, `tasks`, `questions`, `artifacts`, …) are opened read-only, and the allowlist is enforced at the SQLite layer by a `sqlite3_set_authorizer` callback that fires on the parsed table name, not by convention (decisions 995–1001). Owns both operational adapters, Jira and GitHub Issues. |

The capability boundary is each binary's verb set, not a runtime ACL, and three of the five — `planar-agent`, `planar-watch` and `planar-ext` — have capability-boundary tests (`src/cmd/<binary>/capability.t.cpp`) that pin it. `planar` is the only supported access layer for workflows: skills and agents compose its verbs rather than writing to the database directly. `planar-watch` is where agent observability lives: action topology, live claims and feeds.

**Capability invariant — `planar-agent`:** a process invoked as `planar-agent` has no verbs that mutate any planning entity. The verb set is exactly `pull`, `peek`, `claim`, `claim-associate`, `heartbeat`, `complete`, `fail`, `release`, `block`, `action start`/`action end`, `run start`/`end`/`heartbeat`, `context add`/`capsule`/`list`/`resolve`, `dispatch preview`/`confirm`, `ingest`, `reconcile`, `abort`, `queue run`, `queue cancel`, `queue status`, `queue rule`, `version`, `schema`.

**Two capabilities `planar-agent` gained with the queue (plan 1080).** `planar-agent queue run` with a command after the argument terminator *executes a command its caller names*: it waits in the host-wide queue for its turn, runs the command in the caller's working directory with the caller's environment, and exits with the command's own status (so a build or test started by one agent does not run on top of another's). And `planar-agent` *owns the queue's tables*: `queue_entries`, `queue_history` and the `queue_schema` compatibility marker, all in `planar.db`. The queue verbs open `planar.db` themselves and never create or migrate it. They refuse a `planar.db` that is behind their binary, but keep working against one that is ahead while the marker admits them, so agents keep queueing builds while a newer build migrates the file. Nothing in the queue writes a planning entity or an existing agent table, and no transaction spans a queue table and a planning table. The queue is not a security boundary: it is a coordination convenience, and the verb set stays the capability boundary.

**Capability invariant — `planar-watch`:** the binary's verb set contains zero write verbs (`feed`, `ps`, `claims`, `actions`, `plans`, `log`, `tree`, `run`, `sync-events`, `queue`, `queue history`, `version`, `completion`, `schema` only). Enforced two ways: (1) the verb set; (2) the read-only DB handle. It also links the host queue engine but sends no signal to any process: a source scan and a link-level check of the built binary fail if a handler reaches the queue's process-group signaller (its liveness check is a signal-0 existence probe). `planar-watch` is the scriptable, read-only NDJSON streaming viewer; it is not, and was never, the cockpit.

**Capability invariant — `planar-ext`:** the verb set is `ext register jira|github`, `ext list`, `ext test`, `ext create`, `ext propagate`, `ext propagate-one`, `sync pull`, `sync push`, `sync status`, `sync resolve`, `version`, `schema`. A write to any table outside the three-table allowlist is refused by the authorizer before it executes; `sync pull` emits `remote_title`/`remote_status` proposals rather than writing planning entities (decision 996).


The ritual every code-writing agent dispatch follows is `planar-agent pull → heartbeat → complete|fail|release|block` (atomic across all three tables). The terminal verbs (`complete` / `fail` / `release` / `block`) flip the claim and the task status in a single transaction, which is why the claim ritual must not be split across two commands. See `agents/methodology.md` § Coordination claims and the tech spec § "Agent methodology contract" for the full sequence.

`planar-execute` is deliberately **outside** this ritual: it is a workflow engine the caller invokes, not an agent-table writer, and holds no DB handle. When a workflow needs to participate in a claim, it does so by shelling `planar-agent` verbs through the `cli` host function — exactly as any other caller would — never by holding a claim itself.

**Ordering contract — `planar-ext` does not migrate the database; `planar` must run first.** `planar-ext` (decisions 995–1001) is read-only on planning tables and read-write on exactly `external_links`/`external_systems`/`sync_events`, enforced by a `sqlite3_set_authorizer` allowlist — it is deliberately not the migration owner, so — like `planar-agent` and `planar-watch`, and unlike `planar` — it does **not** auto-apply pending migrations on open. Pointed at a database with no `schema_migrations` table (or one behind the binary's minimum schema version), it refuses with `SchemaVersionBehind` rather than migrating. Any operator or agent workflow that talks to `planar-ext` — including test harnesses that allocate a fresh scratch DB per run — must invoke `planar` (any verb; `plan list` and `init` both trigger the auto-migration) against that same `PLANAR_DB` at least once before the first `planar-ext` call.

---

## Host build and test queue

Several agents on one machine, often in different projects, each start builds and test suites. Run side by side they slow each other down and fail in ways that look like real defects. The **host queue** serializes them: one queue per user per host, shared by every project, in which a command waits for its turn and then runs. `planar-agent queue run -- <command>` submits it.

- **No daemon, no lock.** The process that submits a command is the process that runs it, in the caller's directory with the caller's environment, and it exits with the command's own status. The queue is ordered by arrival and holds no lock a dead process could leave behind: an entry is live while its submitter's process exists and keeps refreshing it, and any submitter's poll reaps an entry that is not live. `[queue] slots` (default 1) sets how many commands run at once.
- **Entries and outcomes.** An entry is `waiting`, `running`, or `running` and being stopped; when it ends it is replaced by one history row whose outcome is `exited`, `signaled`, `timeout`, `cancelled`, `wait_timeout`, `not_started` or `abandoned`. [`lifecycles.md` §3.6](lifecycles.md#36-host-queue-entry) draws the transitions. The exit code of `queue run` is ambiguous by design; `queue status <seq>` is the record.
- **Time is bounded.** A command has a run limit (default 30 minutes, `--timeout`) and may be given a wait limit (`--wait-timeout`). A command past its limit gets SIGTERM, then SIGKILL after `[queue] grace`, and keeps its slot until its process group is empty.
- **Detached runs.** `queue run --detach` returns a sequence number and an output-file path at once, so a harness that stops long foreground commands can submit a build and poll `queue status` for it.
- **A coordination aid, not a security boundary.** Any caller that can open the store may cancel any entry, and the canceller is recorded. The queue refuses a command that would start a model (`claude`, `codex` and similar), because that would hold a slot for a whole conversation. It does not sandbox the command.
- **Agents are told to use it.** The rule (`planar-agent queue rule` prints it, for pasting into a project's own agent guide) says builds and tests are queued host-wide and never run directly, and that a queue refusal (exit 125) is reported, not bypassed. It reaches agents through the role files, the skills and the generated workspace guides, and [`agents/doctrine.md`](../agents/doctrine.md) states its principle.
- **Where it sits in the binary boundary.** The queue is a `planar-agent` domain and never `planar`'s. It writes tables that no planning entity and no existing agent table shares, and it is outside the claim ritual: the only touch is `queue run --claim <token>`, which renews a claim the way `planar-agent heartbeat` does. `planar-watch queue` and `queue history` read it.

Operating the queue (watching, cancelling, settings, file modes) is in [`operations.md` §5](operations.md#5-the-host-build-and-test-queue); the schema contract for read-side tools is in [`architecture.md`](architecture.md#reading-the-queue-tables-from-another-tool).

---

## Deterministic workflow engine

`planar-execute` (revived in plan 633) is a deterministic, spawn-free Lua workflow engine — the fourth binary. An LLM caller (or any script) invokes `planar-execute run <wf.lua> --phase <name> [--args <json>]`; the engine loads the workflow in a Lua sandbox, registers an allowlisted, deterministic host surface, runs the named phase, and prints the workflow's `flow.result(table)` payload as JSON on stdout. It is the deterministic, spawn-free complement to a full external workflow harness: `planar-execute` runs only deterministic work and hands control back to its caller for any model step.

> **Centurion engine verbs are not part of this build.** In this tree `planar-execute` is the deterministic Lua workflow engine described in this section; its `run` path (and `profile show`) is the shipped surface. The Centurion client verbs (`submit`, `status`, `cancel`, `follow`, `host status|drain|stop`) require a Centurion-enabled build, available on the `dev/centurion-integration` branch. This build refuses them with `planar-execute was built without the Centurion engine` and exit `1`. See [INSTALL.md](../INSTALL.md) and the [CLI reference](cli-reference.md).

### No DB handle, no model spawn

`planar-execute` holds **no SQLite handle**. It reaches Planar state only by shelling the planning-state binaries via the `cli` host function (`cli.planar` / `cli.planar_json` — binary hardcoded to `planar`/`planar-agent`/`planar-watch`, the script supplies only args). It exposes **no** model-spawning primitive — no `agent`, `parallel`, `pipeline`, `dispatch`, `exec`, or any process-spawn function. This is the load-bearing invariant: an earlier `planar-execute` grew re-entrant headless LLM spawning and became a harness in its own right, which is why it was extracted to a separate external project; the revival reigns that scope back in by construction. A unit test asserts the registered host-fn set equals a frozen allowlist and contains none of the denied spawn-surface names.

### Confined host surface

The host functions are grouped: `cli.*` (allowlisted shell of the planar binaries), `git.*` (a `-C <worktree>`-confined group — the host injects the worktree dir, the script cannot name it), `fs.*` (read/write/exists/mkdir, path-confined to the sandbox root — `..` and absolute paths rejected), `flow.*` (pure: `log`, `phase`, `fail`, `result`), and `ctx.*` (deterministic planner reads — `plan_show`, `task_show`, `recommend_strategy`, `brief`, etc.). The Lua sandbox additionally nils `os`, `io`, `load`, `loadfile`, `loadstring`, `require`, `dofile`, and `math.random` so a workflow script cannot perform I/O or nondeterministic work from Lua itself.

### Hand-back model

Phases are discrete entrypoints — one clean process per deterministic segment. A setup phase runs, the engine exits, the caller does the LLM coder/reviewer step, then a measure phase runs in a fresh process. No coroutine parks awaiting a worker (that resume point is exactly where re-entrant spawning regrew); arm/repetition sequencing lives in the caller's loop, not in the engine.

---

## External workflow harness control plane

This section describes a separate external project that is neither part of this repository nor part of this build; it is documented here only to contrast it with `planar-execute`. An external Lua-based workflow harness drives agent workers through a host-function surface. It is a **separate external project**, not part of the Planar binary set, and must not be confused with the in-repo deterministic `planar-execute` engine described above: an external harness orchestrates LLM calls (it *is* a harness, with spawn surfaces), whereas `planar-execute` runs only deterministic work and exposes no model-spawn function. An external harness is architecturally distinct from the four planning-state binaries: it holds **no DB handle** and never opens SQLite. All state reads go through `planar` / `planar-agent` subprocesses; the workflow script cannot write directly to any database or planning entity.

### No-DB-handle stance

An external harness is a **pure CLI driver**. Every read operation shells `planar` or `planar-agent`, parses their JSON stdout, and returns the result to the Lua layer. Every write operation is similarly mediated: the workflow script calls a spawn primitive, which shells `claude -p` inside a constrained environment; the worker calls `planar-agent` verbs (claim, heartbeat, complete/fail/release/block) — never `planar` directly.

This makes the capability boundary physical, not just policy: the harness process cannot edit files, write DB rows, or call planning-entity mutations. Only the binaries it shells can, and only along the verbs those binaries expose. The Lua sandbox additionally strips `os`, `io`, and dangerous `math` functions so that workflow scripts cannot perform filesystem or network I/O from Lua itself.

### Constrained worker PATH

Agent workers run with a PATH restricted to:

- `planar-agent` — agent-table writes and coordination.
- `git` — source-tree reads and commits.
- System bin directories (for standard POSIX tools).

`planar` (the operator binary) is intentionally absent from the worker PATH. This preserves the no-bare-operator-binary invariant: a worker cannot call planning-entity mutations, trigger scope resolution, or open the DB read-write. The worker's only write surface is `planar-agent`'s bounded verb set.

### Lua control-plane internals

Inside the harness:

- A single `lua_State` is created per invocation and reused for the workflow's lifetime.
- A cooperative scheduler drives `ctx.parallel` (N-way barrier) and `ctx.pipeline` (per-item stage chains).
- A preemptive heartbeat thread fires at TTL/2 cadence independently of the Lua scheduler to keep active claims alive during long-running workflows.
- The journal (`ctx.phase`, `ctx.log`) records the execution arc as a sequence of timestamped entries; the journal is printed to stdout as the workflow progresses.

---

## Interactive cockpit

> **NOT IMPLEMENTED, AND NO LONGER IMPLEMENTED ANYWHERE.** There is no
> interactive TUI in the `planar` binary. `explore` is registered as a leaf,
> but its handler prints the leaf's own help page and exits 0
> (`explore_fallback` in `src/cmd/planar/dispatch.cpp`, decision 1003 /
> task 6444); it is the sole entry in that binary's `unported_paths()`
> inventory. Bare `planar` prints the root help regardless of TTY. Decision
> 980 records the cockpit as a **rewrite candidate, not a port**, and
> decision 982 excluded it from the zig-deletion gate, so the Zig
> implementation that provided it (libvaxis-based, under
> `src/cmd/planar/cockpit/`) was deleted with `zig/` at the M10 cutover
> without a replacement. There is no `vendor/libvaxis/` in this tree.

What the cockpit was specified to be — thirteen read-only views over the
planning graph, three editing tiers routed through `planar`'s existing write
paths, and a terminal-capability gate (`TERM=dumb`, `PLANAR_NO_TUI`,
`--plain`, non-TTY stdout) that fell back to help — is preserved as the
design record in
[`docs/architecture.md § Interactive cockpit`](architecture.md#interactive-cockpit--specified-not-implemented)
and [`docs/cli-reference.md § Domain: explore`](cli-reference.md#domain-explore).
One design point survives as doctrine: any future cockpit belongs in the
read-write `planar` binary, not in `planar-watch`, whose `SQLITE_OPEN_READONLY`
handle and zero-write verb set are load-bearing capability invariants.
