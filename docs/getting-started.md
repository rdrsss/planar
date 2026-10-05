# Getting started

This walkthrough takes you from a fresh checkout to your first plan,
your first task, an end-to-end workbench round-trip, a spec-ingest
pass, and finally an external-system propagation. It is the longest
linear path through Planar — everything else in the docs is either
narrower (a single feature page) or wider (the workflow recipes).

Run every step in order on a fresh machine. The walkthrough takes
about 30 minutes the first time and 5 minutes the second.

## 1. Install

Planar is five C++26 binaries — the operator CLI `planar`, the
agent-callable `planar-agent`, the read-only `planar-watch`, the
workflow runner `planar-execute`, and the external-system binary
`planar-ext` — backed by SQLite. Build from source and stage the
workflow surfaces:

```sh
git clone https://github.com/rdrsss/planar.git
cd planar
./install.sh
```

`install.sh` builds the binaries, installs them under `~/.planar/bin/`,
and installs the one `planar` skill and the `planar-<role>` agents into
the harnesses it finds. Add `~/.planar/bin` to `$PATH` if it is not
already there.

Check that the binary runs:

```sh
planar version
```

## 2. Start with `planar --help`

The first command to run, with or without a coding agent, is the help
page. Every binary documents itself:

```sh
planar --help
planar task --help
planar task add --help
planar help
planar schema --compact
```

`planar --help` lists the verb groups. `planar task --help` lists one
group's verbs, and `planar task add --help` is a leaf verb's help: every
flag is described, and the page ends with `Examples:` and `Exit codes:`
sections. `planar help` prints the root page. To find any verb without
guessing, `planar schema --compact` prints one row per command with its
summary, and `planar schema --command "task add"` prints the full entry
for one command. Ask the catalog for one command or the compact tree,
never the whole catalog. The same works on `planar-agent`, `planar-watch`,
`planar-ext` and `planar-execute`.

Everything in the rest of this tutorial is plain CLI. No skill is needed.

## 3. Using Planar from a coding agent

The installer already placed the one `planar` skill and the fifteen
`planar-<role>` agents for whichever of the six supported vendors it
found on your machine. The skill teaches only the rules that cross verbs;
the CLI help stays the source for everything verb-shaped. Confirm the
install with:

```sh
planar health
```

The `projection freshness` row counts the installed skill and agent
files as `fresh`, `stale` or `missing`; a degraded result prints the
reinstall command.

There is no command to type. From inside a project, ask your coding
agent for the outcome in plain words, and it loads the skill on its own:

```text
Draft a Planar spec for adding multi-currency checkout.
What needs my attention in this project?
Hand off task 42; I'm stopping for today.
```

In Claude Code, `/planar <request>` loads the skill explicitly. Name a
role agent to dispatch it: "use the planar-planner agent to draft …".
[Skill reference § Using it day to day](skill-reference.md#using-it-day-to-day)
lists more requests and maps each retired `pl-*` skill to its
replacement; the rest of that page covers the layout, the vendor
targets, drift and uninstall. Come back to this tutorial for the CLI
path either way.

## 4. Initialise the database

`planar init` creates `~/.planar/planar.db` (the operational SQLite
store) and registers the current directory as a project:

```sh
mkdir -p ~/work/example-app
cd ~/work/example-app
git init
git remote add origin git@github.com:you/example-app.git
planar init
```

`init` does two things:

1. Applies every migration embedded in the binary (sourced from
   `migrations/` at configure time via `cmake/generate_migrations.cmake`) against
   the new database. The public schema-version contract is the
   `schema_migrations` table — query it any time with
   `sqlite3 ~/.planar/planar.db "select * from schema_migrations;"`.
2. Registers the current directory as a `projects` row, named after
   the directory.

`init` does not create the project's planning scope. Its output names
the two commands that do — run them next:

```sh
planar assoc create project:example-app --kind project
planar assoc add project:example-app ~/work/example-app
```

Scope is derived from your current working directory, so write verbs
run from inside the repo now route to `project:example-app` by default.

To group several projects under one association you can scope work to,
create an `org` association and add the project to it:

```sh
planar assoc create org:my-org --kind org --name "My Org"
planar assoc add org:my-org "$(pwd)"
```

Inspect the result:

```sh
planar scope show
planar health
```

`scope show` should print `project:example-app` as the scope resolved
from cwd; `health` should end with `overall: ok`.

## 5. Your first plan

A *plan* is a unit of intent — usually a feature or a bug fix — that
collects tasks, artifacts, decisions, scenarios, and questions
under one slug. Create one:

```sh
planar plan create "Add login flow" --slug login-flow
```

The CLI prints the new plan id. Verbs take that numeric id, not the
slug. Show it:

```sh
planar plan show <plan-id>
```

Plans nest. To carve out a milestone:

```sh
planar plan create "M1 — landing page" --slug login-flow-m1 \
    --parent <plan-id>
```

## 6. Your first task

Tasks are the unit of execution. Create one
under the plan you just made:

```sh
planar task add "Wire OAuth callback" --plan <plan-id> --priority 100
```

Move it through its status states:

```sh
planar task list --plan <plan-id>
planar task update <task-id> --status doing   # todo -> doing
planar task done <task-id>                    # -> done
```

`task update --status` is just a status transition; it does not run any
code. The actual work happens in your editor; Planar tracks the
intent and the outcome.

## 7. The workbench

The *workbench* is a bidirectionally synced filesystem under
`~/.planar/workbench/`. Every plan with the workbench enabled gets
a directory there; specs, ADRs, and design notes are plain markdown
files, and `planar workbench sync` round-trips them with the
`artifacts` table.

Workbench files carry Planar-generated frontmatter (entity kind and
id), so create the artifact through the CLI and let the workbench
materialize its file:

```sh
cat > /tmp/product-spec.md <<'EOF'
## Intent

Allow first-time users to authenticate via OAuth and land on the
right onboarding step.

## Out of scope

- Email/password fallback (deferred).
EOF
planar artifact add "Login flow — product spec" --kind product_spec \
    --plan <plan-id> --from-file /tmp/product-spec.md
planar workbench sync <plan-id>
ls ~/.planar/workbench/<your-org>/p<plan-id>-login-flow/
```

You will see a `README.md` for the plan, `plans/` and `tasks/`
directories, and one `<artifact-id>-<slug>.md` file per artifact.
Edit the product spec on disk, then sync it back to the database:

```sh
planar workbench sync <plan-id>
planar artifact show <artifact-id>
```

Editing the file on disk and re-running `workbench sync` round-trips
the change. A hand-dropped file without the generated frontmatter is
reported as malformed and is not imported.

## 8. Spec ingestion

`planar spec ingest` decomposes a workbench planning document into
a structured task graph. It reads the plan's `tech_spec` and `roadmap`
artifacts from the workbench (both are required), identifies
milestones, and proposes tasks under each.

Author a tech spec and a roadmap:

```sh
planar artifact add "Login flow — tech spec" --kind tech_spec \
    --plan <plan-id> --body "OAuth callback handler and onboarding router."
cat > /tmp/roadmap.md <<'EOF'
## M1 — landing page

- Wire OAuth callback.
- Persist the user record.

## M2 — onboarding routing

- Read the new-user flag.
- Route first-timers to /welcome.
EOF
planar artifact add "Login flow — roadmap" --kind roadmap \
    --plan <plan-id> --from-file /tmp/roadmap.md
planar workbench push <plan-id>
planar spec ingest <plan-id>            # preview only
planar spec ingest <plan-id> --apply    # write the task graph
```

Without `--apply`, `spec ingest` prints the proposed changes and
writes nothing. With it, the ingestor creates (or reuses, when the
title matches an existing child plan) one sub-plan per `## ` heading
and one task per bullet. Inspect the result hierarchically (`planar plan show`
only returns the parent plan's own row, so use `tree` to see the
new sub-plans and tasks):

```sh
planar tree
```

## 9. External-system sync

Planar's operational plane integrates with Jira and GitHub Issues
via adapters. `ext`/`sync` live on the `planar-ext` binary. Register
the external system once, then create counterparts for your plan:

```sh
planar-ext ext register github github --project you/example-app \
    --auth-env GITHUB_TOKEN
planar-ext ext list                            # check registered systems
planar-ext ext propagate-one github --from plan:<plan-id> --dry-run
planar-ext ext propagate <plan-id> --system github --dry-run
```

`ext propagate-one` creates a single external counterpart per call.
`ext propagate <plan-id>` walks the whole tree — the plan, its
sub-plans, and their tasks — and records each counterpart in
`external_links`. Drop `--dry-run` to create the issues. Local changes flow out with `planar-ext sync push`.
`planar-ext sync pull` fetches remote state and reports it — it does not
write local fields (decision 996); use it to see whether the remote drifted,
then update the local entity yourself if warranted.

## 10. Capture, hand off, and resume

The walkthrough so far recorded intent. This section records the *work*
itself, then moves it to a fresh agent. The examples below assume plan 1 and
task 1; substitute your own ids. Everything here writes only to the local
SQLite store.

Give the plan ordered steps, tasks with a next action, and an open question:

```sh
planar plan step add 1 "Inventory v1 callers"
planar plan step add 1 "Document v2 contracts"
planar task add "Inventory v1 callers" --plan 1 --next-action "grep the monorepo"
planar task add "Document v2 contracts" --plan 1
planar question add "Do we deprecate before delete, or atomically?"
```

Capture the working session. Vendor identity comes from `$PLANAR_VENDOR`:

```sh
export PLANAR_VENDOR=claude-code
planar capture session --task 1
planar capture note "found 7 callers under services/billing"
planar capture command "rg -l 'v1.client'"     # cli-lint-ignore: `-l` belongs to the recorded rg command inside the quotes
planar capture snapshot "halfway through the inventory" --task 1 --next-action "audit services/payments"
```

Record a decision tied to the active session:

```sh
planar decision add "Deprecate v1 over two releases" --body "Warn in release N, remove in N+2"
```

Hand off. One command captures a snapshot, opens a handoff record, and
validates it, all atomically:

```sh
planar handoff 1 --vendor codex --note "halfway through; payments next"
```

Resume on a fresh agent in a different vendor:

```sh
export PLANAR_VENDOR=codex
planar resume 1            # 8-section packet, ready to drop into context
planar resume validate 1   # exit 0 if resumable; 1 with remediation if not
```

`resume validate` is the CI gate: it exits `0` when the task can be resumed
from zero conversational context, and `1` with remediation when it cannot.

## 11. What's going on? — `planar tree`

The fastest answer to "what work do I have, where does it live, and what's
the structure?" is `planar tree`:

```sh
planar tree                              # active scope: plans → tasks → derived artifacts
planar tree --all-scopes                 # every scope (global section always rendered)
planar tree --status todo                # only open work
planar tree --kind plan --depth 2        # plan outline, two levels deep
planar tree --json | jq                  # nested machine-readable shape
```

The verb walks `plans.parent_plan_id` for plan hierarchy,
`tasks.plan_id` / `parent_task_id` for tasks and subtasks, and
`entity_links(derives-from)` for the artifacts, decisions, scenarios, and
questions attached to each plan. It is read-only: no schema writes, no
external calls.

The flag surface is exactly seven flags — `--scope`, `--all-scopes`,
`--depth`, `--kind`, `--status`, `--sort`, `--json` — and `--kind` /
`--status` each take a single value. It does not mirror Unix `tree(1)`: none
of that tool's flags (`-L`, `-I`, `-P`, `--prune`, `--noreport`,
`--dirsfirst`, `-J`, …) are accepted, and every one of them fails at parse
time with exit 2. See
[CLI reference § Domain: `tree`](cli-reference.md#domain-tree).

## 12. Where to go next

- [Concepts](concepts.md) — the mental model: scope, association,
  plan, task, handoff, workbench.
- [Workflows](workflows.md) — end-to-end recipes (resuming a
  stalled task, switching scope mid-session, repairing a workbench).
- [Architecture](architecture.md) — storage model, schema contract,
  adapter boundary.
- [CLI reference](cli-reference.md) — every command, every flag,
  every exit code.
- [Skill reference](skill-reference.md) — the `planar` skill, the
  `planar-<role>` agents, and where the installer places them.
- [Light touch](light-touch.md) — Planar as a capture journal, four
  verbs and no agent workflow.

## Troubleshooting

| Symptom | Likely cause | Fix |
|---------|--------------|-----|
| `planar: command not found` | `~/.planar/bin` not on `$PATH` | Add it to your shell rc |
| `plan create`: "project has no association" | `init` registered the project but no association exists yet | Run the `planar assoc create` / `planar assoc add` commands `init` printed |
| `plan show`: "plan id must be an integer" | A slug was passed where an id is required | Pass the numeric plan id |
| `workbench sync`: `MALFORMED … (MissingRequiredField)` | A hand-created file lacks the generated frontmatter | Create the entity with `planar artifact add`, then sync |
| `ext propagate-one`: "external system '<slug>' not found" | System not yet registered | `planar-ext ext register github <slug> --project <owner/repo>` first |
