---
title: Getting started
doc_kind: getting_started
template_version: 1
regenerated_at: 2026-05-18T00:00:00Z
regenerated_by: hand
references:
  doc_tech_spec:
    kind: planar
    entity: artifact:67
  founding_tech_spec:
    kind: planar
    entity: artifact:30
---

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
and stages every vendor workflow surface (Claude commands, Codex,
Copilot, and Gemini skills, Planar agents) into their
per-user install paths. Add `~/.planar/bin` to `$PATH` if it is not
already there.

Verify the install:

```sh
planar version
planar --help
```

## 2. Initialise the database

`planar init` creates `~/.planar/planar.db` (the operational SQLite
store) and registers the current directory as a project[^founding_tech_spec]:

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

Inspect the result:

```sh
planar scope show
planar health
```

`scope show` should print `project:example-app` as the scope resolved
from cwd; `health` should end with `overall: ok`.

## 3. Your first plan

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

## 4. Your first task

Tasks are the unit of execution[^founding_tech_spec]. Create one
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

## 5. The workbench

The *workbench* is a bidirectionally synced filesystem under
`~/.planar/workbench/`. Every plan with the workbench enabled gets
a directory there; specs, ADRs, and design notes are plain markdown
files, and `planar workbench sync` round-trips them with the
`artifacts` table[^founding_tech_spec].

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

## 6. Spec ingestion

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

## 7. External-system sync

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

## 8. Docs as a first-class repo concern

Outward-facing docs under `docs/` are tracked by the separately installed
`tabularium` tool in its machine-local database. The
manifest links each published doc to one or more **repo-path
sources** (directories or files in the working tree), so any drift
in covered subtrees surfaces as a regenerate-candidate on the next
diff. The workflow is:

```sh
# After authoring a new doc, wire it to the source areas it covers:
tabularium cover docs/features/login-flow.md src/login/
tabularium build       # refresh machine-local project state

# Routine drift checks:
tabularium verify      # O(1) root compare against the live tree
tabularium diff        # three-signal breakdown if anything moved
```

Source drift in `src/login/` then surfaces as a
regenerate-candidate on the next `tabularium diff`[^doc_tech_spec].

## 9. Where to go next

- [Concepts](concepts.md) — the mental model: scope, association,
  plan, task, handoff, workbench.
- [Workflows](workflows.md) — end-to-end recipes (resuming a
  stalled task, switching scope mid-session, repairing a workbench).
- [Architecture](architecture.md) — storage model, schema contract,
  adapter boundary.
- [CLI reference](cli-reference.md) — every command, every flag,
  every exit code.
- [Skill reference](skill-reference.md) — every vendor skill
  (`pl-*`) with its inputs, outputs, and side effects.

## Troubleshooting

| Symptom | Likely cause | Fix |
|---------|--------------|-----|
| `planar: command not found` | `~/.planar/bin` not on `$PATH` | Add it to your shell rc |
| `plan create`: "project has no association" | `init` registered the project but no association exists yet | Run the `planar assoc create` / `planar assoc add` commands `init` printed |
| `plan show`: "plan id must be an integer" | A slug was passed where an id is required | Pass the numeric plan id |
| `workbench sync`: `MALFORMED … (MissingRequiredField)` | A hand-created file lacks the generated frontmatter | Create the entity with `planar artifact add`, then sync |
| `ext propagate-one`: "external system '<slug>' not found" | System not yet registered | `planar-ext ext register github <slug> --project <owner/repo>` first |

[^founding_tech_spec]:
[^doc_tech_spec]:
