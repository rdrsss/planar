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

Planar is a single Zig binary (plus the read-only `planar-watch` and
the agent-callable `planar-agent` companions) backed by SQLite. Build
from source and stage the workflow surfaces:

```sh
git clone https://github.com/rdrsss/planar.git
cd planar
./install.sh
```

`install.sh` builds the binary, copies it to `~/.planar/bin/planar`,
and stages every vendor workflow surface (Claude commands, Codex
skills, Copilot skills/instructions, Planar agents) into their
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

`init` does three things:

1. Applies every migration embedded in the binary (sourced from
   `migrations/` at build time via `tools/gen_migrations.zig`) against
   the new database. The public schema-version contract is the
   `schema_migrations` table — query it any time with
   `sqlite3 ~/.planar/planar.db "select * from schema_migrations;"`.
2. Resolves the current directory's git remote to a `projects`
   row and creates an `associations` row of kind `project`, then
   wires the two together via `project_associations`.
3. Pushes the new association onto the active-scope stack so
   subsequent write verbs route here by default.

Inspect the result:

```sh
planar scope show
planar health
```

`scope show` should print one row (your project association at the
top of the stack); `health` should print `OK` everywhere.

## 3. Your first plan

A *plan* is a unit of intent — usually a feature or a bug fix — that
collects tasks, artifacts, decisions, scenarios, and questions
under one slug. Create one:

```sh
planar plan create "Add login flow" --slug login-flow
```

The CLI prints the new plan id. Show it:

```sh
planar plan show login-flow
```

Plans nest. To carve out a milestone:

```sh
planar plan create "M1 — landing page" --slug login-flow-m1 \
    --parent login-flow
```

## 4. Your first task

Tasks are the unit of execution[^founding_tech_spec]. Create one
under the plan you just made:

```sh
planar task add "Wire OAuth callback" --plan login-flow --priority 100
```

Move it through its status states:

```sh
planar task list --plan login-flow
planar task start <task-id>      # -> in_progress
planar task done  <task-id>      # -> done
```

`task start` is just a status transition; it does not run any
code. The actual work happens in your editor; Planar tracks the
intent and the outcome.

## 5. The workbench

The *workbench* is a bidirectionally synced filesystem under
`~/.planar/workbench/`. Every plan with the workbench enabled gets
a directory there; you author specs, ADRs, and design notes as
plain markdown files, and `planar workbench sync` round-trips them
into the `artifacts` table[^founding_tech_spec].

Open the workbench for your plan:

```sh
planar workbench open login-flow
ls ~/.planar/workbench/<your-org>/p<id>-login-flow/
```

You will see one file per artifact (initially empty). Drop in a
product spec:

```sh
cat > ~/.planar/workbench/<your-org>/p<id>-login-flow/product-spec.md <<'EOF'
---
artifact_kind: product_spec
status: draft
title: Login flow — product spec
---
# Login flow — product spec

## Intent

Allow first-time users to authenticate via OAuth and land on the
right onboarding step.

## Out of scope

- Email/password fallback (deferred).
EOF
```

Sync it back to the database:

```sh
planar workbench sync login-flow
planar artifact list --plan login-flow
```

The new `product_spec` artifact is now in the DB. Editing it on
disk and re-running `workbench sync` round-trips the change.

## 6. Spec ingestion

`planar spec ingest` decomposes a workbench planning document into
a structured task graph. The decomposer reads your product/tech
spec, identifies milestones, and proposes tasks under each.

Author a roadmap:

```sh
cat > ~/.planar/workbench/<your-org>/p<id>-login-flow/roadmap.md <<'EOF'
---
artifact_kind: roadmap
status: draft
title: Login flow — roadmap
---
# Login flow — roadmap

## M1 — landing page

- Wire OAuth callback.
- Persist the user record.

## M2 — onboarding routing

- Read the new-user flag.
- Route first-timers to /welcome.
EOF
planar workbench sync login-flow
planar spec ingest login-flow
```

The ingestor creates one sub-plan per `## ` heading and one task
per bullet. Inspect the result hierarchically (`planar plan show`
only returns the parent plan's own row, so use `tree` to see the
new sub-plans and tasks):

```sh
planar tree
```

## 7. External-system sync

Planar's operational plane integrates with Jira and GitHub Issues
via adapters. `ext`/`sync` live on the `planar-ext` binary. To create
a GitHub Issues counterpart for your plan:

```sh
planar-ext ext list                            # check registered systems
planar-ext ext create github --from plan:<login-flow-id> --type Epic
planar-ext ext propagate-one github --from plan:<login-flow-id>
```

`ext propagate-one` creates a single external counterpart per call. The
whole-tree walk (`ext propagate <plan>`, one issue per task, linked via
`external_links`) is not yet implemented on either binary — see
`docs/cli-reference.md`. Local changes flow out with `planar-ext sync push`.
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
| `init`: "git remote not found" | No `origin` remote | `git remote add origin <url>` and re-run |
| `task add`: scope-mismatch error | cwd not equal to stack top | Pass `--scope` or `cd` into the right repo |
| Workbench files reappear after deletion | Sync round-tripped from DB | Delete the artifact with `planar artifact rm` |
| `ext propagate-one`: no adapter registered | Adapter not yet created | `planar-ext ext create <kind>` first |

[^founding_tech_spec]:
[^doc_tech_spec]:
