---
description: Tier-to-model mapping for every supported vendor.
kind: doc
slug: models
---

# Models And Vendors

Agent specs in `agents/` reference abstract tiers (`small`, `medium`, `large`).
**This file is the source of truth for concrete vendor model IDs.**

This orchestration layer owns the catalog, the per-vendor presets, the display
labels, and spawn verification (`evals/candidate-spawn`). The `planar` binary owns opaque IDs, coordination state, evidence, outcomes,
and resolution — it is *told* which vendor and model were used and records them
as strings; it does not decide what is supported (decision 884).

A reader, human or agent, can resolve a tier to a concrete model from the Tier
Table below without consulting Planar configuration.

> **Do not ask Planar for a model.** Planar does not decide tier→model: it
> records the vendor and model an agent reports on
> `planar-agent claim --vendor <host> --model <candidate>`, stored verbatim and
> never validated against a supported list. **This file is the only source of
> tier→model.**
>
> `planar models routing`, `apply`, `candidates`, `list`, and `refresh` — along
> with the `[models.<vendor>]` catalog, the `[routing.*]` map, and the `[roles]`
> role→tier map — no longer exist. They were removed on the plan-950 epic, which
> landed on planar `master` (planar tasks 5613, 5640). The `models` domain is
> now `registry` + `evals` only.
>
> **Planar cannot regenerate this file.** `planar models sync-doc` — which used
> to rewrite the Tier Table *from* Planar config — was deleted (planar task
> 5616); the verb now errors as an unknown subcommand.
>
> That also removed a drift gate. **Nothing checks this table automatically.**
> Correct under the new ownership, since there is no longer an external
> authority to check it against — but it means a stale or internally
> inconsistent table will not be caught for you; the opt-in
> `make eval-candidate-spawn` lane is the closest gate.

## Tier Table

| Tier | Claude | Codex | Copilot | Gemini |
| ------ | ------ | ----- | ------- | ------ |
| small | claude-haiku-4-5 | gpt-5.6-luna | gpt-5-mini | gemini-3.1-flash |
| medium | claude-sonnet-5 | gpt-5.6-terra | gpt-5 | gemini-3.1-pro |
| large | claude-opus-5 | gpt-5.6-sol | claude-opus-4 | gemini-3.1-pro |

At-a-glance defaults: the model a caller gets resolving a bare `(vendor, tier)`
with no work type in hand. The full preset data is §Candidate Presets below.

These values are authoritative and hand-maintained here. Changing a tier default
is a routing-policy decision, not a bookkeeping update — see §Coder tier policy
and §Candidate use-cases within a tier before editing a cell.

## Candidate Presets

The per-vendor preset table. **This file owns this data**; it is hand-maintained
and ships to every installed surface through the ordinary render path, alongside
the orchestrator. There is no generator and no probe step — a candidate listed
here is a claim, not a verified fact, so it can go stale silently when a vendor
adds or drops a model. Re-check against §Notes On Identifiers when a host CLI
changes.

**Planar stores these ids opaquely.** It keeps a candidate id as bytes, never
parses it, and never checks it against a list of supported models — that is the
whole point of decision 884. A typo here is therefore recorded faithfully and
surfaces only when a dispatch tries to spawn it. Nothing downstream will catch
a wrong id for you, which is why `make eval-candidate-spawn` exists.

**A `Use when` cell may not rest on marketing.** Vendor capability claims,
benchmark scores, and release ordering are inadmissible as routing evidence: a
newer or higher-scoring model is not thereby the right one for a work type.
Only two things may justify a cell — a *product fact* (context window, effort
ladder, tokenizer, modality, price) or *recorded dispatch evidence* from
`planar models evals`. If neither is available, leave the cell defaulting and
say so.

**Evidence is cohort-local, and cohorts do not travel.** Planar scopes every
sample to the exact tuple `(project, validation policy version, vendor, role,
tier, work type, complexity)` and never pools across any dimension. A preset
promoted from evidence gathered in one project — or under one validation policy
— carries no authority in another, and re-promoting it elsewhere without fresh
evidence is the same unfounded prior this section forbids.

Candidates are **ordered within a tier**. `[0]` — the top row of each tier block
below — is the tier default and is what §Tier Table shows.

The `Use when` column is the point of this table: a tier says how much judgement
the work needs, `Use when` says which candidate in that tier to reach for. Empty
means no distinction has been drawn yet, and the tier default applies.

### claude

`Characteristics` are product facts from the Anthropic API reference (2026-07),
not capability claims — context window, effort ladder, thinking default, price.
Every ID below was verified spawn-safe on the installed host (§Notes On
Identifiers).

| Candidate | Tier | Use when | Characteristics |
| --------- | ---- | -------- | --------------- |
| `claude-opus-5` | large | **Tier default.** Bare `large` resolutions, reviewers, escalated coders. | 1M ctx · effort→`max` · thinking **on by default** · 512-tok cache min · $5/$25 |
| `claude-opus-4-8` | large | Prior Opus default; routable fallback. | 1M ctx · effort→`max` · adaptive thinking (off if unset) · $5/$25 |
| `claude-fable-5` | large | Wrong early judgement cascades hardest; shipped seed routes `architectural` here. | Mythos-class, above opus · thinking always on · 1M ctx · $10/$50 |
| `claude-opus-4-7` | large | Image-bearing work — the only `large` candidate with high-res vision (2576px), so dense diagrams and screenshots survive. **Not** for latency-sensitive dispatch: it has no fast mode. | 1M ctx · effort→`max` · high-res vision (2576px) · no fast mode · $5/$25 |
| `claude-opus-4-6` | large | **Never at `xhigh`** — its effort ladder stops at `max`, so an `xhigh` dispatch silently loses the effort it asked for. No recorded evidence yet separates it from 4-7/4-8 on anything else. | 1M ctx · effort→`high`/`max` only, **no `xhigh`** · $5/$25 |
| `claude-sonnet-5` | medium | **Tier default.** Everyday coder dispatches. | 1M ctx · effort→`max` incl. `xhigh` · adaptive on by default · new tokenizer (~30% more tokens for the same text) · $3/$15 |
| `claude-sonnet-4-6` | medium | Long-context runs where token cost dominates: same $/token as sonnet-5, but sonnet-5's tokenizer emits ~30% more tokens for identical text, so the same task bills more there. **Never at `xhigh`.** | 1M ctx · effort→`max`, no `xhigh` · older tokenizer · $3/$15 |
| `claude-haiku-4-5` | small | **Tier default.** Mechanical and routine work. | 200K ctx / 64K out — the only non-1M candidate · $1/$5 |

> **What these cells do and do not say.** Every `Use when` above is traceable to
> a product fact in the `Characteristics` column — an effort ladder that stops
> short, a vision resolution, a tokenizer that bills differently. None of them
> claims one candidate is *better* at a work type: that is a prior, and per
> §Candidate use-cases within a tier a prior needs recorded dispatch evidence,
> which does not exist yet for the `large` Claude block. So they answer "when is
> this one wrong" rather than "when is this one best", which is the question
> product facts can actually settle.
>
> Note the row ordering is *recency*, which is not a reason — a newer model is
> not automatically right for a given work type. When
> `planar models evals --vendor claude --role coder --tier large …` accumulates
> declared-experiment evidence in a cohort, promote what it shows into a
> §Work-type routes row; until it clears the confidence gate it reports
> `insufficient_data`, and the tier default stands.

### codex

`Use when` migrated verbatim from the labels Planar carried, which are being
removed with the rest of that surface (planar task 5613).

| Candidate | Tier | Use when |
| --------- | ---- | -------- |
| `gpt-5.6-sol` | large | **Tier default.** Frontier coding and research. |
| `gpt-5.5` | large | Prior frontier; routable fallback. |
| `gpt-5.6-terra` | medium | **Tier default.** Strong everyday coding. |
| `gpt-5.4` | medium | Prior everyday coding. |
| `gpt-5.6-luna` | small | **Tier default.** Fast, cost-efficient. |
| `gpt-5.4-mini` | small | Prior fast tier. |
| `gpt-5.3-codex-spark` | small | Ultra-fast. |

### copilot and gemini

> **INELIGIBLE FOR ROUTING — unverified.** No host probe has confirmed any id
> below spawns, because neither CLI is installed on this machine. They are
> recorded so the catalog is complete, not because they are usable: listing
> them unmarked would imply a check that never happened. Run
> `make eval-candidate-spawn` on a machine with the CLI present to promote them
> from claim to verified fact, then remove this marker.

No per-candidate distinctions are recorded either. One candidate per tier, so
the tier default is the only choice.

| Candidate | Tier | Vendor |
| --------- | ---- | ------ |
| `gpt-5-mini` | small | copilot |
| `gpt-5` | medium | copilot |
| `claude-opus-4` | large | copilot |
| `gemini-3.1-flash` | small | gemini |
| `gemini-3.1-pro` | medium | gemini |
| `gemini-3.1-pro` | large | gemini — same ID as `medium`; the tiers do not distinguish |

### Work-type routes

Which candidate a given work type selects within its tier. A work type with no
row falls back to that tier's `[0]`. Most rows below are identity — they name
the tier default — and are listed for completeness; the one substantive route is
`claude large architectural → claude-fable-5`, the prior described in
§Candidate use-cases within a tier.

| Vendor | Tier | Work type | Candidate |
| ------- | ------ | ------------- | --------- |
| claude | small | mechanical | `claude-haiku-4-5` |
| claude | medium | mechanical | `claude-sonnet-5` |
| claude | large | mechanical | `claude-opus-5` |
| claude | large | architectural | `claude-fable-5` |
| codex | small | mechanical | `gpt-5.6-luna` |
| codex | medium | mechanical | `gpt-5.6-terra` |
| codex | large | mechanical | `gpt-5.6-sol` |
| copilot | small | mechanical | `gpt-5-mini` |
| copilot | medium | mechanical | `gpt-5` |
| copilot | large | mechanical | `claude-opus-4` |
| gemini | small | mechanical | `gemini-3.1-flash` |
| gemini | medium | mechanical | `gemini-3.1-pro` |
| gemini | large | mechanical | `gemini-3.1-pro` |

These tables were transcribed from Planar's embedded defaults on 2026-07-29, at
which point they became this file's to maintain. Planar's copy is legacy and is
being removed (planar task 5613); it is no longer the source.

## Candidate lists and work-type routing

A tier may carry more than one candidate. When it does, the **work type** —
`schema | engine | architectural | cli | feature | mechanical`, the same
vocabulary used throughout this file — selects which candidate a given
dispatch gets; the tier's first candidate is its default. That model is
this file's and survives the migration. **The data lives in §Candidate Presets
above.**

### Where selection happens now

Selection is a table lookup in §Candidate Presets, performed by whoever is
dispatching — the orchestrator at its Phase 3 preview. There is no resolver and
no configuration layer on the orchestration side. Planar's `[models.<vendor>.<tier>]`
candidate lists, `[routing.<vendor>.<tier>]` work-type map, `[roles]` role→tier
map, and the `resolve(role, work_type)` entry point are removed on the plan-950
epic branch (planar task 5613); nothing here depends on whether they are still
present in the installed build.

What Planar still does is **record**: the dispatching agent reports the vendor
and model it used on `planar-agent claim --vendor <host> --model <candidate>`,
and Planar stores those strings verbatim without validating them. `planar models
evals` aggregates those recorded outcomes into a per-(work-type, candidate)
scorecard, which is the evidence this file's routing priors should eventually
rest on.

## Host capability boundary

Configuration expresses desired routing; the active host's subagent surface
defines what can actually be dispatched. Host-native orchestration never
crosses providers: a Codex host selects Codex candidates and a Claude host
selects Claude candidates even when `defaults.vendor` names another provider.
Cross-provider execution requires a separate external executor and is not the
host subagent path described by the orchestrator contract.

The preview records both the desired candidate and its concrete host binding.
If the host cannot represent that candidate, the row is `unsupported` and the
orchestrator stops for operator action. It must not silently substitute another
provider, tier, candidate, or agent type. Claude supports an invocation-level
model parameter, subject to the higher-precedence
`CLAUDE_CODE_SUBAGENT_MODEL` environment override. Codex agent roles may be
fixed to the model in their installed TOML projection; only agent types visible
in the current dispatch surface are valid bindings.

## Agent Assignments

Every installable agent under `agents/`, its authored `tier:` (the source of
truth — see §Conventions), its `capability:` (which drives the Codex
`sandbox_mode` and the Claude tool grant), and its primary work. Rows marked †
are the six **runtime-resolvable roles** whose tier is *also* carried in
`[roles]` of `src/lib/engine/config/defaults.toml` for the plan-540 model resolver;
those two copies MUST agree. Every other agent resolves its model straight from
this frontmatter via the render path.

Note this duplication is role→**tier**, not tier→model, and its fate is an
open question on planar task 5613: tier is a work-shape judgement rather than a
claim about provider support, so it may legitimately stay Planar-side even after
the model catalog goes. Until that is answered, keep the two copies in sync.

**Spelling.** This table hyphenates (`spec-reviewer`, `test-coder`), and
`--role` accepts that — `roles.cpp`'s parser normalises `-` to `_` before the
table lookup, deliberately, so both spellings resolve to the same role. The
CANONICAL wire form is the UNDERSCORED one (`spec_reviewer`, `test_coder`):
that is what `role_rows` stores and what every renderer emits, so passing
`--role spec-reviewer` and reading `role : spec_reviewer` back is expected,
not a bug (task 6341 — filed the other way round, as an "undocumented
underscored alias"; the underscored form is the canonical one and the
hyphenated one is the alias).

| Agent               | Tier   | Capability  | Primary work |
|---------------------|--------|-------------|--------------|
| `orchestrator`      | large  | coordinate  | Full lifecycle dispatch; phase selection, escalation, iteration-cap judgment |
| `planner`           | large  | write       | Drafts product / tech / test specs and the roadmap |
| `spec-reviewer`     | large  | write       | Adversarial spec review before ingestion |
| `ingestor`          | large  | coordinate  | Decomposes workbench specs into the task graph |
| `coder` †           | medium | write       | Scoped implementation (escalates to `large` per §Coder tier policy) |
| `test-coder` †      | large  | write       | Adversarial test authoring against the coder diff |
| `reviewer` †        | large  | read-only   | approve / request-changes / open-question / abort |
| `research`          | large  | read-only   | Read-only investigation dispatch; returns a cited findings brief |
| `janitor`           | medium | coordinate  | Merge, reconcile, cleanup, plan closeout |
| `documenter` †      | large  | read-only   | Proposes the doc worklist from repo drift |
| `doc-author` †      | large  | write       | Authors approved reference prose under `docs/` |
| `ext-sync`          | large  | coordinate  | Propagates a feature to Jira / GitHub Issues |
| `sync-reconciler` † | large  | coordinate  | Reconciles local/external sync conflicts |
| `importer`          | large  | write       | Translates an existing repo's planning content into Planar |
| `synthesizer`       | large  | write       | Synthesizes fresh planning artifacts via an LLM pass |
| `introspector`      | medium | coordinate  | Usage / friction introspection over redacted signal |
| `feedback-triager`  | large  | coordinate  | Triages redacted feedback findings |

## Conventions

- Tiers are coarse on purpose. Add a new tier (e.g. `small` for routine status updates, `xl` for adversarial review) only when an agent spec demonstrably needs it.
- Removing or renaming a tier requires updating every agent file under `agents/` and every vendor surface in the same change.
- The agent spec owns the tier; this file owns the tier-to-model resolution. Agents must not name concrete model identifiers directly.

## Coder tier policy

The coder defaults to `medium`, and the default is load-bearing, not a starting
bid. Planar's premise is that the hard reasoning happens upfront — in the spec,
the decomposition, and the locked decisions — so a well-decomposed task should
usually close at `medium`. The orchestrator proposes `large` per task as a
named exception, never as a batch default. Tier is independent of isolation;
even a `large` coder runs as a separately spawned subagent with blank context.
Routine implementation, interface additions, doc changes, and mechanical
sweeps do not warrant escalation.

Task-title vocabulary is never sufficient escalation evidence. `config`,
`module`, `composition root`, `engine`, `refactor`, and a large file count remain
`medium` when the spec has already made the design decisions. Every `large`
proposal cites a concrete unresolved schema, transaction, concurrency,
ownership, security, state-transition, resource-lifecycle, or architecture
judgment from the acceptance criteria or spec. If it cannot, the task remains
`medium`.

Work type and tier must agree: `schema`, `engine`, and `architectural` require
that cited signal and a `large` proposal; already-decided implementation is
`feature` at `medium`.

The escalation is keyed to the **type of work** in each task — not to the task
count, and not to the task's batch-mates. The orchestrator classifies each
task's dominant work type and proposes the corresponding tier in the Phase 3
dispatch preview:

| Work type | Tier | Signals (any one triggers the row) |
|-----------|------|------------------------------------|
| Schema / migration | large | new or edited migration files; a change to the schema-version contract; a CHECK-constraint or index redesign |
| Engine judgment | large | a transaction, concurrency, ownership, security, resource-lifecycle, status-transition, scope-resolution, or locked-capability-boundary change. Routine wiring that follows an existing pattern is `feature`; directory names alone are not escalation evidence. |
| Architectural | large | a new subsystem or binary; a cross-module diff touching many packages |
| CLI-surface change | medium | a new top-level verb, subcommand, or flag whose contract must be pinned by an integration test. The contract is pinned by the integration suite and the decomposition carries the design; a surface change that genuinely requires cross-cutting parser or design judgment classifies as `architectural` instead. |
| Feature (default) | medium | single-verb handler wiring, a bounded feature addition within an existing surface |
| Mechanical / docs | medium | renames, formatting sweeps, comment/doc-only edits, prose under `docs/`, workflow-surface text |

The one-word reason the orchestrator prints on every non-default (`large`) row
in the dispatch preview is the matching work-type name from this table
(`schema`, `engine`, `architectural`). The default (`medium`) rows carry
no reason. The operator may override any task's tier at the gate; the confirmed
`model_tiers` map is binding for dispatch (Axis C, below).

**Tiers are per-task; a cycle never inherits its highest task's tier.** The
former highest-row-wins rule (a mixed cycle escalates wholesale to the highest
tier present) is retired. When a proposed `grouped`/`single` cycle mixes
confirmed tiers, the orchestrator partitions the group by tier — one coder per
tier partition, dependency edges still ordering the dispatches — or falls back
to per-task dispatch (`strict` / `barrel-deferred`) for that cycle. Inflating a
`medium` task to `large` because of its batch-mates is prohibited; so is
silently folding a `large` task into a `medium` batch.

**Ambiguity escalates to the operator, not to a larger tier.** When the classifier
genuinely cannot decide a task's dominant work type (competing signals, an
under-specified task body), the orchestrator does NOT round up to `large`. It
renders the row as `tier: ?` with the competing signals named — e.g.
`(engine? feature? — touches a core subsystem but follows an existing
pattern)` — and the gate requires an explicit operator answer for that row
before any cycle containing it dispatches. Uncertainty is a routing question
for the operator, not a budget decision the orchestrator resolves by rounding
up.

**Opportunistic escalation on reviewer bounce.** When a `medium` cycle fails
two consecutive reviewer iterations and the failures read as capability gaps
(the coder misunderstands the design, not the spec being ambiguous), the
orchestrator may propose re-dispatching the remaining iterations at `large` —
surfaced at the next preview render with the reviewer evidence, never applied
silently. Spec ambiguity escalates to the user as an open question instead; a
bigger model does not fix an under-specified task.

Axis C is **operator-confirmed, never silent**. The orchestrator surfaces the proposed tier per task in the Phase 3 dispatch preview (the task-breakdown table's tier column, with a one-word reason on every non-default row), and the operator may override any task's tier before confirming. The confirmed assignment is binding: the orchestrator spawns each coder at the confirmed tier and never silently escalates or downgrades — a mid-plan re-proposal is surfaced at the next preview render. Confirmed assignments persist in the dispatch entry's metadata as a `model_tiers` map for cross-cycle stickiness. See [`skills/src/pl-orchestrator.md` §Dispatch preview and model tiers](../skills/src/pl-orchestrator.md) for the preview format.

## Candidate use-cases within a tier

When a tier carries more than one candidate, the routing map is where "which
one, for what" gets encoded — over the existing work-type vocabulary, never new
ad-hoc labels (the orchestrator's classifier and `planar models evals`'
scorecard both key on `schema | engine | architectural | cli | feature |
mechanical`). The shipped priors:

- **`claude-fable-5` vs `claude-opus-4-8`** (Claude `large`): opus is the tier
  default — reviewers, escalated coders, and every bare large resolution get
  opus. Fable is Mythos-class (above opus) and is routed only where a wrong
  early judgment cascades hardest: §Work-type routes routes `architectural` →
  fable. Widen (e.g. add a `schema` → fable row) or retract by editing that
  table here — it is the override path now that Planar's `[routing.*]` map is
  gone.
- **`gpt-5.6-sol` vs `gpt-5.5`** (Codex `large`): sol is the current frontier
  default; gpt-5.5 stays listed as a routable fallback candidate.
- These are **priors, not conclusions**. `planar models evals` aggregates
  completed dispatches into a per-(work-type, candidate) scorecard and emits
  preview-only routing recommendations — let accumulated dispatch history,
  not intuition, decide whether a routing entry earns its cost.

## Notes On Identifiers

- `claude-sonnet-5`, `claude-opus-4-8`, and `claude-fable-5` are the current Anthropic identifiers as of 2026-07. `claude-fable-5` is the Mythos-class tier above opus — kept as a routable `large` candidate, deliberately not the tier default.
- **Verified spawn-safe on the installed Claude host** (claude 2.1.220, probed 2026-07-28, question 884): `claude-opus-5`, `claude-opus-4-8`, `claude-opus-4-7`, `claude-opus-4-6`, `claude-sonnet-5`, `claude-sonnet-4-6`, `claude-sonnet-4-5`, `claude-haiku-4-5`, `claude-fable-5`. Each was spawned and returned a normal completion; an invalid control failed loudly, so a bad identifier is a visible error rather than a silent fallback.
  - **No date suffixes.** `claude-sonnet-4-6-20251114`-style strings are not the identifiers on this host.
  - **Bare aliases are not spawn-stable.** The host documents `opus`, `sonnet`, `haiku`, and `fable` as aliases for *the latest* model, so what they resolve to changes as models ship. Accept them as operator input if useful, but record and pin full identifiers.
  - This verification is host- and version-specific. Re-probe when the installed CLI changes rather than treating the list as permanent.
  - `claude-opus-5` is spawn-safe but is **not** currently a tier default — the Tier Table predates its verification. Promoting it is a routing-policy decision, not a bookkeeping fix.
- Codex and Copilot identifiers must be verified against each vendor's current model list periodically. Treat the values above as defaults, not guarantees.
- Vendors that expose Anthropic models (e.g. Copilot routing to `claude-opus-4`) should resolve to the closest available identifier on that vendor, not the Anthropic-native one.
