---
name: planar
description: >-
  Planar is local agent-operations infrastructure: plans, tasks, work claims, decisions,
  specs, handoffs and Jira or GitHub Issues sync, kept in one SQLite database behind the
  planar, planar-agent, planar-watch, planar-ext and planar-execute CLIs. Use this skill
  whenever a session plans, claims, implements, reviews, hands off or syncs work through
  those CLIs.
license: MIT
metadata:
  version: "0.1.0"
  repository: https://github.com/rdrsss/planar
---

# Planar

## What Planar is and who writes what

Planar keeps planning and agent-coordination state in one SQLite database and exposes it
through five binaries. Each binary has a disjoint write surface. The boundary is each
binary's verb set, not a runtime ACL, so choosing the right binary is the agent's job.

| Binary | Role | Writes |
|--------|------|--------|
| `planar` | Operator surface | Planning entities, and manual `tasks.status` transitions. |
| `planar-agent` | Agent-callable | `agent_actions`, `agent_work_claims`, `routing_dispatch_*`, `queue_*`, and `tasks.status` inside a coordinated operation. |
| `planar-watch` | Read-only viewer | Nothing. Opens the database read-only. |
| `planar-ext` | Jira and GitHub Issues | `external_links`, `external_systems`, `sync_events`. |
| `planar-execute` | Workflow entry point | Nothing. No database handle; shells the other CLIs. |

This skill teaches only the rules that cross verbs. Everything verb-shaped (flags,
arguments, output fields, exit codes) comes from the binaries; see the discovery rule.

## Invariants

1. **Claim, heartbeat, one terminal verb.** Take work with `planar-agent pull` or
   `planar-agent claim`, renew it with `planar-agent heartbeat` at half the TTL, and end it
   with exactly one of `planar-agent complete`, `fail`, `release` or `block`. Each terminal
   verb flips the claim and `tasks.status` in one transaction.
2. **Never split the terminal verb.** Do not run `planar task done` and then
   `planar-agent release`: a process death between the two strands the claim. Use the
   single atomic `planar-agent` terminal verb. In orchestrated work the orchestrator owns
   it; a dispatched coder heartbeats and returns.
3. **Write through the owning binary.** Planning entities go through `planar`, claims and
   agent actions through `planar-agent`, external links through `planar-ext`; `planar-watch`
   and `planar-execute` never write. The table above is the boundary; it is enforced by
   each binary's verb set, not by a runtime ACL, so nothing stops a wrong-binary write.
4. **Cross-scope guard: ten verbs.** Only ten verbs compare scopes (among them
   `spec ingest --apply`, `task update`, `decision accept` and `planar-ext sync push`).
   A mismatch exits 5 with no bypass flag. `assoc:<org>` covers an entity at
   `repo:<member>`; the reverse refuses. Never claim the guard covers every write.
5. **`--scope` means four things.** It selects the write scope on write verbs; it is a
   patch field that moves the row on `plan update`, `artifact update` and
   `annotate update`; it is a read filter on `list`, `search`, `tree`, `dashboard` and
   `health`; and it is inert where `--help` says it is accepted but not read.
6. **Read back every mutation.** Exit 0 shows a command ran, not that the state exists.
   After a write, read the entity back with the matching `show`/`list` verb and report
   identifiers from that read; if no read is available, say so as a warning.
7. **Planning verbs refuse in worktrees.** From inside a git worktree, planning-class
   writes (`plan create`, `task add`, `spec ingest`, `decision add` and the like) exit 8
   before parsing. `--scope` does not bypass it; run them from the parent checkout.
   `planar-agent` verbs and every read work from a worktree.
8. **Next work is claim-aware.** Pick work with `planar plan next` or `planar-agent peek`,
   which skip tasks under a live claim. Never derive it from `planar task list` filtered
   to `todo`: a `todo` task can already be claimed by another vendor.
9. **Plan status follows its tasks.** Every task write and terminal verb recomputes the
   owning plan's status; do not walk plans through `draft`, `active` and `done` by hand.
   An anchor plan never auto-completes: its `done` is an operator release decision.
10. **Operator gates are never automatic.** `planar spec ingest` with `--apply`,
    `planar plan closeout` and `planar workbench archive` each run only after the operator
    confirms that invocation. Preview first, show the diff, then wait.
11. **Sync pull writes no planning entity.** `planar-ext sync pull` emits `remote_title`
    and `remote_status` and changes no plan or task. The agent verifies those values and
    writes any change through `planar`.
12. **Announce cross-scope writes exactly.** Before a write whose target is outside the
    scope `planar scope show` reports, emit this standalone line just before the command:
    `[cross-scope write: <normalized-target-label>]`, with a label such as
    `project:planar`, `association:org:acme` or `global`. The cue grants nothing and
    bypasses no guard. Same-scope writes carry no cue.
13. **Isolate from-source binaries.** The runtime opens `$PLANAR_DB`, falls back to
    `~/.planar/planar.db`, and applies pending migrations on open. Never run `./bin/*` or
    `build/*/bin/*` against the real database; set `PLANAR_DB` and `HOME` to scratch paths
    first. Use the `planar` on `$PATH` to exercise documented behaviour.

## Discovery rule

Before using any verb, read its help. Every flag has a help line, and every leaf verb ends
with `Examples:` and `Exit codes:` sections. For machine-readable detail, ask the binary's
schema catalog for one command, or for the compact tree:

```sh
planar task update --help
planar schema --command "task update"
planar schema --compact
```

The same works on `planar-agent`, `planar-watch`, `planar-ext` and `planar-execute`.
Never load the full `schema` catalog; for `planar` it is about 338 KB.

## Routing

| Intent | Go to |
|--------|-------|
| Claim, heartbeat and finish a unit of work | `references/claim-ritual.md` |
| See what needs attention, in what order | `references/status.md` |
| Degraded health, stale claims, doctor | `references/recovery.md` |
| Capture a handoff, validate a resume, cd into a worktree | `references/resume-handoff.md` |
| Draft, review and ingest specs; workbench; closeout | `references/spec-pipeline.md` |
| Decisions, artifacts, annotations, questions, link endpoints | `references/knowledge.md` |
| Jira/GitHub: register, create, sync, resolve, propagate | `references/external-sync.md` |
| Report an issue from a feedback finding | `references/external-sync.md` |
| Operator-local skills and agents; workspace scan | `references/local.md` |

Report every result with the seven-section envelope in `references/feedback-contract.md`.

Role work goes to agents, dispatched by name and never invoked as skills:
`planar-orchestrator`, `planar-coder`, `planar-reviewer`, `planar-test-coder`, `planar-janitor`,
`planar-research`, `planar-planner`, `planar-spec-reviewer`, `planar-ingestor`, `planar-importer`,
`planar-synthesizer`, `planar-ext-sync`, `planar-feedback-triager`, `planar-introspector`
and `planar-sync-reconciler`. This is the only Planar skill; no slash-command skill exists
for any of these roles.
