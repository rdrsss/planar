# Planar Operations

How a unit of work moves from a goal to merged code through Planar's agents
and the five-binary boundary. This is the operational spine: the moving parts
that the reference docs describe statically, drawn as the flows they actually
run.

Three flows carry most work:

1. **The spec pipeline** turns a goal into a structured task graph
   (draft -> review -> ingest).
2. **The orchestration lifecycle** turns that graph into merged, reviewed code
   across six phases.
3. **The claim ritual** is the atomic coordination primitive every
   code-writing dispatch rides on.

Each section leads with a diagram, then the contract, then pointers into
[`concepts.md`](concepts.md), [`architecture.md`](architecture.md),
[`workflows.md`](workflows.md), and `agents/methodology.md` (the
coordination contract). Nothing here introduces new behavior; it summarizes
the existing contract.

---

## 1. The Spec Pipeline

A new feature is drafted by the **planner**, adversarially reviewed by the
**spec-reviewer**, then decomposed by the **ingestor** into plans, tasks,
decisions, scenarios, questions, and links. Every write is preview-first and
operator-gated; the anchor plan does not become `active` until ingest applies.

```mermaid
flowchart TD
    OP(["Operator states goal"]) --> LT{"Known feature shape?"}
    LT -->|"no, exploring"| LTB["Light-touch capture<br/>1 active plan plus direct<br/>artifact / decision / question / task rows"]
    LTB -.->|"graduates via the planner agent"| DR
    LT -->|"yes"| DR["Planner agent<br/>draft plan + workbench tree<br/>product, tech, roadmap, test specs"]
    DR --> SC["self-check<br/>spec ingest --strict preview"]
    SC --> RV["Spec reviewer agent<br/>adversarial pass"]
    RV --> VD{"Verdict"}
    VD -->|"needs-answers / needs-spec-work"| ED["operator edits workbench<br/>or answers questions"]
    ED --> RV
    VD -->|"abort-replan"| DR
    VD -->|"ready-for-ingest"| IP["Ingestor agent<br/>read-only tree diff"]
    IP --> IG{{"Gate: operator confirms"}}
    IG -->|"--apply"| AP["atomic per anchor plan:<br/>child plans, tasks, decisions,<br/>scenarios, questions, links,<br/>anchor draft -> active"]
    AP --> DONE(["task graph in DB<br/>ready for orchestration"])
```

**Draft.** The `planar-planner` agent creates the anchor plan in `draft`, lays down the
workbench tree, and authors `product-spec.md`, `tech-spec.md`, `roadmap.md`,
and `test-spec.md`. Each file is registered as an artifact and mirrored to the
workbench. The planner also extracts `## Open questions` into first-class
question rows and runs a read-only strict ingest self-check before handing back.

**Review.** The `planar-spec-reviewer` agent runs between planner and ingestor. It checks
intent fit, feature gaps, hazards, open questions, roadmap readiness, and
test coverage. Its verdict is one of `ready-for-ingest`, `needs-answers`,
`needs-spec-work`, or `abort-replan`. It never runs `spec ingest --apply`.

**Ingest.** `planar spec ingest` without `--apply`, which the `planar-ingestor` agent drives, is a read-only preview. It
reads `roadmap.md`, `tech-spec.md`, and `test-spec.md`, computes additions,
updates, proposed removals, slug coverage, and orphan scenarios, then prints a
tree-shaped diff. `--apply` commits atomically per anchor plan: derived rows
and the `draft -> active` flip all commit together or roll back together.

Two annotations are load-bearing:

- Roadmap work-item bullets may carry `[touches: ...]`, whose entries take
  three forms:

  | Entry | Records | Use |
  |-------|---------|-----|
  | `<repo-slug>` | `entity_links(touches, task → repo)` | the task touches the whole repo |
  | `<repo-slug>:<path>` | a `task_touch_paths` row plus the implied repo edge | the task touches one file, repo named explicitly |
  | `<path>` | same, against the anchor association's sole member repo | the common single-repo case |

  **Prefer the path forms.** A whole-repo touch collides with *any* same-repo
  touch under parallel-eligibility rule 2, so a plan whose tasks carry only
  repo edges is no more parallel-eligible than one that declares nothing.
  Path declarations are also the seeds closure extraction reads.

  A bare path resolves only when the anchor's association has exactly one
  member repo; with two there is no principled choice, and the entry is
  reported rather than guessed. An entry naming an unknown repo
  (`typo:src/x.cpp`) is likewise reported, never re-read as a bare path —
  falling back would attach the declaration to the wrong tree while looking
  like it worked. Unresolved entries are counted and warned about at the end
  of `spec ingest --apply`.

  `[slug: ...]` pins the task's stable slug.
- A tech-spec `## Open Questions` item whose first non-blank body line begins
  with the case-sensitive `Resolution:` token is answered during apply. New
  questions are created and answered in the same apply; existing matching
  open questions are flipped to `answered`.

**Light-touch bypass.** When the feature shape is not ready for specs, start
with one `active` plan and direct `artifact`, `decision`, `question`, and
`task add` rows. When the work matures, the `planar-planner` agent can read that captured
material and graduate it into the spec pipeline. See
[`light-touch.md`](light-touch.md).

Detail: [`workflows.md` Recipe 1](workflows.md#recipe-1--start-a-new-feature),
[Plan](concepts.md#plan), [Scenario](concepts.md#scenario), and
[Artifact](concepts.md#artifact).

---

## 2. The Orchestration Lifecycle

The orchestrator is the top-level dispatcher. It selects phases from the
anchor plan's status and drives a feature through planning, ingestion,
execution, optional finalization, and optional propagation/archive.
Planning and ingestion are hard-gated;
execution runs a reviewer loop capped at five iterations.

The orchestrator, coder, reviewer, test-coder, and janitor roles that drive
this lifecycle live in this repo's `agents/`, alongside their companion
methodology, doctrine, and model-tier-routing docs. Planar itself
drives the primitive these roles compose: the `planar-agent` claim ritual
(§3 below).

```mermaid
flowchart TD
    S(["Goal / anchor plan"]) --> SEL{"Phase selection<br/>by plan.status"}
    SEL -->|"draft, no artifacts"| P1["Phase 1 Planning<br/>planner drafts 4 specs"]
    SEL -->|"draft + artifacts"| P2["Phase 2 Ingestion<br/>ingestor preview"]
    SEL -->|"active / paused"| EXE
    SEL -->|"done"| OPT
    P1 --> G1{{"Gate: user reviews artifacts"}}
    G1 --> P2
    P2 --> G2{{"Gate: confirm diff"}}
    G2 -->|"--apply -> active"| EXE
    subgraph EXE["Phase 3 Execution"]
      direction TB
      SG{{"Gate 1: strategy + isolation<br/>dispatch preview: tier + routed model"}} --> SH{{"Gate 2: dispatch shape"}}
      SH --> PULL["planar-agent pull / claim"]
      PULL --> COD["spawn coder<br/>fresh context + heartbeat"]
      COD --> TC{"Phase 3.5<br/>test-coder needed?"}
      TC -->|"failure-surfaced / abort"| ESC["escalate"]
      TC -->|"covered / expanded"| REV["spawn reviewer<br/>blind-read brief"]
      REV --> V{"Verdict"}
      V -->|"request-changes, iter < 5"| COD
      V -->|"cap reached"| ABT["forced abort"]
      V -->|"open-question"| BLK["block + pause"]
      V -->|"abort"| ABT
      V -->|"approve"| CMP["complete"]
    end
    CMP --> DN{"all cycles terminal?"}
    DN -->|"yes + --finalize"| P37["Phase 3.7 Finalization<br/>janitor merge -> reconcile -> closeout"]
    DN -->|"yes"| OPT["Phase 4 / 5<br/>propagate / archive when requested"]
    P37 --> OPT
    OPT --> END(["done"])
```

**Phases 1-2** are the spec pipeline above. The orchestrator surfaces drafted
artifacts and ingest diffs; it never auto-advances those gates.

**Phase 3** has two operator gates. Gate 1 recommends a strategy (`classic`,
`barrel-deferred`, `barrel-bypass`, or `parallel-fanout`) and isolation mode
(`pwd` or `worktree` where applicable), then shows the dispatch preview. The
preview includes each task's model tier and routed-model candidate. Gate 2
selects the dispatch shape nested under that strategy.

**Phase 3.5** runs after coder output when the cycle's task slugs intersect
`planar test-spec status <plan> --json` uncovered slugs. This fires across all
dispatch shapes; `barrel-bypass` skips the reviewer, not the coverage gate. A
`failure-surfaced` or `abort` test-coder result halts the cycle and escalates.

**Reviewer loop.** A reviewer returns exactly one verdict:

| Verdict | Terminal path | Effect |
|---|---|---|
| `approve` | `planar-agent complete` | task -> done; next cycle |
| `request-changes` before iteration 5 | none | same claim stays live; coder respawns |
| `open-question` | `planar-agent block` | task -> blocked; pause and surface |
| `abort` | `planar-agent fail` | task -> todo; halt and escalate |

At iteration 5, `request-changes` is invalid and routes to forced abort.
Under `barrel-bypass` there is no reviewer, so that cap does not apply.

**Phase 3.7** is explicit (`--finalize` or interactive confirmation). The
janitor, not the coder and not the orchestrator directly, runs merge
verification, Planar reconciliation, branch/worktree cleanup, and
`planar plan closeout`.

**Phases 4-5** are explicit: `--propagate` creates external counterparts and
`--archive` archives the workbench tree. The database retains the feature.

Detail: [`workflows.md` Recipe 2](workflows.md#recipe-2--run-the-orchestrator)
and `agents/methodology.md`.

---

## 3. The Claim Ritual

Every code-writing dispatch acquires a lease on `planar-agent`, heartbeats to
keep it live, and ends with exactly one atomic terminal verb. The terminal
verbs update claim state, task state, action state, and plan roll-up in one
transaction. Agents and skills must never split `planar task done` and
`planar-agent release`.

```mermaid
flowchart TD
    A["peek / plan next<br/>dry-run selector"] --> B["planar-agent pull or claim<br/>claim active + task todo -> doing"]
    B -->|"no eligible task"| Z["no_work<br/>writes nothing"]
    B -->|"claim_token"| C["spawn coder<br/>fresh context"]
    C --> D{"heartbeat<br/>at TTL/2 cadence"}
    D -->|"lease valid"| D
    D -->|"lease expired"| X["ClaimNotActive<br/>heartbeat + terminal refused"]
    D -->|"work done"| E["coder returns<br/>no terminal verb"]
    E --> F{"reviewer / caller verdict"}
    F -->|"request-changes"| C
    F -->|"terminal verdict"| G{"one terminal verb"}
    G -->|"complete"| H["task done<br/>claim completed"]
    G -->|"fail"| I["task todo<br/>claim aborted + category"]
    G -->|"release"| J["task todo<br/>claim released"]
    G -->|"block"| K["task blocked<br/>claim released"]
    X -.->|"self-recovery"| R["claim task:id --no-transition<br/>then complete"]
    R --> H
```

**Acquire.** `planar-agent pull <plan>` picks the next eligible task and, in a
single transaction, opens an active claim, flips `todo -> doing`, and writes
the top-level action row. For hand-picked work, `planar-agent claim --entity
task:<id>` claims a specific task; by default it also performs the
`todo -> doing` transition. `--no-transition` keeps the primitive claim-only
behavior.

**Heartbeat.** The holder calls `planar-agent heartbeat --claim <token>` at
least every TTL/2. Once the lease expires, heartbeat and terminal verbs are
refused with `ClaimNotActive`; a dead lease is not revived by heartbeat.

**Terminal ownership.** Under orchestrated dispatch, the coder heartbeats and
returns without firing a terminal verb; the orchestrator owns exactly one of
`complete`, `fail`, `release`, or `block`. Under direct-claim or
barrel-bypass-style caller-owned dispatch, the caller owns the terminal verb.

**Recovery.** If a holder loses a lease but the task is still `doing`, it can
self-recover with `planar-agent claim --entity task:<id> --no-transition`, then
complete. Operator recovery uses `planar-agent reconcile --dry-run` first, then
`reconcile`, `abort --claim <token>`, or force takeover as appropriate. Recovery
never mutates a live, unexpired claim.

Detail: [`concepts.md` §Claim-owned task state and recovery](concepts.md#claim-owned-task-state-and-recovery)
and `agents/methodology.md` § Coordination claims.

---

## 4. Model Routing

Model choice is config-driven. `[models.<vendor>]` maps tiers to scalar model
ids or ordered candidate lists, `[routing.<vendor>.<tier>]` maps work types
(`schema`, `engine`, `architectural`, `cli`, `feature`, `mechanical`) to a
candidate inside the tier, `[roles]` maps agent roles to tiers, and
`[role_vendors]` can route a role to a non-default vendor.

The orchestrator's Phase 3 preview classifies each task's work type and calls
the shared resolver to show both the tier and routed candidate. The operator
may override either before confirming. The confirmed `{tier, candidate,
work_type}` triple is persisted in the cycle's dispatch note as
`model_choice`.

`planar models evals` later mines those dispatch notes, terminal claim status,
and test-coder action outcomes into a per-`(work_type, candidate)` scorecard.
It is read-only: recommendations are previews, not config writes.

Detail: [`concepts.md` §Model routing](concepts.md#model-routing),
and
[`cli-reference.md` §Domain: models](cli-reference.md#domain-models); each
verb's `--help` carries the flags, examples and exit codes.

---

## 5. The Host Build and Test Queue

Builds and tests on one machine go through a single queue, so agents in
different projects do not compile or run suites at the same time. The queue is
`planar-agent queue`; it has no daemon, because the process that submits a
command is the process that runs it. The states an entry moves through are the
diagram in [`lifecycles.md` §3.6](lifecycles.md#36-host-queue-entry), the verbs
and exit codes are in [`cli-reference.md` §Queue verbs](cli-reference.md#queue-verbs),
and the rule agents follow is printed by `planar-agent queue rule`.

**Where it lives.** The queue's state is three tables in `planar.db`
(`queue_entries`, `queue_history` and `queue_schema`), in the file `PLANAR_DB`
names, else `~/.planar/planar.db`. There is no second database. Queue verbs
never create or migrate `planar.db`: run `planar init` first. A queue verb
refuses a `planar.db` that is behind its binary (exit 125, remedy `planar
init`) and keeps working against one that is ahead as long as the queue's own
compatibility marker admits the binary, so agents keep queueing builds and tests
while a newer build migrates the shared file. Claims keep the exact-version
rule, so `queue run --claim` renewals fail against an ahead database; during a
migration cycle submit with the binary that migrated the file. The details are
in [`architecture.md` §The host queue's tables](architecture.md#the-host-queues-tables)
and [`cli-reference.md` §Queue verbs](cli-reference.md#queue-verbs). Output of a
detached run (`queue run --detach`) is written to `<seq>.log` in `queue-logs/`
beside the database (`<stem>.queue-logs/` for a database file not named
`planar.db`), one file per entry, and is deleted with the history row that
names it. Sequence numbers start above 1,000,000. Rolling the queue back or
restoring a backup rewinds that counter; see the recipe in
[`migrations/README.md` §Host-queue rollback recovery](../migrations/README.md).

**Settings.** The `[queue]` table of `~/.planar/config.toml` sets `slots` (how
many commands run at once, default 1), `poll_interval`, `stale_after`, `grace`
(SIGTERM to SIGKILL) and `history_days` (how long history and logs are kept,
default 30). Ranges and units are in
[`cli-reference.md`](cli-reference.md#the-queue-table). The file is read at each
poll, so an edit applies to waiting submitters without a restart, and
`planar config validate` refuses a bad value. A command's run limit is 30
minutes unless its submitter passes `--timeout`.

**Watching.** `planar-watch queue` lists every running and waiting entry, in
order, with its place in line, how long it has waited and run, who submitted it
and whether it is still live. An entry marked `NOT-LIVE` has lost its submitter
and is reaped by the next submitter's poll; the viewer never changes anything.
`planar-watch queue history [--since <duration>]` lists entries that have
ended, with the outcome, exit code or signal, and who cancelled or replaced
them. Both take `--json`. One entry's full record is `planar-agent queue status
<seq>`.

**Waiting for a detached gate.** Submit with `planar-agent queue run --detach
--timeout 2h --vendor <vendor> --role <role> --claim <token> -- <command>` and
save the first output line, its ticket sequence. The command's run limit here
is two hours after it starts; `--claim` lets the submitter renew a
caller-supervised task claim while it waits and runs. Then observe that ticket:

```sh
planar-agent queue wait <seq> --timeout 3h --json
```

The three-hour observation budget can cover an expected hour of backlog plus
two hours of runtime. The separate `queue run --wait-timeout`, if supplied,
limits the entry's time waiting for a slot. The observer's default is `30m`
and an explicit positive budget may be at most `24h`; it does not change
either submitted limit or renew the claim. The wait result's `wait_reason`
describes **why observation stopped**, while `status.outcome` describes a
**recorded job end**. Read both before judging the gate: `completed` with
`exited` and exit code 0 is a pass; other completed outcomes need their
recorded code or signal handled as a gate result. Exit 124 or 125 alone is
ambiguous because the child can return either code itself. The log path in
`status.log_path` is for output; a blank log does not prove completion. See
[`queue wait` in the CLI reference](cli-reference.md#waiting-for-a-logical-ticket-queue-wait)
for every outcome and error code.

If a harness needs shorter turns, use a finite slice such as `queue wait
<seq> --timeout 10m --json`, do independent work, and wait again on the **same
ticket**. `timed_out` and `interrupted` stop only the observer; the detached
command may still be queued or running. `stalled`, missing history, and read
errors also give no completion verdict. Inspect the ticket and queue health
before deciding what to do. Never resubmit or start a conflicting build just
because observation stopped, and never run a command directly after the queue
refuses it. Avoid an outer loop with no finite bound: each wait slice must
return control for an explicit decision.

**Cancelling.** `planar-agent queue cancel <seq>` removes a waiting entry, or
stops a running one (SIGTERM to its command's process group, SIGKILL after the
grace period) and returns when the group is empty. Any caller that can open the
store may cancel any entry; the canceller's vendor, role and process id are
recorded and shown in `queue status` and in history. Pass `--vendor` and
`--role` so the record names who acted.

**Uptake.** The queue only helps if agents use it. Every `queue run` records
the submitter's `--vendor` and `--role` (or `PLANAR_VENDOR` and `PLANAR_ROLE`),
so `planar-watch queue history --since 7d --json` shows which vendors and roles
queue their builds, how long they wait, how often a command times out and how
often entries end `abandoned`. An entry with no vendor or role was submitted by
an agent or script that did not say who it is.

**Preservation and file modes.** `install.sh --uninstall` keeps every data path, `planar.db`
(with its `-wal` and `-shm`) and `queue-logs/` among them, and `--force` does not
change that; the full list is in
[INSTALL.md § Preserved paths](../INSTALL.md#preserved-paths). Remove the data by
hand when you want it gone. (`install.sh --prebuilt` moves the retired queue database and its old logs into `~/.planar/retired/<date>/` instead of removing them; see [INSTALL.md § Prebuilt install](../INSTALL.md#prebuilt-install---prebuilt).) A live entry stores the submitter's task
claim token in the clear (`queue_entries.claim_token`, written when `queue run`
is given `--claim`; the history row does not keep it), and a claim token
authorises heartbeats and terminal verbs on that claim. `planar.db` holds claim
tokens too. So the files are owner-only:

- `~/.planar` (`PLANAR_HOME`) is `0700`. `install.sh` makes it so, and tightens
  an existing install in place: `planar.db` and its sidecars become `0600`.
- `queue-logs/` is created `0700` and its logs `0600`. A log directory that
  already exists must be owned by you and not writable by group or others, or a
  detached run refuses.
- A `PLANAR_DB` outside the install root is yours: `install.sh` changes the
  modes of the database directly under the install root only. It does create
  a missing database, or migrate a behind one, at the resolved path (`PLANAR_DB`
  when set) with the installed `planar init --skip-project --allow-no-repo`,
  and refuses, changing nothing, a database ahead of the release it installs
  ([INSTALL.md](../INSTALL.md#ownership-recovery-and-the-order-of-an-install)).
- An install root shared by several users (a `--prefix` such as `/opt/planar`
  that more than one account runs from) is unsupported under the `0700` rule:
  only the owner can open the database. Run one install per user.
- If `install.sh` cannot change a mode (a file you can write but do not own),
  it prints a warning and carries on; fix the mode by hand.

**When something looks wrong.** A `waiting` entry that never advances and shows
`NOT-LIVE` is an orphan; poll again, and a later submitter reaps it. A command
past its run limit with no submitter polling is stopped by the next poll or by
`queue cancel`. Both are accepted limits, listed with the others in
[`cli-reference.md` §Known limits](cli-reference.md#known-limits). Exit 125 from
`queue run` before it issues a ticket means the queue refused; agents stop and
report it and never run the command directly. Exit 125 from `queue wait` may
instead be a recorded child exit, cancellation, wait timeout, stalled
observation or observer error; inspect `wait_reason` and `status.outcome`.

On an older queue-capable install without `queue wait`, discover support once
from the compact schema or leaf help. Short commands, or scripts that can wait
for the whole job, may use foreground `queue run`. For a long command under an
agent harness, keep the detached ticket and use a finite, error-checking JSON
`queue status` observer with a fixed overall deadline and bounded status
subprocesses. Treat `state: ended` only through its recorded `outcome`; an
abandoned row without a successor and missing successor history remain
uncertain. On timeout or interruption, stop only the observer and keep the
ticket for a later check. The fallback must clean up only its own status
helpers, never the queued job, submitter, log or claim. Queue present but
refusing is not an absent-queue fallback.

---

## See Also

- [Architecture](architecture.md) — storage model, schema contract, binary
  boundary, workbench, adapters.
- [Concepts](concepts.md) — the entity and gate mental model.
- [Workflows](workflows.md) — step-by-step operator recipes.
- `agents/methodology.md` — authoritative
  agent-coordination contract.
