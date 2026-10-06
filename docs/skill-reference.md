# Planar Skill and Agent Reference

Planar ships one skill, `planar`, in the [Agent Skills](https://agentskills.io) layout, and
fifteen role agents named `planar-<role>`. The CLI is the contract: the skill teaches only
the rules that cross verbs, and everything verb-shaped comes from the binaries. The first
step, with or without the skill, is `planar --help`.

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

The same works on `planar-agent`, `planar-watch`, `planar-ext` and `planar-execute`. Ask the
catalog for one command or for the compact tree, never for the whole catalog: for `planar`
it is about 338 KB. Every verb, flag and exit code is in [cli-reference.md](cli-reference.md).

## Using it day to day

There is no command to type. Ask for the outcome in plain words, from a directory inside
the project, and the harness loads the `planar` skill because the request matches its
description. The skill tells the session which reference to read, and role work goes to
the matching `planar-<role>` agent.

```text
Draft a Planar spec for adding multi-currency checkout.
Review the spec for plan 1234.
Show me the ingest preview for plan 1234.
Orchestrate plan 1234.
What needs my attention in this project?
Hand off task 5678; I'm stopping for today.
Resume task 5678.
Sync plan 1234 to GitHub Issues.
```

### Asking for the skill or an agent by name

- **The skill.** In Claude Code, `/planar <request>` loads the skill explicitly. In the
  other harnesses, say "use the planar skill" in the request.
- **An agent.** Name it in the request: "use the planar-planner agent to draft a spec for
  …", "have planar-reviewer review the diff on this branch". Agents are dispatched as
  subagents, never typed as commands.

### What stays with you

Three steps never run automatically: `planar spec ingest --apply`, `planar plan closeout`
and `planar workbench archive`. The session shows the preview, then waits for you to
confirm that exact invocation. Nothing reaches Jira or GitHub Issues unless you ask:
propagation runs only on an explicit request and previews with `--dry-run` first, and
`planar-ext sync push` is never automatic. Say yes, or run the command yourself.

### Coming from the `pl-*` skills

The forty `pl-*` skills were retired in favour of the one skill and the agents. Each one
maps to a request, an agent or a command:

| Retired skill | Now |
|---------------|-----|
| `pl-spec-draft` | "Draft a spec for …": the `planar-planner` agent |
| `pl-spec-review` | "Review the spec for plan N": the `planar-spec-reviewer` agent |
| `pl-spec-ingest` | "Preview the ingest for plan N": `planar spec ingest N --strict --json`, then `--apply` on your yes |
| `pl-orchestrator` | "Orchestrate plan N": the `planar-orchestrator` agent |
| `pl-coder`, `pl-test-coder`, `pl-reviewer` | the `planar-coder`, `planar-test-coder` and `planar-reviewer` agents, usually dispatched by the orchestrator |
| `pl-research` | the `planar-research` agent |
| `pl-import`, `pl-synthesize` | the `planar-importer` and `planar-synthesizer` agents, over `planar import` and `planar synthesize` |
| `pl-status`, `pl-observe` | "What needs attention?": `references/status.md`, over `planar dashboard`, `planar-watch ps` and `planar-watch plans` |
| `pl-health`, `pl-doctor` | "Planar health is degraded": `references/recovery.md`, over `planar health` and `planar-agent reconcile`; there is no `doctor` verb |
| `pl-handoff`, `pl-resume` | "Hand off task N" or "Resume task N": `references/resume-handoff.md`, over `planar handoff create` and `planar resume` |
| `pl-plan`, `pl-task`, `pl-question`, `pl-scenario` | ask for the change, or run `planar plan`, `planar task`, `planar question` or `planar scenario` directly |
| `pl-knowledge`, `pl-promote` | `references/knowledge.md`, over `planar decision`, `planar artifact`, `planar annotate`, `planar links`, `planar promote` and `planar demote` |
| `pl-workbench`, `pl-workbench-sync`, `pl-workbench-archive` | `references/spec-pipeline.md`, over `planar workbench` |
| `pl-ext-create`, `pl-ext-propagate`, `pl-sync`, `pl-audit-trail`, `pl-templates` | `references/external-sync.md` and the `planar-ext-sync` agent, over `planar-ext`, `planar audit trail` and `planar templates` |
| `pl-feedback-triage`, `pl-introspect`, `pl-report-issue` | the `planar-feedback-triager` and `planar-introspector` agents, over `planar feedback` and `planar report` |
| `pl-local`, `pl-local-import`, `pl-workspace-scan` | `references/local.md`, over `planar local` and `planar workspace` |
| `pl-models-config` | `planar models`; see [concepts.md](concepts.md#model-routing) |
| `pl-init`, `pl-scope` | `planar init` and `planar scope show` |
| `pl-help` | `planar --help`, `planar help` and `planar schema --command "<verb>"` |

A request that names a retired skill still works: the session reads the intent, not the
name.

## What installs where

`install.sh` stages the skill and the agents under `$PLANAR_HOME` (default `~/.planar/`),
then places them into each vendor harness whose presence marker exists. Six vendors read
nine targets. A vendor without its marker is skipped and named in the installer's summary;
`--vendors` narrows the set further.

### Vendor targets

| Vendor | Presence marker | Skill | Agents |
|--------|-----------------|-------|--------|
| Claude Code | `~/.claude/` | `~/.claude/skills/planar` | `~/.claude/agents/planar-<role>.md` |
| Codex | `$CODEX_HOME` set, else `~/.codex/` | `~/.agents/skills/planar` (shared) | `$CODEX_HOME/agents/planar-<role>.toml` |
| Copilot | `~/.copilot/` | `~/.agents/skills/planar` (shared) | `~/.copilot/agents/planar-<role>.agent.md` |
| Gemini CLI | `~/.gemini/settings.json` | `~/.agents/skills/planar` (shared) | `~/.gemini/agents/planar-<role>.md` |
| Antigravity | `~/.gemini/antigravity-cli/` | `~/.gemini/antigravity-cli/skills/planar` | `~/.gemini/antigravity-cli/agents/planar-<role>.md` |
| OpenCode | `~/.config/opencode/` | none of its own | `~/.config/opencode/agents/planar-<role>.md` |

The nine targets are the three skill roots (`~/.claude/skills`, the shared `~/.agents/skills`
and Antigravity's private `~/.gemini/antigravity-cli/skills`) and the six agent directories.

- **The shared skill root.** `~/.agents/skills/planar` is placed once when any of Codex,
  Copilot, Gemini CLI or OpenCode is present, however many of them are. The install
  manifest records it under the owner `shared`, because no single vendor owns it.
- **Antigravity** has a private skill root and is detected independently of Gemini CLI.
- **OpenCode** gets no private skill copy. It reads the shared `~/.agents/skills` root and
  `~/.claude/skills`, and Planar never writes `~/.config/opencode/skills`. Its agent copy is
  derived: the frontmatter is reduced to `description` and `mode: subagent`, the body kept.
- **Copilot** agent files carry the `.agent.md` suffix.
- **Codex** agents are TOML files with `name`, `description` and `developer_instructions`,
  staged under `$PLANAR_HOME/codex-agents/`. `$CODEX_HOME` defaults to `~/.codex`.

### Gemini CLI subagents

Gemini CLI loads the agents in `~/.gemini/agents/` as subagents by default. Setting
`experimental.enableAgents` to `false` in `~/.gemini/settings.json` turns subagents off; the
skill keeps working and the installer still places the agent files.

### Link and copy mode

By default `install.sh` copies: each skill target is a real directory and each agent a
regular file, so the source checkout can be deleted afterwards. With `--link`, the skill
directories and the Markdown, Copilot and Codex agent files are symlinks into
`$PLANAR_HOME`, and source edits show up without reinstalling. The OpenCode agents are
derived, so they are regular files in both modes. A destination that exists and that no
Planar install manifest records stops the install, naming the path. See
[INSTALL.md § Copy mode vs link mode](../INSTALL.md#copy-mode-vs-link-mode).

### Drift, upgrade and uninstall

- **Drift.** `$PLANAR_HOME/install-manifest.json` is the only ownership record.
  `planar health` compares every recorded target across the nine roots with the staged
  authority and reports it `fresh`, `stale` or `missing`; other entries in those roots are
  `unmanaged` and never degrade health. A degraded result carries the reinstall command.
- **Upgrade.** A reinstall over an older install removes the previous per-vendor skill and
  command projections that it can prove Planar made, prints each removal, and leaves and
  reports anything it cannot prove. See
  [INSTALL.md § Upgrade note](../INSTALL.md#upgrade-note-the-previous-skill-and-agent-projections).
- **Uninstall.** `./install.sh --uninstall` removes every target the manifest records while
  it still holds what Planar placed, and everything under `$PLANAR_HOME` except
  the preserved data paths (`planar.db` and its sidecars, `queue-logs/`, `workbench/`,
  `config.toml`, `local/` and the rest of the
  [INSTALL.md § Preserved paths](../INSTALL.md#preserved-paths) list); `--force` does
  not remove those. It never removes a vendor
  directory or `~/.agents/skills` itself. See [INSTALL.md § Uninstall](../INSTALL.md#uninstall).

## The `planar` skill

The source is [`skills/planar/`](../skills/planar/SKILL.md): one `SKILL.md` and nine
references. The installed copy lives in the skill roots above and in
`$PLANAR_HOME/skills/planar/`.

### The four parts of `SKILL.md`

1. **Write surface.** The five binaries and what each one writes. The boundary is each
   binary's verb set, not a runtime ACL, so choosing the binary is the agent's job.
2. **Invariants.** Thirteen rules that hold across verbs, listed below.
3. **Discovery rule.** Read a verb's `--help` before using it; use `schema --command` or
   `schema --compact` for machine-readable detail; never load the full catalog.
4. **Routing.** An intent table that points at one reference, and the names of the
   fifteen agents for role work.

The thirteen invariants, by their lead phrase:

| # | Lead phrase |
|---|-------------|
| 1 | Claim, heartbeat, one terminal verb. |
| 2 | Never split the terminal verb. |
| 3 | Write through the owning binary. |
| 4 | Cross-scope guard: ten verbs. |
| 5 | `--scope` means four things. |
| 6 | Read back every mutation. |
| 7 | Planning verbs refuse in worktrees. |
| 8 | Next work is claim-aware. |
| 9 | Plan status follows its tasks. |
| 10 | Operator gates are never automatic. |
| 11 | Sync pull writes no planning entity. |
| 12 | Announce cross-scope writes exactly. |
| 13 | Isolate from-source binaries. |

The full text is in `SKILL.md`; this page does not restate it.

### The nine references

| Reference | Covers |
|-----------|--------|
| `references/claim-ritual.md` | Claim-aware pick, claim, heartbeat, one terminal verb, lapsed-claim recovery, the host build queue. |
| `references/status.md` | Read-only scope status in attention order, and live plan activity. |
| `references/recovery.md` | Reading `planar health`, each contributor's route, the doctor flow and its triage rules. |
| `references/resume-handoff.md` | What "resumable" means, capturing state, handing off, resuming from zero context. |
| `references/spec-pipeline.md` | The four planning documents, the ingest grammar, and the six gated stages to closeout. |
| `references/knowledge.md` | Decisions, artifacts, annotations and questions; resolving targets; links; promote and demote. |
| `references/external-sync.md` | Jira and GitHub Issues: register, create, pull, push, conflicts, propagate, report an issue. |
| `references/local.md` | Operator-local skills and agents, and polyrepo workspaces. |
| `references/feedback-contract.md` | The seven-section result envelope every workflow reports with. |

### How a session uses it

A session reads `SKILL.md`, picks one reference from the routing table, and reads only that
one. For each verb it is about to run, it reads `--help` or asks `schema --command` for that
one command. It writes through the owning binary, reads the result back, and reports with
the seven-section envelope (Context, Intent, Actions, Result, Warnings, Next actions,
Recovery). That envelope is stated once, in `references/feedback-contract.md`; the other
references point at it rather than repeat it.

## Choose by intent

This mirrors the routing table in `SKILL.md`.

| Intent | Go to |
|--------|-------|
| Claim, heartbeat and finish a unit of work | `references/claim-ritual.md` |
| See what needs attention, in what order | `references/status.md` |
| Degraded health, stale claims, doctor | `references/recovery.md` |
| Capture a handoff, validate a resume, enter a worktree | `references/resume-handoff.md` |
| Draft, review and ingest specs; workbench; closeout | `references/spec-pipeline.md` |
| Decisions, artifacts, annotations, questions, link endpoints | `references/knowledge.md` |
| Jira or GitHub: register, create, sync, resolve, propagate, report an issue | `references/external-sync.md` |
| Operator-local skills and agents; workspace scan | `references/local.md` |
| Deliver a feature end to end | the `planar-orchestrator` agent |
| Implement, test or review one task | `planar-coder`, `planar-test-coder`, `planar-reviewer` |
| Draft or review specs as a dispatched role | `planar-planner`, `planar-spec-reviewer` |
| Bring an existing repository into Planar | `planar-importer` or `planar-synthesizer` |
| Capture exploration without the agent workflow | [light-touch.md](light-touch.md) |

## The role agents

Role work goes to agents. An agent is dispatched by name through the host's subagent
surface, never invoked as a skill: no slash-command skill exists for any role.

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
| `planar-importer` | Translates an existing repository's planning content into Planar. |
| `planar-synthesizer` | Synthesizes fresh planning artifacts from docs, git history and source. |
| `planar-ext-sync` | Propagates a feature to Jira or GitHub Issues, on explicit request. |
| `planar-feedback-triager` | Triages redacted feedback findings, applying only approved changes. |
| `planar-introspector` | Proposes friction findings from redacted usage signal. |
| `planar-sync-reconciler` | Recommends one disposition for a sync conflict and applies it on confirmation. |

### Frontmatter contract

Each agent source is `agents/planar-<role>.md` with this frontmatter:

```yaml
---
name: planar-<role>
description: <one paragraph a harness shows when choosing an agent>
planar:
  kind: agent
  slug: planar-<role>
---
```

`name` equals the file stem and the `slug`. The Copilot, OpenCode and Codex forms are
derived from this one source at install time.

### Doctrine documents

Four shared documents sit beside the agents: `methodology.md` (the procedural flow and the
claim ritual), `doctrine.md` (cross-cutting judgment, and the canonical operator feedback
contract), `models.md` (tiers and role assignments) and `cross-scope-writes.md` (the
cross-scope write cue). They install under `$PLANAR_HOME/agents/` and are never placed into
a vendor agent directory; an agent refers to them by name.

### Role values on flags

The `planar-` prefix belongs to agent names only. The role values that flags and routing
bindings carry are bare:

- `planar-agent queue run --role coder` records the submitting role on a queue entry.
- `planar-agent claim --role` and `planar-agent pull --role` record the role on a claim.
- `planar models resolve --role coder` answers which tier a role runs at.
- The role column of `models.md`'s assignment table and the `roles.<name>` config keys use
  the bare role (`coder`, `spec-reviewer`, `test-coder`).

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

- **Projection.** Each source is copied, never linked, into the same roots as the bundled
  skill and agents, under the name `planar-local-<name>`. The source is never edited; a
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

Two artifact source schemes survive from an earlier layout:

- `pl-forward-spec://<kind>` marks the product spec, tech spec and roadmap seeded from a
  forward proposal by `planar import` and `planar synthesize`.
- `pl-synthesize://<kind>` marks the three artifacts `planar synthesize` seeds.

They are stored values in the artifact's source path, kept so that existing databases stay
readable. They name no skill. The `planar synthesize` handler branches on them: when it
retires an anchor's artifacts it keeps those whose source starts with the
`pl-synthesize://` scheme.

## Where the rest lives

| Topic | Document |
|-------|----------|
| Every verb, flag and exit code | [cli-reference.md](cli-reference.md) |
| Install, upgrade and uninstall | [INSTALL.md](../INSTALL.md) |
| The cross-scope guard and scope resolution | [concepts.md § Cross-scope guard](concepts.md#cross-scope-guard) |
| Orchestration flow and the claim ritual | [`agents/methodology.md`](../agents/methodology.md) |
| Cross-cutting agent judgment | [`agents/doctrine.md`](../agents/doctrine.md) |
| Role tiers and model routing | [`agents/models.md`](../agents/models.md) |
| The cross-scope write cue | [`agents/cross-scope-writes.md`](../agents/cross-scope-writes.md) |
| Capture without the agent workflow | [light-touch.md](light-touch.md) |
