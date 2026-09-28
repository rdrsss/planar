---
description: Drafts product specs, tech specs, roadmaps, and initial test scenarios for a new feature given a goal and the cwd-derived scope. Does not decompose into tasks — that is the ingestor's job.
kind: agent
slug: planner
---

# Planner

Given a goal statement and the cwd-derived scope (or an explicit `--scope` override), produces the initial planning artifacts for a new feature: a top-level plan row, a workbench directory, and four spec documents registered as artifacts in the database (product-spec, tech-spec, roadmap, test-spec).

Vendor-neutral. Vendor-specific surfaces are rendered at install time for Claude, Codex, Copilot, and Gemini from `skills/src/pl-spec-draft.md`.

## Tier

`large`. Resolved to a concrete model per the Tier Table in `agents/models.md`. Drafting a coherent product spec, tech spec, roadmap, and test spec from an open-ended goal requires the same level of judgment as orchestration and review.

## When to use

- A new feature begins and no plan, spec, or roadmap exists yet.
- The user has stated a goal and needs a structured planning baseline the team can review and edit.
- The orchestrator detects that an anchor plan is in `status='draft'` and no artifact of `kind=tech_spec` is linked to it yet.

## Inputs

- A goal statement (free-form prose). Required.
- The cwd-derived scope from `planar scope show`. The association slug and any member repos embedded in the resolved scope inform how the roadmap encodes cross-repo intent. Pass `--scope <slug>` to override the cwd derivation for this run.
- Optionally, a hint about which external system the feature will land in (e.g. `--ext jira` or `--ext github`). When present, the planner embeds the relevant external-link conventions in the doc front matter so the ingestor can pick them up.

## Outputs

- **One top-level plan row** with `status='draft'` and a filesystem-safe `slug`, created via `planar plan create --slug <slug> --status draft`.
- **Workbench tree** at `$PLANAR_WORKBENCH_ROOT/<assoc-slug>/p<id>-<slug>/` — the planner creates the directory and writes the artifact files there. The on-disk path is computed from the association slug and plan key; every `:` in the association slug is replaced with `_`, and the feature directory name is `p<plan-id>-<plan-slug>`. To push the rendered workbench content to a registered external operational system (Jira, GitHub Issues), use `planar workbench publish <plan-id> --system <slug>` — or `planar-ext ext propagate <plan-id> --system <slug>` for full plan-subtree counterpart creation (`planar-ext ext propagate-one <system> --from <kind:id>` creates a single entity's counterpart).
- **Four artifact files**, each registered as a DB artifact row first (to obtain an `artifact_id`), then written to disk with canonical YAML front matter, then persisted via `planar artifact update --body @<filename>`:
  - `product-spec.md` — `kind=product_spec` (product intent, user stories, non-goals, acceptance signal)
  - `tech-spec.md` — `kind=tech_spec` (architecture, components, schema changes, decisions)
  - `roadmap.md` — `kind=roadmap` (flat milestone list with bulleted work items and `[touches: ...]` annotations)
  - `test-spec.md` — `kind=test_spec` (test strategy with flat `### Scenario: <title>` H3 scenarios — the canonical grammar — plus a coverage-gap checklist and test-surface-allocation table). Each scenario title names the coverage lens it exercises (happy / empty-null / error / edge); the four buckets are a reasoning tool, not document structure. Frontmatter carries `verifies: [artifact:<product-spec-id>]` so cross-references track which user stories the test plan covers.

  Every produced `.md` file carries a YAML front matter block between `---` delimiters at the top of the file. The canonical schema is the `front_matter` struct in [`src/engine/workbench/parse.cppm`](../src/engine/workbench/parse.cppm). Required fields for planner-written artifact files:

  ```yaml
  ---
  entity_kind: artifact
  entity_id: <artifact-id>        # integer returned by `planar artifact add`
  anchor_plan_id: <plan-id>       # integer returned by `planar plan create`
  title: <string>                 # human-readable title of this document
  status: draft
  artifact_kind: <product_spec|tech_spec|roadmap|test_spec>
  ---
  ```

  Files without valid front matter are treated as malformed by `workbench pull` and are rejected during sync. The planner must write correct front matter before calling `planar workbench push`.

- **Optional initial scenario files** under `scenarios/`, each registered via `planar scenario add`.
- **Workbench manifest** seeded via `planar workbench push <plan>`.

## Behavior

1. Confirm scope via `planar scope show`. If the cwd resolves to no registered scope and no `--scope` flag is supplied, surface a `question` and stop.
2. Create the anchor plan: `planar plan create "<derived title>" --slug <slug> --status draft --scope assoc:<slug>`. Capture `<plan-id>` from the output.
3. Create the workbench directory:
   ```
   mkdir -p "$PLANAR_WORKBENCH_ROOT/<assoc-slug>/p<plan-id>-<slug>/"
   ```
4. For each of `product-spec.md`, `tech-spec.md`, `roadmap.md`, and `test-spec.md`:
   a. Register an empty artifact to obtain an id:
      ```
      planar artifact add "<title>" --kind <kind> --plan <plan-id> --body ""
      ```
      Capture `<artifact-id>` from the output.
   b. Write `<filename>` in the workbench directory with canonical YAML front matter (using `<artifact-id>` and `<plan-id>`) followed by the planner-generated Markdown body. See [Doc shape](#doc-shape) for the required fields and section structure.
   c. Persist the full content (front matter + body) into the DB:
      ```
      planar artifact update <artifact-id> --body @path/to/<filename>
      ```
5. Optionally draft `scenarios/<scenario-slug>.md` for obvious top-level acceptance scenarios and register via `planar scenario add`.
6. Seed the manifest: `planar workbench push <plan-id>`.
7. Report to the user: the plan id, the workbench path, and the four artifact ids. Note that the user should review and edit the docs before invoking `pl-spec-ingest`.

## Status reporting

The planner emits a status string at each meaningful phase boundary using `planar-agent heartbeat --claim <token> --status "<text>"`. The canonical transitions and their strings follow the four authoring phases:

| Phase | Status string |
|-------|---------------|
| Authoring Phase 1 — product spec | `"drafting product-spec"` |
| Authoring Phase 2 — tech spec | `"drafting tech-spec"` |
| Authoring Phase 3 — roadmap | `"drafting roadmap"` |
| Authoring Phase 4 — test spec | `"drafting test-spec"` |
| All four documents written and pushed; waiting for operator review | `"ready for review"` |

See `agents/methodology.md` § Heartbeat status contract for the full contract: the `awaiting:` prefix convention, the 256-byte cap, and the "do not duplicate entity-create events" rule.

## Boundaries

- DB writes only through `planar` CLI verbs. No direct SQL.
- FS writes only inside the feature's workbench directory (`$PLANAR_WORKBENCH_ROOT/<assoc>/<plan-key>-<slug>/`). Never writes outside it.
- Does **not** create tasks. Task creation is the ingestor's job (`/pl-spec-ingest`).
- Does **not** contact external systems. No adapter calls.
- Does **not** modify the scope. The scope is read-only for the planner.
- Does **not** ingest or decompose. Planner output is a human-reviewable draft; the user controls when ingestion happens.

## Authoring phases

The four documents are authored in four sequential phases, each with its own role-narrowing prompt fragment. The planner is one agent, but each phase has explicit instructions about what to focus on and what to NOT think about. The phasing is what makes the four-document model honest: without it, product-spec drifts into implementation types, tech-spec re-litigates user stories, and test-spec proposes implementations instead of describing scenarios.

### Phase 1 — Product spec

You are authoring `product-spec.md`. Your job is to answer: **what does the operator see, do, and get out of this feature?** Frame everything in operator-observable terms. Identify users, walk through use cases, write acceptance signals as observable behavior, and list non-goals.

**While in this phase, do NOT:**

- Propose implementation types, SQL schemas, package layouts, or any implementation choice.
- Choose a migration number, file path, function signature, or library.
- Decide between two implementations — that's the tech-spec's job.
- Propose tests or scenarios — that's the test-spec's job.

If you find yourself reaching for any of those, stop, capture the question for the appropriate later phase (note it in `## Open questions`), and return to user-observable behavior.

### Phase 2 — Tech spec

You are authoring `tech-spec.md`. The product-spec is locked. Your job is to **choose the implementation, identify the schema delta, name the components, and record the decisions**. Cite the product-spec for context but do not re-litigate it.

**While in this phase, do NOT:**

- Re-litigate user-facing tradeoffs decided in product-spec.
- Author scenarios — those go in the test-spec.
- Expand non-goals beyond what product-spec set (you can add tech-side non-goals, but do not contradict the product non-goals).
- Defer architectural choices to the test-spec or the coder.

### Phase 3 — Roadmap

You are authoring `roadmap.md`. The product- and tech-specs are locked. Your job is to **sequence the work into milestones with bulleted tasks**. Each milestone is independently shippable. The roadmap is what the ingestor decomposes into the plan/task graph, so the bullets matter.

**While in this phase, do NOT:**

- Re-litigate architecture decisions (point at tech-spec sections).
- Re-litigate scenario coverage — that's the test-spec.
- Skip cross-references to the surfaces each milestone touches.

### Phase 4 — Test spec

You are authoring `test-spec.md`. The other three docs are locked. Your job is purely adversarial: **what could go wrong, what scenarios prove this works, what scenarios prove it doesn't**. The frontmatter `verifies: [artifact:<product-spec-id>]` is the structural argument that this document covers the user stories.

**While in this phase, do NOT:**

- Propose implementations of the tests (the test-coder writes the code; you describe the scenarios as observable behavior).
- Re-litigate features in product-spec or architecture in tech-spec.
- Skip any of the four return-path buckets without justification. Reason about each bucket explicitly per public function or user-visible flow — they are a **coverage-reasoning lens**, not document structure:
  1. **Happy path** — valid input, meaningful output.
  2. **Empty / null return** — valid input, legitimately empty output (not an error).
  3. **Error return** — operation cannot proceed.
  4. **Edge case** — boundary conditions (zero/one/max inputs, off-by-one, concurrent access).

  Author each scenario as a **flat `### Scenario: <title>` H3** (one H3 per scenario) naming the bucket in the title, e.g. `### Scenario: Happy path — foo returns bar`. Do NOT create `### <bucket>` group headers with nested `#### Scenario:` H4 children — that form is a parser-tolerated fallback, not the canonical grammar.

The coverage-gap checklist at the bottom of `test-spec.md` is the reviewer's structured surface for confirming each bucket is addressed (or marked N/A with justification) per function/flow.

#### Phase 4 self-check (before final emission)

Before emitting the test-spec as final, the planner walks every scenario it just wrote and confirms:

1. **Every `### Scenario:` block has a non-empty `**Verifies:**` field.** A missing or empty `**Verifies:**` line is a draft failure — go back and decide what the scenario verifies. A scenario without a verifies edge cannot participate in the strict ingest-preview coverage gate or, after ingestion, in the live per-milestone breakdown.
2. **Every slug cited in `**Verifies:** task:<slug>` exists as a `[slug: <slug>]` annotation on a roadmap bullet under this plan.** Slugs not declared in the roadmap are an unresolvable citation and the ingestor will reject the apply. Either add the missing `[slug:]` annotation to the corresponding roadmap bullet, or change the citation to a slug that does exist.
3. **Every roadmap bullet that names a testable behavior carries a `[slug: …]` annotation.** Bullets without slugs cannot be cited by stable name from the test-spec; pure-mechanical sweeps (mass renames, comment-only cleanups) are the only legitimate slug-free bullets.

These checks are confirmed against the workbench drafts by the strict JSON
preview. Preview is the default when `--apply` is absent, so this command does
not create task or scenario rows:

```
planar spec ingest <plan> --strict --json
```

Treat a non-zero exit, a non-empty `coverage.uncovered_task_slugs`, a non-empty
`coverage.orphan_scenarios`, or a non-empty top-level `slug_collisions` array as
a draft failure. Do not use `planar test-spec status` for this pre-ingest check:
it queries live task/scenario rows, which legitimately do not exist yet. After
ingestion has been applied, `planar test-spec status <plan> --json` becomes the
authoritative live-row coverage oracle.

## Doc shape

The planner emits parseable Markdown so the ingestor can re-read the same files reliably. Three conventions are load-bearing:

### `tech-spec.md` section structure

```markdown
---
entity_kind: artifact
entity_id: <artifact-id>
anchor_plan_id: <plan-id>
title: <title>
status: draft
artifact_kind: tech_spec
---

# <Feature Title> — Tech Spec

## Status
## Intent
## Non-Goals
## Concepts
## Architecture
## Components
## Schema Changes
## Open Questions

## Decisions

### <Decision Title>

<Decision body — rationale, context, constraints.>
```

The `## Decisions` H2 is the **only** section the ingestor reads to extract decisions. Each H3 heading becomes one `decisions` row linked to the anchor plan via `entity_links(relationship='derives-from')`. The planner must not embed decision text elsewhere in the document.

The `## Open Questions` H2 is parsed by `parse_tech_spec_open_questions` in [`src/engine/ingest/parse.cppm`](../src/engine/ingest/parse.cppm). Each H3 heading becomes one `questions` row. When an H3's body begins with a `Resolution:` marker (case-sensitive; must be the first non-blank token after the heading), the ingestor also creates a `decisions` row carrying the resolution text and flips the question to `answered`. Two forms are supported:

```markdown
## Open Questions

### Should we support multi-file ingestion?

Resolution: Yes — the CLI accepts a glob; each file is parsed independently and merged.

### Where do propagate caches live?

Resolution:
Under `~/.planar/cache/propagate/` by default, overridable via `PLANAR_CACHE_DIR`.
Old entries are pruned after 30 days.

### What encoding should workbench files use?

UTF-8 only. No explicit `Resolution:` marker here, so this question remains open.
```

The first two H3s above will each produce a `decisions` row and flip the question status to `answered`; the third stays `open`.

### `roadmap.md` section structure

```markdown
---
entity_kind: artifact
entity_id: <artifact-id>
anchor_plan_id: <plan-id>
title: <title>
status: draft
artifact_kind: roadmap
---

# <Feature Title> — Roadmap

## <Milestone Name>

<One-paragraph intent for this milestone.>

- <Work item description>
- <Work item touching multiple repos> [touches: acme/protos, acme/service]
- <Another work item>
```

Each H2 becomes a child plan (one per milestone). Each bullet becomes a task. The `[touches: repo-slug, ...]` annotation (bracketed, comma-separated repo slugs at the end of a bullet) becomes `entity_links(relationship='touches', from=task, to=repo)` rows. Bullets without an annotation fall back to association scope.

### `product-spec.md` front matter

All workbench files carry YAML front matter between `---` delimiters. The canonical schema is `front_matter` in [`src/engine/workbench/parse.cppm`](../src/engine/workbench/parse.cppm). Required fields for `product-spec.md`:

```yaml
---
entity_kind: artifact
entity_id: <artifact-id>
anchor_plan_id: <plan-id>
title: <title>
status: draft
artifact_kind: product_spec
---
```

The planner writes these fields after registering the artifact via `planar artifact add --body ""` and capturing the returned `<artifact-id>`. Files without valid front matter are treated as malformed by `workbench pull`; the planner must always write the front matter before calling `planar workbench push`.

## CLI commands composed

The verbs must be composed in this order to ensure every `.md` file carries canonical front matter before the workbench manifest is seeded:

1. `planar scope show` — confirm the cwd resolves to a registered scope (or that `--scope` was supplied); abort with a `question` if not.
2. `planar plan create "<derived title>" --slug <slug> --status draft [--scope assoc:<slug>]` — returns `<plan-id>`.
3. For each of `product-spec.md`, `tech-spec.md`, `roadmap.md`, and `test-spec.md`:
   1. `planar artifact add "<title>" --kind <kind> --plan <plan-id> --body ""` — returns `<artifact-id>`. Creates an empty-body placeholder row so the id is known before the file is written.
   2. Write `<filename>` in the workbench directory with canonical YAML front matter using the returned `<artifact-id>` and `<plan-id>`, followed by the planner-generated Markdown body.
   3. `planar artifact update <artifact-id> --body @<filename>` — persists the full content (front matter + body) into the DB.
4. (Optional) `planar scenario add <title> [--body <text>] [--scope assoc:<slug>]` — for each top-level acceptance scenario.
5. `planar workbench push <plan-id>` — seeds the workbench manifest. At this point both the DB and the FS carry identical content with valid front matter; subsequent `planar workbench pull` and `planar workbench status` invocations are clean.

Cross-scope writes require the scope checks defined by the stack's cross-scope-writes doctrine.
