# Planar Skill and Agent Reference

Planar ships one skill, `planar`, in the [Agent Skills](https://agentskills.io) layout, and
a set of example role agents named `planar-<role>`. The CLI is the contract: the skill is a
short reference for what Planar does and how to call it, and everything verb-shaped comes
from the binaries. It prescribes no workflow. The first step, with or without the skill, is
`planar --help`.

If you want Planar as a capture journal with no agent workflow at all, read
[light-touch.md](light-touch.md): it uses four `planar` verbs and no skill, and it starts
from `planar --help` too.

## Start with `planar --help`

Every binary documents itself. Each leaf verb's help ends with `Examples:` and
`Exit codes:` sections, and each binary has a `schema` catalog for machine-readable detail.

```sh
planar --help
planar task update --help
planar schema --command "task update"
planar schema --compact
```

The same works on `planar-agent`, `planar-watch` and `planar-ext`. Ask the
catalog for one command or for the compact tree, never for the whole catalog: for `planar`
it is about 338 KB. Every verb, flag and exit code is in [cli-reference.md](cli-reference.md).

## Using it day to day

There is no command to type. The harness loads the `planar` skill when a request mentions
Planar, a Planar plan or task id, or one of its CLIs (`planar`, `planar-agent`,
`planar-watch`, `planar-ext`). A request that never names Planar does not load it. The
skill tells the session which reference to read.

```text
Draft a Planar spec for adding multi-currency checkout.
Show me the Planar ingest preview for plan 1234.
Claim Planar task 5678 for this session.
What needs my attention in Planar?
Hand off Planar task 5678; I'm stopping for today.
Resume Planar task 5678.
Sync Planar plan 1234 to GitHub Issues.
```

### Asking for the skill or an agent by name

- **The skill.** In Claude Code, `/planar <request>` loads the skill explicitly. In the
  other harnesses, say "use the planar skill" in the request.
- **An agent.** Name it in the request: "use the planar-planner agent to draft a spec for
  …", "have planar-reviewer review the diff on this branch". Agents are dispatched as
  subagents, never typed as commands.

### Confirmations

The skill requires no confirmation step of its own. Which verbs need the operator's yes
before a session runs them is the operator's policy: the `confirm_before` key in
`config.toml` lists verb paths, and `planar-agent brief <plan-id>` prints the list so a
session sees it. Planar displays the list and does not enforce it. See
[cli-reference.md](cli-reference.md) for the policy keys. Nothing reaches Jira or GitHub
Issues unless a `planar-ext` verb runs; `planar-ext ext propagate --dry-run` previews
without contacting the remote system.

### Coming from the `pl-*` skills

The forty `pl-*` skills were retired in favour of the one skill and the agents. Each one
maps to a request, an agent or a command:

| Retired skill | Now |
|---------------|-----|
| `pl-spec-draft` | "Draft a Planar spec for …": the example `planar-planner` agent |
| `pl-spec-review` | "Review the Planar spec for plan N": the example `planar-spec-reviewer` agent |
| `pl-spec-ingest` | "Preview the ingest for plan N": `planar spec ingest N --strict --json`, then `--apply` |
| `pl-orchestrator` | the example `planar-orchestrator` agent; `references/dispatch.md` describes the claims it records |
| `pl-coder`, `pl-test-coder`, `pl-reviewer` | the example `planar-coder`, `planar-test-coder` and `planar-reviewer` agents |
| `pl-research` | the example `planar-research` agent |
| `pl-status`, `pl-observe` | "What needs attention in Planar?": `references/status.md`, over `planar dashboard`, `planar-watch ps` and `planar-watch plans` |
| `pl-health`, `pl-doctor` | "Planar health is degraded": `references/recovery.md`, over `planar health` and `planar-agent reconcile`; there is no `doctor` verb |
| `pl-handoff`, `pl-resume` | "Hand off Planar task N" or "Resume Planar task N": `references/resume-handoff.md`, over `planar handoff create` and `planar resume` |
| `pl-plan`, `pl-task`, `pl-question`, `pl-scenario` | ask for the change, or run `planar plan`, `planar task`, `planar question` or `planar scenario` directly |
| `pl-knowledge`, `pl-promote` | `references/knowledge.md`, over `planar decision`, `planar artifact`, `planar annotate`, `planar links`, `planar promote` and `planar demote` |
| `pl-workbench`, `pl-workbench-sync`, `pl-workbench-archive` | `references/preparation.md`, over `planar workbench` |
| `pl-ext-create`, `pl-ext-propagate`, `pl-sync`, `pl-audit-trail`, `pl-templates` | `references/external-sync.md` and the example `planar-ext-sync` agent, over `planar-ext`, `planar audit trail` and `planar templates` |
| `pl-local`, `pl-local-import`, `pl-workspace-scan` | `references/local.md`, over `planar local` and `planar workspace` |
| `pl-init`, `pl-scope` | `planar init` and `planar scope show` |
| `pl-help` | `planar --help`, `planar help` and `planar schema --command "<verb>"` |

A request that names a retired skill still works: the session reads the intent, not the
name.

## What installs where

`install.sh` stages the skill under `$PLANAR_HOME` (default `~/.planar/`), then places it
into each vendor harness whose presence marker exists. Six vendors read three skill
targets. A vendor without its marker is skipped and named in the installer's summary;
`--vendors` narrows the set further. No role agent is staged or placed: the
`planar-<role>` agents are examples under `examples/agents/` in the source tree, which you
can copy into a harness's agent directory yourself.

### Vendor targets

| Vendor | Presence marker | Skill |
|--------|-----------------|-------|
| Claude Code | `~/.claude/` | `~/.claude/skills/planar` |
| Codex | `$CODEX_HOME` set, else `~/.codex/` | `~/.agents/skills/planar` (shared) |
| Copilot | `~/.copilot/` | `~/.agents/skills/planar` (shared) |
| Gemini CLI | `~/.gemini/settings.json` | `~/.agents/skills/planar` (shared) |
| Antigravity | `~/.gemini/antigravity-cli/` | `~/.gemini/antigravity-cli/skills/planar` |
| OpenCode | `~/.config/opencode/` | none of its own |

The three targets are `~/.claude/skills`, the shared `~/.agents/skills` and Antigravity's
private `~/.gemini/antigravity-cli/skills`.

- **The shared skill root.** `~/.agents/skills/planar` is placed once when any of Codex,
  Copilot, Gemini CLI or OpenCode is present, however many of them are. The install
  manifest records it under the owner `shared`, because no single vendor owns it.
- **Antigravity** has a private skill root and is detected independently of Gemini CLI.
- **OpenCode** gets no private skill copy. It reads the shared `~/.agents/skills` root and
  `~/.claude/skills`, and Planar never writes `~/.config/opencode/skills`.
- **Role agents an older install placed.** An upgrade removes the `planar-<role>` agents an
  older install placed in the six vendor agent directories, when it can prove Planar placed
  them, and never touches a `planar-local-*` agent. See
  [INSTALL.md § Upgrade note](../INSTALL.md#upgrade-note-the-previous-skill-and-agent-projections).

### Link and copy mode

By default `install.sh` copies: each skill target is a real directory, so the source
checkout can be deleted afterwards. With `--link`, the skill directories are symlinks into
`$PLANAR_HOME`, and source edits show up without reinstalling. A destination that exists and that no
Planar install manifest records stops the install, naming the path. See
[INSTALL.md § Copy mode vs link mode](../INSTALL.md#copy-mode-vs-link-mode).

### Drift, upgrade and uninstall

- **Drift.** `$PLANAR_HOME/install-manifest.json` is the only ownership record.
  `planar health` compares every recorded target across the vendor roots with the staged
  authority and reports it `fresh`, `stale` or `missing`; other entries in those roots are
  `unmanaged` and never degrade health. A degraded result carries the reinstall command.
- **Upgrade.** A reinstall over an older install removes the previous per-vendor skill and
  command projections that it can prove Planar made, prints each removal, and leaves and
  reports anything it cannot prove. See
  [INSTALL.md § Upgrade note](../INSTALL.md#upgrade-note-the-previous-skill-and-agent-projections).
- **Uninstall.** `planar-uninstall` (`$PLANAR_HOME/bin/planar-uninstall`, also run by
  `./install.sh --uninstall`) reads the manifest without `python3` and removes every
  target it records while it still holds what Planar placed, then the managed subtrees
  and the install records under `$PLANAR_HOME`. It keeps and names the preserved data
  paths (`planar.db` and its sidecars, `queue-logs/`, `workbench/`, `config.toml`,
  `local/`, `templates/` and the rest of the
  [INSTALL.md § Preserved paths](../INSTALL.md#preserved-paths) list) and reports every
  entry it does not know; `--purge` removes the data paths too, except a relocated one.
  Without a version 2 manifest it removes nothing under the vendor directories, and it
  never removes a vendor directory or `~/.agents/skills` itself. See
  [INSTALL.md § Uninstall](../INSTALL.md#uninstall).

## The `planar` skill

The source is [`skills/planar/`](../skills/planar/SKILL.md): one `SKILL.md` of about 80
lines and ten references. The installed copy lives in the skill roots above and in
`$PLANAR_HOME/skills/planar/`.

### The parts of `SKILL.md`

1. **Write surface.** The binaries and what each one writes. The boundary is each binary's
   verb set, not a runtime ACL, so choosing the binary is the caller's job.
2. **Start here.** `planar-agent brief <plan-id>`, one read-only call that returns the
   plan's state, claims, the next parallel wave, readiness, the effective policy and
   command templates.
3. **Invariants.** Seven rules that hold across verbs, listed below.
4. **Discovery rule.** Read a verb's `--help` before using it; use `schema --command` or
   `schema --compact` for machine-readable detail; never load the full catalog.
5. **Routing.** A table that points at one reference per topic.

The seven invariants, by their lead phrase:

| # | Lead phrase |
|---|-------------|
| 1 | Claim, heartbeat, one terminal verb. |
| 2 | Choose next work with the claim-aware verbs. |
| 3 | Write through the owning binary. |
| 4 | Cross-scope guard. |
| 5 | Planning writes refuse in a git worktree. |
| 6 | Task writes and terminal verbs recompute the owning plan's status. |
| 7 | Isolate from-source binaries. |

The full text is in `SKILL.md`; this page does not restate it.

### The ten references

| Reference | Covers |
|-----------|--------|
| `references/dispatch.md` | Handing work to a worker: reading the brief, claiming on its behalf, passing the token, heartbeats, who runs the terminal verb, reviews by a separate actor. |
| `references/preparation.md` | The required and optional inputs for ready work, task readiness reasons, `spec ingest` and its work-list grammar, `task packet` references, and the workbench round trip. |
| `references/queue.md` | The host build and test queue: submitting, waiting, refusals. Optional, and set by the `use_queue` policy. |
| `references/claim-ritual.md` | Claim-aware pick, claim, heartbeat, one terminal verb, lapsed-claim recovery. |
| `references/recovery.md` | Reading `planar health`, each contributor's route, the doctor flow and its triage rules. |
| `references/resume-handoff.md` | What "resumable" means, capturing state, handing off, resuming from zero context. |
| `references/external-sync.md` | Jira and GitHub Issues: register, create, pull, push, conflicts, propagate, report an issue. |
| `references/status.md` | Read-only scope status in attention order, and live plan activity. |
| `references/knowledge.md` | Decisions, artifacts, annotations and questions; resolving targets; links; promote and demote. |
| `references/local.md` | Operator-local skills and agents, and polyrepo workspaces. |

### How a session uses it

A session reads `SKILL.md`, picks the reference its task needs, and reads only that one.
For each verb it is about to run, it reads `--help` or asks `schema --command` for that
one command. The skill sets no reporting format, review stage, testing cadence or
confirmation habit; those belong to the operator's own workflow. The
[`examples/`](../examples/README.md) directory holds sample workflow documents, including
a result-report format, that an operator can adopt or ignore.

## Choose by intent

This mirrors the routing table in `SKILL.md`.

| Intent | Go to |
|--------|-------|
| Hand work to a worker, record reviews | `references/dispatch.md` |
| Prepare work, ingest a work list, task readiness, workbench | `references/preparation.md` |
| Route builds and tests through the host queue | `references/queue.md` |
| Claim, heartbeat and finish a unit of work | `references/claim-ritual.md` |
| Degraded health, stale claims, doctor | `references/recovery.md` |
| Capture a handoff, validate a resume, enter a worktree | `references/resume-handoff.md` |
| Jira or GitHub: register, create, sync, resolve, propagate, report an issue | `references/external-sync.md` |
| See what needs attention, in what order | `references/status.md` |
| Decisions, artifacts, annotations, questions, link endpoints | `references/knowledge.md` |
| Operator-local skills and agents; workspace scan | `references/local.md` |
| Capture exploration without the agent workflow | [light-touch.md](light-touch.md) |

## The role agents

The role agents are examples of one workflow built on Planar's claims, reviews and policy
facts. Planar does not require them. An agent is dispatched by name through the host's
subagent surface, never invoked as a skill: no slash-command skill exists for any role.

| Agent | Role |
|-------|------|
| `planar-orchestrator` | Top-level delivery dispatcher: planning, review, ingest, implementation, verification, finalization. |
| `planar-coder` | Implements one scoped task and returns its change set for review. |
| `planar-test-coder` | Adds verification for cited test-spec scenarios between coder and reviewer. |
| `planar-reviewer` | Reviews coder output: approve, request-changes, open-question or abort. |
| `planar-janitor` | Finalizes delivery: integrates approved work, reconciles Planar, cleans up, closes the plan. |
| `planar-research` | Read-only investigation that returns a cited findings brief. |
| `planar-planner` | Drafts product spec, tech spec, roadmap and initial test scenarios for a goal. |
| `planar-spec-reviewer` | Adversarially reviews draft specs before ingestion. |
| `planar-ingestor` | Decomposes workbench specs into child plans, tasks, decisions and scenarios. |
| `planar-ext-sync` | Propagates a feature to Jira or GitHub Issues, on explicit request. |
| `planar-sync-reconciler` | Recommends one disposition for a sync conflict and applies it on confirmation. |

### Frontmatter contract

Each agent source is `examples/agents/planar-<role>.md` (`agents/` for
`planar-orchestrator` and `planar-test-coder`) with this frontmatter:

```yaml
---
name: planar-<role>
description: <one paragraph a harness shows when choosing an agent>
planar:
  kind: agent
  slug: planar-<role>
---
```

`name` equals the file stem and the `slug`. The installer does not place these files.

### Role values on flags

The `planar-` prefix belongs to agent names only. The role values that flags carry are bare:

- `planar-agent queue run --role coder` records the submitting role on a queue entry.
- `planar-agent claim --role` and `planar-agent pull --role` record the role on a claim.
- The `roles.<name>` config keys use the bare role (`coder`, `spec-reviewer`, `test-coder`).

`--role` accepts the hyphenated and the underscored spelling; the stored form is
underscored (`spec_reviewer`, `test_coder`).

## Operator-local skills and agents

Personal skills and agents live under `~/.planar/local/skills/<name>/SKILL.md` and
`~/.planar/local/agents/<name>.md`, managed only through `planar local`:

| Verb | Does |
|------|------|
| `planar local import <path>` | Copies a file, a skill directory or a mix into the sandbox, then links it unless `--no-link`. |
| `planar local link [<name>]` | Projects one source, or all, into every present vendor root. |
| `planar local list` | Reports every recorded projection and its state. |
| `planar local unlink <name>` | Removes the projections; `--purge` also deletes the source. |

- **Projection.** Each source is copied, never linked, into the vendor skill and agent roots,
  under the name `planar-local-<name>`. The source is never edited; a
  skill copy has its frontmatter `name` rewritten to match its directory.
- **Names.** A local name must match `^[a-z0-9]+(-[a-z0-9]+)*$` and be at most 51
  characters, so `planar-local-<name>` stays within the 64-character Agent Skills limit.
  Names are never normalized: `My_Skill` is refused at exit 2, not renamed.
- **Retired `shadow` key.** A source that sets `shadow` is refused at exit 2. Every
  projection carries the `planar-local-` prefix; a local source cannot take a bundled name.
- **States.** `planar local list` recomputes each state from disk: `live`, `stale`,
  `legacy` (an old projection waiting to be migrated), `missing` or `broken` (the source
  is gone).
- **Repair.** `planar local link --reconcile` rewrites stale copies, writes missing ones,
  migrates legacy projections and drops records whose source is gone.
- **Ownership.** A destination that exists, differs, and is not a prior Planar projection
  is refused at exit 6 before anything is written.

Full verb detail: [cli-reference.md § Domain: `local`](cli-reference.md#domain-local).

## Legacy source identifiers

Two artifact source schemes survive from the removed `planar import` and `planar synthesize`
verbs (preserved at tag `archive/import-synthesize`):

- `pl-forward-spec://<kind>` marks the product spec, tech spec and roadmap seeded from a
  forward proposal.
- `pl-synthesize://<kind>` marks the three artifacts a synthesis seeded.

They are stored values in the artifact's source path, kept so that existing databases stay
readable. They name no skill and no live verb.

## Where the rest lives

| Topic | Document |
|-------|----------|
| Every verb, flag and exit code | [cli-reference.md](cli-reference.md) |
| Install, upgrade and uninstall | [INSTALL.md](../INSTALL.md) |
| The cross-scope guard and scope resolution | [concepts.md § Cross-scope guard](concepts.md#cross-scope-guard) |
| The claim ritual and dispatch | [`skills/planar/references/claim-ritual.md`](../skills/planar/references/claim-ritual.md), [`dispatch.md`](../skills/planar/references/dispatch.md) |
| Example workflow documents | [`examples/`](../examples/README.md) |
| Capture without the agent workflow | [light-touch.md](light-touch.md) |
