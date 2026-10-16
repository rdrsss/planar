---
name: planar
description: >-
  Planar is local agent-operations infrastructure: plans, tasks, work claims, decisions,
  specs, handoffs and Jira or GitHub Issues sync, kept in one SQLite database behind the
  planar, planar-agent, planar-watch and planar-ext CLIs. Use this skill when the
  operator mentions Planar, a Planar plan or task id, or one of those CLIs.
license: MIT
metadata:
  version: "0.1.0"
  repository: https://github.com/rdrsss/planar
---

# Planar

Planar keeps planning and agent-coordination state in one SQLite database behind four
binaries with disjoint write surfaces. The boundary is each binary's verb set, not a
runtime ACL, so the caller picks the right binary.

| Binary | Role | Writes |
|--------|------|--------|
| `planar` | Operator surface | Planning entities, and manual `tasks.status` transitions. |
| `planar-agent` | Agent-callable | `agent_actions`, `agent_work_claims`, `queue_*`, and `tasks.status` inside a coordinated operation. |
| `planar-watch` | Read-only viewer | Nothing. |
| `planar-ext` | Jira and GitHub Issues | `external_links`, `external_systems`, `sync_events`. |

Start with `planar-agent brief <plan-id>`. It is read-only and returns, for one plan, the
task counts and milestones, live and stale claims, the next parallel wave, per-task
readiness, the effective policy and its source (including `confirm_before`), and command
templates for claiming, heartbeating and the terminal verbs. `--json` is for programs.

## Invariants

1. **Claim, heartbeat, one terminal verb.** Take work with `planar-agent pull` or
   `planar-agent claim`, renew it with `planar-agent heartbeat` before the lease runs out,
   and end it with exactly one of `planar-agent complete`, `fail`, `release` or `block`.
   Each flips the claim and `tasks.status` in one transaction. `planar task done` followed
   by `planar-agent release` splits that, and a death between them strands the claim.
2. **Choose next work with the claim-aware verbs.** `planar plan next` and
   `planar-agent peek` skip tasks under a live claim; a `todo` filter on
   `planar task list` does not.
3. **Write through the owning binary.** Planning entities through `planar`, claims and
   agent actions through `planar-agent`, external links through `planar-ext`.
4. **Cross-scope guard.** Only `spec ingest --apply`, `task update`, `decision accept`,
   `decision withdraw`, and `planar-ext sync push`, `sync pull` and `sync resolve` compare
   scopes. A mismatch exits 5 and no flag bypasses it. `assoc:<org>` covers an entity at
   `repo:<member>`; the reverse refuses. On `plan update`, `artifact update` and
   `annotate update`, `--scope` moves the entity; on reads it filters.
5. **Planning writes refuse in a git worktree** (exit 8): `plan create`, `task add`,
   `spec ingest` and the like. Run them from the parent checkout.
6. **Task writes and terminal verbs recompute the owning plan's status**; an anchor plan
   never completes by itself. `planar-ext sync pull` writes no planning entity; it emits
   `remote_title` and `remote_status` for a write through `planar`.
7. **Isolate from-source binaries.** The runtime opens `$PLANAR_DB`, falls back to
   `~/.planar/planar.db`, and applies pending migrations on open. Set `PLANAR_DB` and
   `HOME` to scratch paths before running any binary built from source.

## Discovery

Read a verb's help before using it; each leaf verb ends with `Examples:` and
`Exit codes:`. For machine-readable detail use `planar schema --command "task update"`,
or `planar schema --compact` for the command tree. All four binaries have `schema`.
Avoid the full catalog; it is about 338 KB for `planar`.

## References

| Topic | File |
|-------|------|
| Dispatching work to workers: tokens, waves, reviews | `references/dispatch.md` |
| Preparing plans: work lists, decisions, questions, scenarios, ingest | `references/preparation.md` |
| The host build and test queue | `references/queue.md` |
| Claim, heartbeat and terminal verbs in detail | `references/claim-ritual.md` |
| Health, stale claims, doctor | `references/recovery.md` |
| Handoff, resume, worktree paths | `references/resume-handoff.md` |
| Jira and GitHub: register, create, sync, resolve, propagate | `references/external-sync.md` |
| Dashboards, claims and activity | `references/status.md` |
| Decisions, artifacts, annotations, questions, links | `references/knowledge.md` |
| Local skills and agents; workspaces | `references/local.md` |
