# Planar Concepts

This document explains the core concepts in Planar. Read it after `planar init` and before doing substantive work — the vocabulary here maps directly to CLI commands and database tables.

---

## Binaries

Planar ships as five executables. Four are **planning-state executables**, each with a disjoint capability boundary over the shared SQLite DB enforced **by the verb set the binary registers** (not by runtime ACLs). The boundary is a compile-time and install-time property: the binary on PATH literally has no verb for the work it is not allowed to do. This makes vendor-hook blast radius bounded — a hook configured with only `planar-agent` on its PATH cannot mutate planning state regardless of how it is invoked.

The fifth, `planar-execute`, is **not** a planning-state executable: it is the deterministic, spawn-free Lua workflow engine (plan 633) and holds no DB handle at all. A caller invokes `planar-execute run <wf.lua> --phase <name>` to run a deterministic workflow over an allowlisted host surface (`cli`/`git`/`fs`/`flow`/`ctx`) and collect its JSON result; it reaches Planar state only by shelling the planning-state binaries. It exposes no model-spawning host function, so it is a workflow *runner*, not a harness. See [the workflow-engine section](#deterministic-workflow-engine) below; do not conflate it with an external full-harness project described under [External workflow harness control plane](#external-workflow-harness-control-plane).

| Binary | Audience | Writes to |
|---|---|---|
| `planar` | Operator (human + scripts) | Planning entities (`plans`, `tasks.status` via manual transitions, `decisions`, `questions`, `scenarios`, `artifacts`, `annotations`, …) — everything **except** `agent_work_claims`. It does not write `agent_actions` either, save for one best-effort exception: the entity-create provenance hook (plan 467 D2/D3) appends a `created <entity>` action when `decision`/`question`/`artifact add` runs under an active agent claim; with no active claim it is a silent no-op. Also hosts the interactive operator cockpit (bare `planar` on a TTY, or `planar explore`). |
| `planar-agent` | Agent (vendor hook, orchestrator dispatch) + operator recovery | `agent_actions`, `agent_work_claims`, `tasks.status` (the last only as part of atomic coordinated operations: `pull`, `complete`, `fail`, `release`, `block`), `workflow_runs` (via `run start`/`end`), `context_records` (via `context add`/`resolve`). **Never** to plan / decision / question / scenario / artifact / annotation. |
| `planar-watch` | Operator (live view) + scripts (`--json`) | **Nothing.** Opens SQLite via `file:?mode=ro` so the driver itself rejects every write SQL string. |
| `planar-doc` | Operator + documenter agent | **`.planar-manifest` only** — the repo-state merkle index at the repo root. Never opens SQLite at all. |

**Capability invariant — `planar-agent`:** a process invoked as `planar-agent` has no verbs that mutate any planning entity. The verb set is exactly `pull`, `peek`, `claim`, `heartbeat`, `complete`, `fail`, `release`, `block`, `action start`/`action end`, `run start`/`end`, `context add`/`list`/`resolve`, `ingest`, `reconcile`, `abort`, `version`, `schema`.

**Capability invariant — `planar-watch`:** the binary's verb set contains zero write verbs (`feed`, `ps`, `claims`, `actions`, `plans`, `log`, `tree`, `run`, `version`, `completion`, `schema` only). Enforced two ways: (1) the verb set; (2) the read-only DB handle. `planar-watch` is **not** the interactive cockpit — it is and remains the scriptable, read-only NDJSON streaming viewer. The cockpit lives in the read-write `planar` binary because editing requires a read-write DB handle (see [§ Interactive cockpit](#interactive-cockpit)).

**Capability invariant — `planar-doc`:** the binary has no SQLite driver linked at all. Its verb set is exactly `build`, `verify`, `diff`, `cover`, `nodoc`, `lint`, `schema`. The only write is `.planar-manifest` at the repo root.

All three invariants are locked by `integration_tests/capability_boundary_test.zig` — a future change that registers a write verb on `planar-watch`, a planning-entity verb on `planar-agent`, or any SQLite-touching verb on `planar-doc` fails CI immediately. The `planar agent <verb>` subcommand namespace deliberately does not exist; agent observability lives on `planar-watch`, agent-table writes live on `planar-agent`.

The ritual every code-writing agent dispatch follows is `planar-agent pull → heartbeat → complete|fail|release|block` (atomic across all three tables). See [agents/methodology.md § Coordination claims](../agents/methodology.md#coordination-claims) and the tech spec § "Agent methodology contract" for the full sequence.

`planar-execute` is deliberately **outside** this ritual: it is a workflow engine the caller invokes, not an agent-table writer, and holds no DB handle. When a workflow needs to participate in a claim, it does so by shelling `planar-agent` verbs through the `cli` host function — exactly as any other caller would — never by holding a claim itself.

---

## Deterministic workflow engine

`planar-execute` (revived in plan 633) is a deterministic, spawn-free Lua workflow engine — the fifth binary. An LLM caller (or any script) invokes `planar-execute run <wf.lua> --phase <name> [--args <json>]`; the engine loads the workflow in a Lua sandbox, registers an allowlisted, deterministic host surface, runs the named phase, and prints the workflow's `flow.result(table)` payload as JSON on stdout. It is the deterministic, spawn-free complement to a full external workflow harness: `planar-execute` runs only deterministic work and hands control back to its caller for any model step.

### No DB handle, no model spawn

`planar-execute` holds **no SQLite handle**. It reaches Planar state only by shelling the planning-state binaries via the `cli` host function (`cli.planar` / `cli.planar_json` — binary hardcoded to `planar`/`planar-agent`/`planar-watch`, the script supplies only args). It exposes **no** model-spawning primitive — no `agent`, `parallel`, `pipeline`, `dispatch`, `exec`, or any process-spawn function. This is the load-bearing invariant: an earlier `planar-execute` grew re-entrant headless LLM spawning and became a harness in its own right, which is why it was extracted to a separate external project; the revival reigns that scope back in by construction. A unit test asserts the registered host-fn set equals a frozen allowlist and contains none of the denied spawn-surface names.

### Confined host surface

The host functions are grouped: `cli.*` (allowlisted shell of the planar binaries), `git.*` (a `-C <worktree>`-confined group — the host injects the worktree dir, the script cannot name it), `fs.*` (read/write/exists/mkdir, path-confined to the sandbox root — `..` and absolute paths rejected), `flow.*` (pure: `log`, `phase`, `fail`, `result`), and `ctx.*` (deterministic planner reads — `plan_show`, `task_show`, `recommend_strategy`, `brief`, etc.). The Lua sandbox additionally nils `os`, `io`, `load`, `loadfile`, `loadstring`, `require`, `dofile`, and `math.random` so a workflow script cannot perform I/O or nondeterministic work from Lua itself.

### Hand-back model

Phases are discrete entrypoints — one clean process per deterministic segment. A setup phase runs, the engine exits, the caller does the LLM coder/reviewer step, then a measure phase runs in a fresh process. No coroutine parks awaiting a worker (that resume point is exactly where re-entrant spawning regrew); arm/repetition sequencing lives in the caller's loop, not in the engine.

---

## External workflow harness control plane

An external Lua-based workflow harness drives agent workers through a host-function surface. It is a **separate external project**, not part of the Planar binary set, and must not be confused with the in-repo deterministic `planar-execute` engine described above: an external harness orchestrates LLM calls (it *is* a harness, with spawn surfaces), whereas `planar-execute` runs only deterministic work and exposes no model-spawn function. An external harness is architecturally distinct from the four planning-state binaries: it holds **no DB handle** and never opens SQLite. All state reads go through `planar` / `planar-agent` subprocesses; the workflow script cannot write directly to any database or planning entity.

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

The interactive cockpit is an operator-facing TUI embedded in the `planar` binary. It provides a live, multi-view window into the full planning graph — agent activity, tasks, decisions, questions, sessions, external systems, and more — with support for editing planning entities and triggering workbench or sync actions directly from the interface.

### Entry

There are two entry points:

- **Bare invocation.** `planar` with no verb on a TTY launches the cockpit, landing on the Scope Explorer. This is the "open the dashboard" idiom: `planar` becomes `vim` in the sense that the bare binary is the interactive entry point when stdout is a terminal.
- **Explicit alias.** `planar explore` is the explicit, always-available verb that does the same thing. Use it when you want to name the intent explicitly, or from a context where bare-invocation TTY detection may not fire (e.g. a tmux pane launched by a script).

Both paths run the same terminal-capability gate before entering the alt-screen.

### Terminal-capability gate

The gate determines whether to launch the cockpit or fall back to help/usage output. **Any one** of the following conditions triggers fallback:

- stdout is not a TTY (piped, redirected, CI, the integration test harness)
- `TERM=dumb` (terminal cannot handle VT sequences)
- `PLANAR_NO_TUI` environment variable is set (any value, including empty)
- `--plain` flag passed to `planar explore`

Skills, agents, and scripts always invoke explicit verbs and run in non-TTY contexts, so the cockpit never activates in automated pipelines — `TERM=dumb` / `PLANAR_NO_TUI` are available as belt-and-suspenders overrides when needed.

### Views

The cockpit ships thirteen views. Tab / Shift-Tab cycle through them; `1`–`9` jump to the first nine by position:

| View | Default key | What it shows |
|------|-------------|---------------|
| Scope Explorer | `1` / landing | Collapsible plan tree filtered to cwd-derived scope; scope toggle reveals all scopes; split detail pane renders artifact or task body |
| Agent Monitor | `2` | Live agent-claim roster with heartbeat coloring; event stream tails `agent_actions` and claim transitions |
| Task Board | `3` | Tasks grouped by status (`todo` / `doing` / `blocked` / `done`) with reopen history and touched-path detail |
| Decision Log | `4` | Decisions in chronological order; selecting one renders body and derives-from edges |
| Open Questions | `5` | Questions filtered by status; jump-to-linked entity |
| Test Scenario & Coverage | `6` | Scenarios with `verifies` edges; surfaces uncovered tasks and orphan scenarios |
| Entity-Link Graph | `7` | Related entities via `entity_links`; navigate an edge to refocus |
| External / Ops Plane | `8` | External systems, sync status, unresolved conflicts |
| Sessions & Handoff | `9` | Session lineage, resume-readiness, commit attribution |
| Audit Log | Tab | `audit_log` rows in chronological order, filterable by entity |
| CLI Invocation History | Tab | `cli_invocations` rows with verb / args-shape / outcome, filterable by verb |
| Scope / Association Topology | Tab | Associations mapped to member projects with scope-resolution context |
| Utility | Tab | Config inspector, annotations, workbench-sync state |

All views are **read-only projections** — they read existing tables and add no schema. The cockpit opens the DB read-write (as `planar` already does) only to support the editing tiers described below; the views themselves never write.

The default landing view is the Scope Explorer. On launch the cockpit filters to the cwd-derived scope and offers a one-key all-scopes toggle; it falls back to showing all scopes when launched outside any registered repository.

The wake loop behind the views is edge-triggered: a dedicated thread owns the `Wake` (kqueue on macOS, inotify on Linux, on the SQLite `-wal` file) and posts a `db_changed` event to the libvaxis event queue on a WAL change, and on a ≤1 second heartbeat as the coalesced/missed-wake backstop. The main loop re-queries the active view's data on each `db_changed` event with no polling.

### Editing tiers

The cockpit offers three editing tiers. All edits route through `planar`'s existing write engine paths and scope guards — no new write code or schema.

| Tier | Key | What it does | Guard |
|------|-----|--------------|-------|
| Entity-field editing | `e` | Edit a planning entity's title, body, or field (e.g. answer a question, update a task body) | Strict scope resolver — cross-scope edit refused without explicit scope; confirm-on-overwrite for destructive changes |
| Task lifecycle editing | `L` | Move a task through its status lifecycle (open → doing → done, block, reopen, reprioritize) | Claim-atomic: when the engine returns `ActiveClaimRefused`, the cockpit enters `force_confirm` mode — the overlay shows the active claim and prompts `f` to force-override or Esc to cancel. Never a silent raw flip under a live claim. |
| External / workbench actions | `S` | Trigger a sync/propagate or workbench push/pull/status | Confirmation-gated before executing |

Agent-claim mutation (releasing, reassigning claims) stays on `planar-agent` and is not available from the cockpit.

### Why the cockpit is in `planar`, not `planar-watch`

Editing requires a read-write DB handle, which is structurally incompatible with `planar-watch`'s `SQLITE_OPEN_READONLY` driver (the driver rejects every write SQL string; this is load-bearing for its capability invariant). `planar-watch` is unchanged and remains the scriptable, NDJSON-streaming, read-only viewer for agents and monitoring pipelines. The cockpit is in `planar` — the operator binary that already owns mutation — so the read-only boundary of `planar-watch` is preserved in full.

**SQLite tables:** projections of all existing application tables (no new tables or columns). **Primary entry points:** bare `planar` on a TTY, `planar explore [--plan <id>] [--task <id>] [--scope <s>] [--plain]`. **TUI framework:** libvaxis (vendored under `vendor/libvaxis/`). **Source:** `src/cmd/planar/cockpit/` (`gate.zig`, `app.zig`, the `view_model.zig` facade and `view_model/` query domains, `views/`, `widgets/`, `edit/`).

See [`docs/cli-reference.md § Domain: explore`](cli-reference.md#domain-explore) for the full flag reference and fallback conditions.

---

## Context plane

The context plane is the durable working-memory layer that lets one workflow stage pass structured information to the next. It is distinct from the session timeline (`session_entries`) by deliberate design (decision 445): `session_entries` is a narrative record of what happened; `context_records` is working memory — typed, lifecycle-managed rows that the next stage reads and acts on. The two tables have different consumers, different lifecycles, and overloading the timeline with working-memory noise would force every downstream reader to filter it out forever.

### Tables

**`workflow_runs`** is the identity and audit record for one external workflow harness invocation. A row is opened by `planar-agent run start` before the Lua `run()` function is entered, and closed by `planar-agent run end` after it returns. The harness itself holds no DB handle (decision 444) — it shells those verbs exactly as it shells the coordination verbs (`pull`, `complete`, etc.). The row carries `plan_id`, `workflow_name`, a unique `run_identifier` (`run-<pid>-<nanos>`), `pid`, `repo_root`, and a `status` in `running | completed | failed | interrupted | abandoned`. `abandoned` is written only by `planar-agent reconcile`, which pid-probes stalled rows whose process is no longer alive. Dry-run (`--dry-run`) creates no run row.

**`context_records`** is run-scoped working memory. Every record is keyed `(run_id, stage, session_id, claim_id)` and carries a `kind` (`finding`, `risk`, `artifact`, `followup`, `summary`, `capsule`) plus a free-text `body`. The `status` column (`active | consumed | superseded`) is the lifecycle signal. A nullable `compiled_from` column on `capsule` records stores the integer ids of the raw records the capsule distilled — full provenance without deletion.

### Accumulate → read → compose loop

**Accumulate.** A worker writes records via `planar-agent context add --claim <token> --kind <kind> --body <text>`. The claim token is the only envelope the worker needs to thread (decision 447): `planar-agent` stamps `run_id`, `stage`, `session_id`, and `task_id` server-side from the claim row. The claim row gains nullable `run_id` and `stage` columns (migration 00023), populated at `pull`/`claim` time when the orchestrator passes `--run <id> --stage <name>` (decision 450). Interactive claims leave these null; the context verb is a no-op for claims without a run row.

**Read.** A workflow script reads accumulated records from a prior stage via `ctx.context([stage])`. With no argument it returns all records for the current run; with a stage name it returns only that stage's records. The Lua return value is a 1-based sequence of tables, each carrying `id`, `run_id`, `stage`, `kind`, `body`, `status`, `compiled_from` (nil when absent), and `created_at`. Under the hood the external harness shells `planar-agent context list --run <run_db_id> [--stage <s>] --json` and maps the parsed JSON into the Lua table — no DB handle is opened.

**Compose.** `ctx.brief({...})` assembles a methodology-compliant coder brief and automatically injects the current run's context records into the "Prior-stage context" section. Any `capsule`-kind record is promoted to a compiled-capsule sub-section; remaining records render as a `kind: body` bullet list. The caller supplies the `problem_statement`, `claim_token`, `gates`, and optional `spec_citations`/`locked_decisions`; the context injection is automatic when `active_run` is non-null.

### Lifecycle: active → consumed | superseded, never deletion

Raw records start life as `active`. Stage close marks them `consumed` (records incorporated into the capsule) or `superseded` (records overridden by a later record in the same stage) and writes one compiled `capsule` record whose `compiled_from` column points back to the raw record ids (decision 446). Raw records are retained permanently — the audit trail is preserved for `pl-introspect` and journal-based resume. The three-value status is the machine-readable lifecycle signal; `compiled_from` is the provenance trace.

### Observability

`planar-watch run list [--plan <id>] [--status <s>]` lists runs. `planar-watch run show <id> [--json]` returns the full run row plus all `context_records`, grouped and ordered by stage then `created_at`. The JSON shape is `{run: RunRow, context_records: [...]}`.

**SQLite tables:** `workflow_runs` (migration 00022), `context_records` (migration 00022), `agent_work_claims.run_id/stage` (migration 00023). **Primary verbs:** `planar-agent run start/end`, `planar-agent context add/list/resolve`, `ctx.context([stage])`, `ctx.brief({...})` (host functions on the external workflow harness), `planar-watch run list/show`. **Decisions:** 444 (run row owned by `planar-agent`; harness is DB-handle-free), 445 (separate table — timeline vs working memory), 446 (lifecycle not deletion; capsule provenance), 447 (claim is the correlation key), 450 (claims carry run/stage).

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

Both the read and write resolvers begin by walking from the current working directory up the filesystem. `DeriveFromCwd` collects every registered scope whose root path is a prefix of cwd. Two kinds of root path are matched:

- A `projects.root_path`. The cwd is inside a registered repo; the resulting candidate can be the concrete `repo:<slug>` scope, and any association memberships for that project can also contribute association candidates.
- An `associations.config_json.root_path` for `kind in ('org', 'client', 'personal', 'ad-hoc')`. The cwd is at (or inside) a workspace root registered via `planar workspace init` or the equivalent assoc creation flow.

Each match becomes a `Candidate` carrying the association id, kind, and the root path that fired. When two project roots both match cwd, the longer `projects.root_path` wins: a cwd under `~/work/root/modules/nested/` resolves to the nested project instead of the containing root project, while a cwd under `~/work/root/src/` resolves to the root project.

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

A repo scope outranks an `org` whose membership contains that project, so a cwd inside `~/work/repo-a/` resolves to `repo:repo-a` even when `org:work` also matches. Association candidates of kind `project` rank above raw repo candidates for compatibility with older project-association flows. At the workspace root (`~/work/`, no project root_path contains it) only `org:work` matches, so the org wins — but at the workspace root writes refuse rather than land in the org by default (see below).

### Write resolution

Writes use the strict `scopearg.ResolveForWrite` algorithm. The first step that yields a single unambiguous scope wins:

1. **Explicit flag.** If `--scope` is passed, parse it and return. The flag is the user's stated intent; never override it.
2. **Cwd derivation, most-specific-wins.** Run `DeriveFromCwd`, rank by specificity, return the single most-specific match. Two refusals fire here:
   - If multiple candidates tie at the best rank, refuse with an `AmbiguousScopeError` listing the tied `--scope` values.
   - If the single winner is `kind=org` and the org has at least one member project, refuse with the workspace-root variant (see below). A zero-member org passes through — that is the workspace-init edge case where writing at the org level is the only sensible choice.
3. **Refuse.** Return an `AmbiguousScopeError`. No silent default; no fallback to ambient state.

### Workspace-root refusal

When cwd lands at a registered workspace root (org `config_json.root_path`) and the org has members, `task add` (or any other write verb without `--scope`) refuses with the candidate list:

```
error: you are in a workspace root with 3 member projects, but no
       specific project scope was passed. Choose one with --scope:

         --scope repo:repo-a
         --scope repo:repo-b
         --scope repo:repo-c

       Or cd into a specific member project. Pass --scope
       assoc:work to write at the org level (cross-repo work).
       Pass --no-scope-check to override (legacy escape hatch).
```

Fan-out is deliberately not the default: a workspace-root `task add` could plausibly mean any of the members or the org itself, and silently picking is the lectio incident pattern. The refusal forces the operator to state which.

### Read resolution

Reads use `scopearg.ResolveForRead`. The contract:

- If `--scope` is set, parse and return that single resolved scope.
- Otherwise run `DeriveFromCwd` and apply the same specificity ranking as the write path. Reads return a `[]Resolved` set, not a single scope:
  - At a workspace root, the set is the org plus every member project (the operator's expectation of "show me everything under this workspace").
  - At a member project root or subdirectory, the set is the most specific registered repo. Longer `projects.root_path` matches beat shorter parent roots, so nested repos do not leak parent-repo work.
- If cwd matches zero registered scopes and no flag is passed, refuse with a clear message instructing the operator to `cd` into a registered scope or pass `--scope global` for the global slice. There is no silent fallback.

This composes naturally with `plan list`, `task list`, `question list`, `scenario list`, `decision list`, `artifact list`, `search`, and `tree` — these query verbs see the same cwd-derived set.

### Cross-scope guard

Layered on top of the write resolver, the [cross-scope guard](#cross-scope-guard) compares the operator's resolved scope against each *target entity's* stored `(scope_kind, scope_id)` and refuses if they disagree. The guard is membership-aware: an operator scope `assoc:<org>` covers any entity scoped to one of the org's member projects (via `project_associations`). The reverse — operator project, entity org — still refuses; cross-repo coordination requires either `cd` to the workspace root or `--scope assoc:<org>` explicitly.

### Removed: the active scope stack

Earlier releases maintained a per-database `active_scope` table and exposed `planar scope use`, `planar scope pop`, and `planar scope clear` to manipulate it. Plan 153 M5 dropped the table (migration `migrations/00009_drop_active_scope.up.sql`) and removed the verbs; concurrent sessions sharing one database can no longer trample each other through stack manipulation. Operators who habitually typed those verbs get an exit-1 redirect pointing at `planar scope show`.

**SQLite tables:** `associations`, `project_associations`, `projects`. **Primary verbs:** `planar scope show` (derived view), `planar scope suggest`. Set the scope for any verb by `cd`-ing into the target or passing `--scope <slug>`.

---

## Cross-scope guard

The cross-scope guard is a refusal mechanism that fires when the operator's resolved write scope disagrees with the target entity's stored `(scope_kind, scope_id)`. Guarded verbs read the entity's scope, resolve the operator's scope through the strict write resolver above, compare the two, and refuse with exit 1 if they differ.

### Why it exists

The motivating incident: an operator running from `~/work/lectio/` invoked `planar spec ingest 88` against a plan that belonged to a different project's scope. The verb materialised 126 child entities — plans, tasks, decisions, scenarios — under the cwd's association rather than the anchor plan's. The rows were recoverable but the silent cross-scope materialisation was the bug. The guard exists so verbs that walk *from* a parent entity *to* its children, or that mutate a specific existing entity, refuse to proceed when operator intent and entity provenance disagree.

### Which verbs are guarded

Two classes of verb are guarded:

- **Bulk-write-from-parent.** Verbs that take a parent entity id (typically a plan) and write a tree of derived rows. The parent's scope is the natural scope for the derived rows; running from a cwd that resolves to a different scope is the lectio incident pattern. Verbs: `spec ingest --apply`, `ext propagate`, `sync push <link|kind:id>`, `sync pull <link|kind:id>`, `sync resolve`. The default `spec ingest` preview is read-only: an explicit numeric plan id may locate and inspect an anchor outside the cwd-derived scope, but crossing into `--apply` requires a matching cwd or explicit `--scope`.
- **Mutating-existing-entity.** Verbs that take an existing entity id and rewrite it (or its links). Routing such a mutation through the wrong cwd updates the row but skews future writes that follow the same code path. Verbs: `plan update`, `plan step add/done/skip`, `task update/done/block` (both endpoints on `block`), `question edit`, `scenario edit`, `decision edit`, `decision supersede` (both old and new), `artifact update`, `audit publish-decision`, `ext create --from`, `link <kind:id> --to`, `unlink`, `links update`.

### Membership-aware coverage

The comparison is not strict equality. An operator scope `assoc:<org>` covers any entity whose stored scope is a project that belongs to the org via `project_associations`. From a workspace-root cwd with `--scope assoc:work` (or via the workspace-root opt-in flow), the guard accepts writes targeting `project:repo-a`, `project:repo-b`, etc. — the org operator is "above" its member projects. The reverse direction (operator `project:repo-a`, entity in `assoc:work`) still refuses; cross-repo coordination requires explicit org scope.

### Which verbs are deliberately not guarded

Several verb classes were audited and explicitly left unguarded; the absence is not an oversight:

- **All `*_link` and `links add/remove` verbs.** `entity_links` is cross-entity by design — the polyrepo `touches`/`derives-from` story depends on edges crossing scope boundaries.
- **All `*_add` / `*_create` verbs.** A newly-created entity has its own `--scope` resolved through the write resolver; `--plan` or `--parent` on a create verb is a reference, not scope inheritance.
- **`sync push --all` / `sync pull --all`.** Bulk fan-outs that the operator opts into explicitly.
- **All read-only verbs.** `show`, `list`, `status`, `audit trail`, `tree` — reads do not corrupt state and the audit-from-anywhere case is the common case.
- **Identity-bucket verbs** (`assoc`, `init`, `promote`/`demote`, `scope`, `workspace`). Associations *are* scope; `promote`/`demote` deliberately cross scopes (that is the verb's job).
- **Operator-state verbs** (`handoff`, `capture`, `resume`). These manage vendor-session rows, not project-scoped entities. The legitimate polyrepo handoff workflow is "a session inside repo A captures a handoff that references a task in repo B".
- **`planar-doc` verbs** (`build`, `verify`, `diff`, `cover`, `nodoc`, `lint`). The `planar-doc` binary never opens SQLite, so there is no operator-vs-entity scope comparison to make. Its only write is `.planar-manifest` at the repo root — a repo-state artifact, not a scope-owned planning entity.

### Escape hatch

`--no-scope-check` downgrades the refusal to a one-line stderr warning and proceeds. Appropriate uses: legacy scripts that cannot be updated immediately, one-off corrections after a scope-rewrite migration, and ad-hoc fix-ups. Inappropriate uses: routine workflows, skills, agents, orchestrator code. Reach for `--scope <slug>` first to assert explicit intent; `--no-scope-check` is the documented fallback when an explicit scope is not yet known.

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

`planar assoc detect` auto-detects associations from the current repo's git remote and parent path. `planar assoc add` creates one manually.

**SQLite table:** `associations`, `project_associations`. **Primary verbs:** `planar assoc add`, `planar assoc list`, `planar assoc detect`.

---

## Project

A project is a registered local working directory — what you'd call a "repo." It is identified by its `root_path` on disk and has a `slug` that is used in scope references (`--scope repo:<slug>`).

`planar init` registers the current directory as a project (or upgrades an existing registration) and applies any pending schema migrations. You only run it once per repo. Registration does not automatically create an association; human output gives the exact `planar assoc create` and `planar assoc add` commands for that next step.

Projects are read-only after `init` — the project record is not meant to be updated or deleted. Associations are the mechanism for grouping projects.

Use `--scope repo:<slug>` when a plan, task, question, scenario, decision, or artifact belongs to the repository itself. Use `--scope assoc:<workspace>` for cross-repo coordination work that intentionally sits above any one repository. A root repo and a nested repo can both be registered projects; cwd matching picks the longest root path so nested work does not collapse into the containing repo.

`planar plan create` refuses an implicit write from a registered project that has no association, because treating the missing association as global would hide an ownership mistake. Add the project to an association first, or pass `--scope global` explicitly when the plan is intentionally global.

**SQLite table:** `projects`. **Primary verbs:** `planar init`, `planar project list`, `planar project show`.

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
- `.manifest-docs` — drift manifest (plan 96) tracking the generated files.

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

The load-bearing rule for `synthesize` is that **code presence beats text claims**. A task the LLM proposes with `status != "todo"` must cite a `code_evidence` path that exists in the probed source tree; the binary's `Validate` step (`src/engine/synthesize.zig`) refuses results that violate the invariant. A roadmap line that says "M3 is finished" is treated as TODO unless source files, tests, or CI configs corroborate the claim. The greenfield case (no source detected) collapses naturally onto all-todo output.

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

A plan is the anchor unit of work. It is a structured intent — a named body of work with a status lifecycle. Plans are hierarchical: a child plan has a `parent_plan_id` and belongs to its parent's feature tree.

### Status lifecycle

```
draft → active ⇄ paused
          ↓
        done  (terminal)
          |
       abandoned  (terminal)
```

Legal transitions (enforced by `policy.status.check`):

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

`plan.recomputeStatus` (triggered by task writes) deliberately bypasses this check — it is an engine-internal aggregate roll-up whose target is computed by `computeTarget` and can only emit transitions the matrix considers valid. Operator overrides go through `plan update --status <s>`.

A plan has a filesystem-safe `slug` unique within its parent scope, used in workbench directory names.

The anchor plan for a feature is the top-level plan with no `parent_plan_id`. Child plans are used for sub-features or roadmap milestones within a larger feature.

**Status auto-promotion (plan 304).** Plan status is a function of task status, enforced at task-write time. The auto-promotion invariant fires inside every `task.Add` / `Update` / `Done` / `Reopen` / `Cancel` / `Block` transaction and applies a transition matrix that flips the plan based on the post-write task aggregate. The matrix lives in [`agents/methodology.md` §Plan-status invariant](../agents/methodology.md#plan-status-invariant).

Two operator-visible consequences:

- **Child plans auto-promote.** A child plan flips `draft → active` when any task starts, and `active → done` when every task is terminal (done or cancelled). The orchestrator no longer needs to walk child plans manually — `planar task done <id>` on the last task flips the parent child plan to `done` in the same transaction.
- **Anchor plans don't auto-promote to done.** The anchor's `done` transition is a release-gate decision; the invariant only auto-flips anchors to `active`. Operators close an anchor explicitly with `planar plan update <id> --status done`. A plan's status also depends only on its OWN tasks — a child plan being done does NOT propagate to its parent anchor's status.
- **Paused and abandoned are operator overrides.** Both are no-ops for the recompute. Re-engage with `plan update --status active`.

The opt-out is `--no-auto-promote` on the task verbs, used by migrations and scripted bulk edits that don't intend the plan-level transition.

Each transition emits a `session_entries` row with `prefix='note'` and a body that begins with the sentinel line `plan_status: <id>` — recoverable via `planar audit trail --kind plan <plan-id> --grep "^plan_status:"`.

**SQLite table:** `plans`. **Primary verbs:** `planar plan create`, `planar plan show`, `planar plan list`, `planar plan active`, `planar plan done`, `planar plan abandon`.

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

**Apply semantics:** Without `--dry-run`, passing the hard gate marks the plan `done` directly — bypassing the `recompute-status` anchor cap. This is intentional: `plan closeout` is the explicit operator release-gate for anchor plans. `--dry-run` evaluates and reports without writing. Both modes exit non-zero when the hard gate is blocked.

---

## Task

A task is the leaf unit of work. It is attached to a plan via `plan_id` and optionally to a parent task via `parent_task_id` (subtasks). Tasks are the entities that agents implement.

### Status lifecycle

```
        ┌──────────────────────┐
        ↓                      |
todo ⇄ doing ⇄ blocked → done  (terminal)
  ↘     ↓                ↓
cancelled (terminal)  cancelled (terminal)
```

Legal transitions (enforced by `policy.status.check`):

| From | To | Notes |
|------|----|-------|
| `todo` | `doing`, `blocked`, `cancelled` | |
| `doing` | `todo`, `blocked`, `done`, `cancelled` | |
| `blocked` | `doing`, `done`, `cancelled` | |
| `done` | `todo`, `doing`, `blocked` | Only via `task reopen --reason` or `task update --force` |
| `cancelled` | `todo`, `doing`, `blocked` | Only via `task reopen --reason` or `task update --force` |

- `todo`: task is queued, not yet started.
- `doing`: an agent is actively working on it.
- `blocked`: work is stalled; requires `next_action` — `planar resume validate` refuses a resume packet without it.
- `done`: task is complete. Terminal for bare `task update`; escape via `task reopen --reason <why>`.
- `cancelled`: task was deliberately dropped. Terminal for bare `task update`; escape via `task reopen --reason <why>`.

**Verb-gated escape from terminal status.** `planar task reopen <id> [--status todo|doing|blocked] --reason <why>` performs the terminal → open move that bare `task update --status` refuses, and records a `task_reopens` audit row. `task update --force` is the operator override that also performs the move and records a `task_reopens` row with `source='task-update-force'`. Both paths bypass the matrix explicitly; the bypass is the documented exception, not the default.

**Identity transition** (`from == to`) is accepted silently by all arms — a redundant `--status doing` on a `doing` task is a no-op, not a refusal.

Tasks carry a `title`, an optional `body` (Markdown), a `next_action` field for handoff continuity, and a `scope_kind`/`scope_id` pair that records which scope they belong to.

### Claim-atomic operator transitions

Operator-driven status transitions (`task done`, `task block`, `task reopen`, `task update --status`) are **claim-atomic**: when the task has an active work claim (`agent_work_claims.status = 'active'` and `lease_expires_at >= now()`), the operator verb refuses the status flip and exits non-zero with a message identifying the active claim.

This closes the TOCTOU window where an operator `task done` would strand a live agent lease mid-flight. The correct closure path for a claimed task is the agent terminal verb (`planar-agent complete | fail | release`). The operator override is `--force`, which bypasses the claim guard AND the status-transition matrix — use it only when the agent is known to be no longer active (e.g. the process crashed without releasing its claim).

Expired claims (`lease_expires_at < now()`) are NOT active and do not trigger the guard. The check is real-time on every operator status-flip.

**SQLite table:** `tasks`. **Primary verbs:** `planar task add`, `planar task list`, `planar task show`, `planar task doing`, `planar task done`, `planar task block`, `planar task cancel`, `planar task reopen`.

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

**Audit trail.** Every cycle emits a `session_entries` row with `prefix='note'` and a structured body that begins with the sentinel line `dispatch_shape: <shape>`. The `session_entries.prefix` CHECK constraint allows a fixed set (`action / observation / decision / question / file / command / note / error / read`); the dispatch convention reuses `note` with the sentinel body as the grep-recoverable alternative — the same pattern used by plan 304's `plan_status:` audit trail.

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

For the canonical contract see [`agents/methodology.md` §Barrel modes](../agents/methodology.md#barrel-modes). For the CLI-flag surface see [`docs/cli-reference.md` §`/pl-orchestrator`](cli-reference.md#planar-orchestrator).

**SQLite tables:** none beyond `session_entries`. **Primary entry points:** `/pl-orchestrator` (the gate), `agents/methodology.md` §Barrel modes (the contract), `planar audit trail --kind plan <plan-id>` (the forensic surface).

---

## Orchestration strategy

An orchestration strategy is the operator-facing dispatch frame for a plan. It bundles five underlying axes into one named choice the operator confirms at the strategy gate — Phase 3's first sub-step, run **before** the existing dispatch-shape gate. Strategy answers "what is the overall methodology for this plan?" Dispatch shape (the next gate, nested under it) answers "within that strategy, how do I batch *this cycle's* work?"

### The named strategies and isolation choice

| Strategy | One-line description |
|----------|----------------------|
| `classic` | Sequential cycles, reviewer per cycle. Default isolation is `pwd` on the current branch; `worktree` isolation is selectable when rollback or pwd hygiene matters. |
| `parallel-fanout` | Fan out to N parallel coders on the parallel-eligible subset of the plan's open tasks; each in its own worktree off the shared epic branch; staged into dependency-respecting waves; one consolidated reviewer pass at fan-in. **Model-runnable** via the spawn-free `workflows/parallel-dispatch.lua` seam (plan 760) — no external harness. |
| `isolated-sequential` | Descriptive alias for `classic` + `worktree` isolation: one cycle worktree per task, reviewer per cycle. |
| `barrel-deferred` | Coder cycles run back-to-back; reviewer fires once at a milestone or plan boundary on the union diff. Supports both `pwd` and `worktree` isolation. |
| `barrel-bypass` | No reviewer dispatch at all. Quality gates (`make fmt-check` + `make build` + `make test` + `make test-integration` twice + render check + remaining validators) are the entire signal. Supports both `pwd` and `worktree` isolation. |

The model-driven `/pl-orchestrator` skill runs the sequential strategies in either `pwd` or `worktree` isolation and runs `parallel-fanout` in worktrees. Worktree bookkeeping is driven via the spawn-free `workflows/parallel-dispatch.lua` seam: `cycle_plan` computes one sequential lane; `plan`/`waves` compute fan-out lanes. The model runs the git worktree/branch/merge ops and spawns the coders; the seam only computes and hands back. There is **no external harness**. In-flight worktree execution is watched through the existing `planar-watch ps --plan <id>` surface (claims + each claim's `worktree_path`); there is no dedicated wave/barrier view (a recorded non-goal).

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

1. **Strategy + isolation gate.** The model orchestrator surfaces its recommendation + a one-line rationale + the runnable strategy menu (`classic` / `barrel-deferred` / `barrel-bypass` / `parallel-fanout`) and the isolation choice (`pwd` / `worktree`) where applicable. Operator confirms or overrides. Skipped only when `--strategy <name>` plus any needed axis flags was passed at invocation. The recommendation algorithm is plan-shape-driven with status-quo bias — see [`agents/methodology.md` §Recommendation algorithm](../agents/methodology.md#recommendation-algorithm).
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

For the canonical axis table, named bundles, invalid-combination list, and recommendation algorithm see [`agents/methodology.md` §Orchestration strategies](../agents/methodology.md#orchestration-strategies). For the "pick a strategy" recipe and a worked `parallel-fanout` example see [`docs/workflows.md` §Recipe 21](workflows.md#recipe-21--pick-an-orchestration-strategy-for-a-plan) and [§Recipe 22](workflows.md#recipe-22--orchestrate-a-multi-task-plan-with-parallel-coders).

**SQLite tables:** none — strategy is metadata on the dispatch row. **Primary entry points:** `/pl-orchestrator` (the gate), `agents/methodology.md` §Orchestration strategies (the contract).

---

## Worktree

A Planar worktree is a git working tree created for an isolated coder cycle. It is a real `git worktree add` checkout — Planar does not reinvent the git primitive, it just owns the path convention and the persistence of which claim owns which worktree.

**Worktree lifecycle — creation, the epic/cycle branch model, fan-in merge, failed-lane retention, and full teardown on plan completion — is model-driven for both sequential worktree isolation and `parallel-fanout`**. The deterministic wave/lane/merge/teardown computation lives in the spawn-free `workflows/parallel-dispatch.lua` seam: `cycle_plan` computes one sequential lane, and `plan`/`waves` compute fan-out lanes. The model runs the git ops and spawns the coders. Parallel eligibility applies only to `parallel-fanout`; a single sequential worktree lane does not require `task_touches`.

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

The standalone-entity alternative remains available — the forward-compat `validateWorktreeId` hook in `src/engine/runtime/agentactivity/store.zig` is the seam — but the claim-attached model satisfies every current use case (dispatch persistence, resume recovery, observability).

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
2. **Planning verbs are refused from inside worktrees.** Verbs that mutate planning state (`plan add/update/done`, `task add/update/done/touches`, `question`, `decision`, `artifact add/update`, `scenario`, `spec draft/ingest`, `link/unlink/links`, `assoc`, `promote`, `demote`, `init`) refuse with a distinct exit code and a message pointing at the parent repo's cwd. `task done` is refused on purpose — coders use `planar-agent complete --claim <token>`, the atomic terminal verb. `--scope <slug>` does NOT override the refusal; the rule is about *where the verb runs*, not which scope it targets.

For the canonical path scheme, branch scheme, lifecycle, the six parallelizability rules, and the conflict-resolution taxonomy see [`agents/methodology.md` §Worktrees](../agents/methodology.md#worktrees). For the recovery recipe when a coder dies mid-cycle see [`docs/workflows.md` §Recipe 23](workflows.md#recipe-23--recover-a-dead-coder-from-its-worktree).

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

Legal transitions (enforced by `policy.status.check`):

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

A decision is a recorded design choice. Decisions have a `kind` (`design`, `technical`, `process`) and a status lifecycle:

```
proposed → accepted
         → superseded  (terminal)
         → withdrawn   (terminal)

accepted → superseded  (terminal)
         → withdrawn   (terminal)
```

Legal transitions (enforced by `policy.status.check`):

| From | To |
|------|----|
| `proposed` | `accepted`, `superseded`, `withdrawn` |
| `accepted` | `superseded`, `withdrawn` |
| `superseded` | — terminal |
| `withdrawn` | — terminal |

Architecture decision records (ADRs) are artifacts of `kind=adr`, not decision rows — the `decisions` table is for in-flight design choices made during feature work. A decision row points to the session in which it was made and optionally to a plan.

**SQLite table:** `decisions`. **Primary verbs:** `planar decision add`, `planar decision list`, `planar decision accept`, `planar decision supersede`.

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

Legal transitions (enforced by `policy.status.check`):

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

Legal transitions (enforced by `policy.status.check(.annotation, …)`):

| From | To |
|------|----|
| `active` | `resolved`, `dismissed`, `archived` |
| `resolved` | `archived` (retention-tier progression) |
| `dismissed` | `archived` (retention-tier progression) |
| `archived` | — sole final state |

All other moves are illegal: `resolved → dismissed`, `dismissed → resolved`, `resolved → active`, `dismissed → active`, `archived → anything`. Identity (`from == to`) is a no-op.

`annotate sweep --since-days <n>` selects `resolved`/`dismissed` rows older than the cutoff and archives them (resolved→archived and dismissed→archived are both legal), making sweep the primary housekeeping path for outcome rows that have aged past their review window.

**SQLite tables:** `annotations`, `annotation_tags`. **Primary verbs:** `planar annotate add`, `planar annotate resolve|dismiss|archive`, `planar annotate bulk-resolve|bulk-dismiss|bulk-archive`, `planar annotate sweep`, `planar annotate verify`.

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

**SQLite table:** `workbench_sync_state` (tracks per-file sync state). **Primary verbs:** `planar workbench push`, `planar workbench pull`, `planar workbench sync`, `planar workbench status`, `planar workbench archive`, `planar workbench restore`, `planar workbench gc`, `planar workbench publish`. **Primary engine module:** `src/engine/workbench/terminal.zig` (the comptime status table that backs the filter — a future status added by a migration is a compile-time error here).

---

## Entity Link

An entity link is a typed cross-entity relationship. Any two entities of any kind can be linked. Links are stored in the `entity_links` table.

### Relationship types

| Type | Meaning |
|------|---------|
| `derives-from` | This entity was derived from the referenced entity |
| `blocks` | This entity cannot proceed until the referenced entity is resolved |
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
| `mirror` | This external item was created by Planar's `ext propagate` and tracks the local entity one-to-one |
| `reference` | This external item was created independently; the link is informational |

Each `external_links` row also carries a `config_json` blob used by the ext-sync engine to cache per-feature propagation state (selected GitHub strategy, Projects v2 node id, etc.). This is what makes strategy selection sticky across re-propagation runs.

`planar link <entity> <system-slug>:<external-id>` creates a reference link manually. `planar ext propagate <plan>` creates mirror links automatically.

**SQLite table:** `external_links`, `external_systems`, `sync_events`. **Primary verbs:** `planar link`, `planar unlink`, `planar ext propagate`, `planar ext create`, `planar sync pull`, `planar sync push`.

---

## Session

A session is a durable agent-execution record. One session = one vendor's episode of work, scoped to a task and project. Sessions are the backbone of agent continuity — they make it possible to resume work after compaction, a vendor switch, or a process restart.

Every command that writes to the database appends a `session_entries` row to the current active session for the affected task. If no session exists, one is auto-created using the `PLANAR_VENDOR` environment variable.

A **handoff** record is written at the end of a session via `planar handoff`. It includes a `context_snapshots` row (the resume packet) and the metadata needed for the receiving agent to pick up where the previous one left off.

`planar resume <task-id>` assembles the resume packet — task state, recent session entries, open questions, decisions made, and next action — into a structured prompt the new agent session reads at startup.

**SQLite tables:** `sessions`, `session_entries`, `context_snapshots`, `handoffs`. **Primary verbs:** `planar capture session`, `planar capture end`, `planar capture note`, `planar handoff`, `planar resume`.

---

## Templates layer

Planar ships per-entity-kind Markdown templates that seed new entities created via the editor-first `add` verbs. The shipped set lives in the repo under `templates/entity/` and is installed to `~/.planar/templates/entity/` by `install.sh` (existing files are preserved on re-install; `--force` overwrites).

Layout:

```
~/.planar/templates/entity/
├── artifact/
│   ├── product_spec.md
│   ├── tech_spec.md
│   ├── roadmap.md
│   ├── research.md
│   ├── decision-record.md
│   └── default.md       # fallback for unrecognized artifact subkinds
├── decision.md
├── task.md
├── question.md
└── scenario.md
```

Each template is a Markdown file with a YAML frontmatter block plus a short body skeleton. Placeholders use a Go-template-compatible mini-language: `{{.Title}}`, `{{.PlanID}}`, `{{.PlanSlug}}`, `{{.ArtifactKind}}` (artifact templates only), `{{.Priority}}` (task template only), `{{.Date}}`. The loader (`src/cmd/planar/editflow.zig` + `src/engine/templates/`) treats an unknown placeholder as an error so typos surface immediately.

**Customizing.** Operators edit the installed file directly; `install.sh` does not clobber an existing template unless run with `--force`. Per-workspace template overrides are a future seam tracked in the M2 tech-spec.

**Validation.** A unit-time `LintAll` renders every shipped template against a sentinel `Vars` to catch placeholder typos. A richer schema-aware validator at install time is tracked as a follow-up (task 740).

**SQLite tables:** none — templates are pure filesystem assets. **Primary entry points:** `src/engine/templates/loader.zig` (resolve + load), `src/engine/templates/render.zig` (render), `src/engine/templates/validate.zig` (lint). Engine-internal, not a CLI surface.

## Model routing

Which model an agent role spawns is **config-driven and unified** (plan 540, extended plan 899). The Planar config (`~/.planar/config.toml`, embedded defaults in `src/engine/config/defaults.toml`) carries:

- `[models.<vendor>]` — per-vendor **tier maps**: the canonical `small` / `medium` / `large` tiers → a **scalar-or-list** candidate value (e.g. `[models.claude] medium = "claude-sonnet-5"`). A scalar is one candidate; an ordered list (`large = ["gpt-5.6-sol", "gpt-5.5"]`) names several — `list[0]` is always the **tier default**, the model any caller gets when it resolves a bare `(vendor, tier)`/`(vendor, role)` pair with no work type in hand. Every existing scalar config resolves unchanged.
- `[routing.<vendor>.<tier>]` — a **work-type → candidate** map (plan 899 D4/D9/D10/D11): each key is one of `schema | engine | architectural | cli | feature | mechanical` and its value is a candidate **model id string** naming one member of that tier's candidate list (never a list index — index values silently re-route when the list is reordered). Ships as an embedded default (only `mechanical` is routed by default, to the tier default) and is fully operator-overridable. `planar config validate` rejects a routing entry naming a model id absent from the matching tier's candidate list.
- `[roles]` — **role → tier** (e.g. `coder = "medium"`, `reviewer = "large"` — the "sonnet coder, opus reviewer" default).
- `[role_vendors]` — optional **role → vendor** override; unset roles use `[defaults].vendor`.

A single **shared resolver** (`src/engine/models.zig`) composes these into a concrete `(vendor, model)`. The tier-only path (`resolveTier`, `resolveRole`, `resolveRoleAuto`) is unchanged and always returns the tier default (`list[0]`). A parallel `resolve(role, work_type)` entry point (`resolveTierWorkType` / `resolveRoleWorkType` / `resolveRoleAutoWorkType`) additionally consults the routing map: a hit returns the named candidate, a miss (or a stale routing target absent from the candidate list) falls back to the tier default. Every consumer resolves through one of these — there are no parallel per-tool model tables:

- **`planar models`** — `list` (discover installed provider CLIs + curated catalog), `routing` (effective role→vendor/model with provenance; `--json` is what an external workflow harness shells to build its per-role dispatch table), `candidates` (effective tier candidate lists + the work-type routing map with provenance, plan 899), `refresh` (write the `~/.planar/models/catalog.json` cache), `apply` (scaffold the config block).
- **`agents/models.md`** Tier Table + rendered skill/agent `model:` fields — generated from the resolver at `planar skills render`; the render path always renders `list[0]` for a candidate-list tier (static surfaces show the tier default; per-task routing is runtime-only).
- **External workflow harnesses** — shell `planar models routing --json` to build a per-role dispatch table (no engine handle), falling back to compiled defaults when `planar` is unreachable.
- **Orchestrator dispatch (Phase 3)** — the dispatch preview's routed-model column classifies each task's work type and calls `resolve(role, work_type)` to show the routed candidate alongside the tier column; the operator may override either before confirming. The confirmed `{tier, candidate, work_type}` triple persists per task in the dispatch session entry's `model_choice` map (a convention extension, no schema change — see [`agents/orchestrator.md`](../agents/orchestrator.md) step 8a and [`skills/src/pl-orchestrator.md` §Dispatch preview and model tiers](../skills/src/pl-orchestrator.md)).
- **`planar models evals`** — read-only routing evaluation. It mines completed dispatch notes (`dispatch_shape:` / `model_choice:`), terminal claim status, and test-coder action outcomes into a per-`(work_type, candidate)` scorecard plus preview-only recommendations. Quality-gate pass/fail is not persisted today, so the command reports that signal as unsourced rather than guessing. It writes nothing; applying a recommendation is a separate operator-gated config edit.

The provider CLIs (`claude`, `codex`) do not expose a machine-readable model list, so the per-vendor catalog is curated in the binary; discovery confirms which CLIs are installed by invoking `<bin> --version`. See `docs/cli-reference.md` § Domain `config` (Model routing) and § Domain `models`, and the `pl-models-config` skill.

## Color output

`planar tree`, the per-entity `list` verbs (`plan list`, `task list`, `question list`, `decision list`, `artifact list`, `scenario list`), and the child-plan summary section of `plan show` colorize the status column based on the entity kind that owns the status. Status is the only field colorized; titles, IDs, dates, and relationship arrows stay plain.

**Palette families.** The palette uses six visual categories from the 8-color ANSI set (universal across modern terminals: macOS Terminal, iTerm2, kitty, alacritty, Windows Terminal). The map is keyed by `(entity_kind, status)` so the same status text on different entities can take different colors.

| Family | Color | Statuses |
|---|---|---|
| Live in-flight | bold cyan | `plan.active`, `task.doing`, `plan_step.in-progress` |
| Ready / open | default | `task.todo`, `question.open`, `decision.proposed`, `plan.draft`, `artifact.draft`, `scenario.draft`, `annotation.active`, `plan_step.pending` |
| Done / accepted | green | `plan.done`, `task.done`, `question.answered`, `decision.accepted`, `scenario.verified`, `annotation.resolved`, `scenario_outcome.pass`, `artifact.active`, `plan_step.done` |
| Caution | yellow | `plan.paused`, `task.blocked`, `scenario.failing`, `scenario.ready` |
| Hard fail | red | `scenario_outcome.error`, `scenario_outcome.fail`, `question.wontfix` |
| Terminal-dim | gray | `plan.abandoned`, `task.cancelled`, `annotation.dismissed`, `annotation.archived`, `artifact.superseded`, `artifact.retired`, `decision.superseded`, `decision.withdrawn`, `scenario.retired`, `scenario_outcome.skipped`, `plan_step.skipped` |

**Mode precedence (highest wins).**

1. `NO_COLOR` env var (any non-empty value) — never emit color
2. `--no-color` or `--color=never` — never emit color
3. `--color=always` — emit color
4. `--color=auto` (default) and stdout is a TTY — emit color
5. otherwise — do not emit color

**JSON bypass.** The `--json` output path never calls into the color helper. The bypass is structural, not toggle-based: scripts piping JSON receive byte-identical output regardless of `--color` or `NO_COLOR`.

**Drift gate.** A unit test in the output color module walks every domain's `Statuses` constant and asserts a palette entry. A future migration that adds a new status without updating the palette fails the test and the build is red.

**SQLite tables:** none — color is a pure rendering concern. **Primary entry points:** the output color helpers in `src/cmd/planar/output.zig`. Engine-internal, not a CLI surface.

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
model: claude-opus-4-7          # optional explicit model override
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

**Promotion is manual.** No `planar local promote` shortcut. A skill earning a place in the canonical repo means going through the normal git contribution flow: copy the file into `skills/src/`, run `make install` (which invokes `planar skills render` at install time), commit, push, and let `planar skills render --check` (run against an out-of-tree staging dir) plus any remaining relevant validators gate it. The absence of a shortcut is deliberate — canonical and sandbox have different bars.

**SQLite tables:** none — the sandbox is filesystem state. **Primary entry points:** `sandbox.WalkSandbox`, `sandbox.Migrate`, `link.Link`, `link.Unlink`, `link.List`, `importer.Import`. CLI surface: `planar local {list, link, unlink, import, migrate}`; the `pl-local repair` workflow uses `planar local link --reconcile`, not a separate repair verb.

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

**SQLite tables:** none beyond the existing `test_scenarios` and `entity_links`. **Primary entry points:** `ingestor.ParseTestSpec` (parses the body), `workbench.LoadCrossRefs` (reads the cross-reference edges), the `test_spec` artifact kind in `artifacts.kind`.

## Test-coder

The fourth agent role, dispatched between the coder and the reviewer in [Phase 3.5](../agents/methodology.md#phase-35--test-coder-dispatch). Reads the test-spec and the coder's diff; produces a test-only diff that closes uncovered slugs. Never modifies a failing test to make it pass — surfaces failures with a classification (`test-wrong-author-error` / `code-wrong-bug-surfaced` / `ambiguous-operator-decide`).

**Cognitive-split rationale.** A coder writing tests for their own feature has the wrong incentive: make-the-green-test-pass shapes both the feature and the test. A test-coder reading the test-spec and the coder's already-committed feature has no incentive to make tests easy to pass — only to verify the cited scenarios. The two roles enforce different mental models. The load-bearing clause is **tests-may-be-elevating-bugs**: when a new test fails on first run, the test-coder does NOT modify the test, it classifies the failure and reports it. A red test is signal, not noise.

**Position in the loop.** Phase 3.5 fires *only* when (a) the cycle's dispatched tasks carry `[slug: …]` annotations AND (b) `planar test-spec status --json` reports `uncovered_task_slugs` intersecting the cycle's slugs. Slug-less tasks are out of scope by construction. The decision taxonomy is:

- `expanded` — test diff covers cited scenarios; all new tests pass. Orchestrator stages the diff alongside the coder's; reviewer sees the union.
- `no-expansion-needed` — cited scenarios already verified by the coder's diff or pre-existing tests. Reviewer sees the coder's diff alone.
- `failure-surfaced` — at least one new test fails on first run. Orchestrator escalates to the operator with the test-coder's classification; reviewer NOT dispatched until the operator resolves.
- `abort` — cannot satisfy the brief.

**Iteration cap.** The test-coder cycle has its own cap (default 2; the work shape is "expand or don't" rather than "iterate to convergence"). Independent of the coder/reviewer's 5-iteration cap.

**Manual invocation.** Operators can invoke `/pl-test-coder <task-id>` directly to backfill coverage on an already-committed change set, or `/pl-test-coder <plan-id> --plan` to run against every cited scenario in a plan. Useful after authoring a new test-spec for an older feature.

**SQLite tables:** none. **Primary entry points:** [`agents/test-coder.md`](../agents/test-coder.md) (canonical contract), [`skills/src/pl-test-coder.md`](../skills/src/pl-test-coder.md) (unified skill source), `planar test-spec status` (gating verb), `planar spec ingest --strict` (ingest-time gate). Vendor projections are generated at install time.

## Usage Introspection Privacy Model

Planar's usage-introspection loop (capture → report → introspect) applies a two-tier privacy model. The two tiers provide different guarantees and must not be conflated.

### Tier 1: Structurally-redacted diagnostic bundle

`planar report [--json]` is **privacy-safe by query construction**. The aggregate queries in `src/engine/introspect.zig` select only counts, error categories, verb paths, statuses, and timestamps from the observability tables. They never select `title`, `body`, `summary`, scope slugs, file paths, or any column that could carry operator-authored or PII-adjacent text. This guarantee is testable with sentinel fixtures and holds with no human in the loop.

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

**Frontmatter surface.** The workbench `FrontMatter` struct exposes three optional list fields:

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

Each entry is a `"<kind>:<id>"` reference parsed into an `EntityRef` struct. Custom YAML marshalling (`UnmarshalYAML` / `MarshalYAML`) round-trips the string form. Malformed entries (empty kind, non-integer id, etc.) error at parse time so drift surfaces immediately.

**Push and pull.** On `workbench push`, the renderer reads the entity's `entity_links` rows and populates the three frontmatter lists. On `workbench pull`, the parser reads the frontmatter lists and reconciles them against `entity_links` (additive in v1; orphan-edge removal is a follow-up). Same pattern as the `touches:` field used by cross-repo tasks.

**Authoring discipline.** The planner emits frontmatter cross-references when relationships are obvious (test-spec verifies product-spec; tech-spec derives from product-spec). Operators can hand-edit the lists at any time via `<entity> edit <id>`. The new entries become `entity_links` rows on the next pull.

**Why frontmatter, not a body section.** A first design appended a `## Cross-references` Markdown section to artifact bodies. That broke the ingestor — the roadmap parser iterates every `## …` H2 as a milestone, and the injected section shifted milestone counts. Frontmatter avoids the collision. The `workbench.RenderCrossRefsSection` helper exists for callers (e.g. `planar tree` or `planar <entity> view`) that want a human-readable rendering, but the body itself stays clean.

**SQLite tables:** `entity_links` (existing; widened CHECK accepts `verifies` / `cites` / `derives-from` since plan 4). **Primary entry points:** `workbench.LoadCrossRefs(db, kind, id)`, `workbench.RenderCrossRefsSection`, `FrontMatter.Verifies` / `Cites` / `DerivesFrom`.
