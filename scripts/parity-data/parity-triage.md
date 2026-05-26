# Parity Triage (Phase 2.5)

_Generated 2026-05-25T23:44:30Z by orchestrator dispatch on task 2364._

**Source:** `scripts/parity-data/parity-gap-report.json` (173 rows). Methodology: artifact 190 §"Reference oracle, not gospel".

## Methodology

Each of the 173 gap rows is classified into one of four buckets per artifact 190:

1. **Zig regression — Go is correct.** Phase 3 writes a failing integration test that pins Go's behavior; Phase 4 ports the fix to zig.
2. **Go bug — neither side should match Go.** Phase 3 pins the *intended* behavior (which may match neither current binary today).
3. **Intentional zig divergence.** Phase 3 writes a passing test pinning the zig contract; allowlist the diff in `scripts/parity-allowlist.txt` (a Phase 5 artifact).
4. **Cosmetic — help-text language, version SHAs, host paths, error-wording.** Allowlist only.

Rather than 173 standalone entries, this triage groups rows into **clusters** sharing a single triage decision. A cluster collapses {N rows × 1 decision} into {1 decision applied to N rows} — necessary discipline given the audit's wide-format pattern (most verbs were probed with the same 5 invocations).

Per-row classification is preserved in the cluster member lists below and in the appendix table for direct cite-back from Phase 3 tests.

## Distribution

| Bucket | Count | % |
|--------|------:|--:|
| Bucket 1 — Zig regression (Go correct) | 133 | 76% |
| Bucket 3 — Intentional zig divergence | 8 | 4% |
| Bucket 4 — Cosmetic | 32 | 18% |
| **Total** | **173** | 100% |

Note: Bucket 2 is empty in this audit. No row was triaged as 'Go is wrong here, neither binary's current behavior should be pinned'. The closest cases (`annotate` missing from Go, `agent` missing from both) are forward-evolution gaps, not bugs.

## Open questions gating this triage

Four planar questions were opened against plan 351; each gates a specific cluster's bucket assignment. Phase 3 should not write tests for the gated clusters until these resolve (or the recommendations are explicitly accepted).

| Question | Gates cluster | Recommendation |
|---|---|---|
| [Q234](#) — Bare parent-verb behavior (help+exit0 vs strict error) | A | Match Go (Bucket 1) |
| [Q235](#) — `planar health` rich field set + exit-1-on-DEGRADED | C | Match Go (Bucket 1) |
| [Q236](#) — Reinstate `plan next` (orchestrator skill dependency) | F-plan-next | Reinstate (Bucket 1) |
| [Q237](#) — `task add` / `question add` positional-title shape | F-q233-add-shape | Confirm intentional (Bucket 3); docs follow-up |

Until resolved, the cluster bucket assignments below carry the recommended bucket. If a question resolves against the recommendation, re-classify the cluster en masse.

## Clusters

Each cluster is a single triage decision applied to one or more rows. Member rows are listed inline so Phase 3 can iterate them directly.

### A-bare-parent-verb — Bare parent-verb invocation diverges (Go: help+exit0; Zig: 'unknown subcommand' + exit1)

**Bucket:** **1** (rec; gated by Q234)  **Rows:** 84  **Gate:** Q234

**Summary.** On every parent verb (plan, task, artifact, audit, capture, config, decision, doc, ext, links, local, question, scenario, scope, skills, spec, templates, test-spec, workbench, workspace, assoc), the four bare invocations (`<verb>`, `<verb> --json`, `<verb>` from real-cwd, `<verb> --json` from real-cwd) show the same pattern: Go's Cobra default prints the subcommand list + global flags and exits 0; zig's parser prints `error: unknown subcommand [in: <verb>]` to stderr and exits 1.

**Intended behavior / action.** Match Go: when a parent verb is invoked with no subcommand, print help to stdout and exit 0. The fix is a single change in `src/cli/parser.zig` — when subcommand dispatch fails to find a leaf, render the parent's help instead of returning UnknownSubcommand. Phase 3 writes one failing test per parent verb (`integration_tests/parity_<verb>_bare_help_test.zig`); Phase 4 lands the parser change.

<details><summary>84 member rows</summary>


- `[6]` `artifact/no-args` (fc=zig-failed, diff_bytes=1800)
- `[7]` `artifact/json` (fc=zig-failed, diff_bytes=1800)
- `[8]` `artifact/real-cwd` (fc=zig-failed, diff_bytes=1800)
- `[9]` `artifact/real-cwd-json` (fc=zig-failed, diff_bytes=1800)
- `[11]` `assoc/no-args` (fc=zig-failed, diff_bytes=1668)
- `[12]` `assoc/json` (fc=zig-failed, diff_bytes=1668)
- `[13]` `assoc/real-cwd` (fc=zig-failed, diff_bytes=1668)
- `[14]` `assoc/real-cwd-json` (fc=zig-failed, diff_bytes=1668)
- `[16]` `audit/no-args` (fc=zig-failed, diff_bytes=1348)
- `[17]` `audit/json` (fc=zig-failed, diff_bytes=1348)
- `[18]` `audit/real-cwd` (fc=zig-failed, diff_bytes=1348)
- `[19]` `audit/real-cwd-json` (fc=zig-failed, diff_bytes=1348)
- `[21]` `capture/no-args` (fc=zig-failed, diff_bytes=1667)
- `[22]` `capture/json` (fc=zig-failed, diff_bytes=1667)
- `[23]` `capture/real-cwd` (fc=zig-failed, diff_bytes=1667)
- `[24]` `capture/real-cwd-json` (fc=zig-failed, diff_bytes=1667)
- `[26]` `config/no-args` (fc=zig-failed, diff_bytes=1724)
- `[27]` `config/json` (fc=zig-failed, diff_bytes=1724)
- `[28]` `config/real-cwd` (fc=zig-failed, diff_bytes=1724)
- `[29]` `config/real-cwd-json` (fc=zig-failed, diff_bytes=1724)
- `[31]` `decision/no-args` (fc=zig-failed, diff_bytes=1823)
- `[32]` `decision/json` (fc=zig-failed, diff_bytes=1823)
- `[33]` `decision/real-cwd` (fc=zig-failed, diff_bytes=1823)
- `[34]` `decision/real-cwd-json` (fc=zig-failed, diff_bytes=1823)
- `[41]` `doc/no-args` (fc=zig-failed, diff_bytes=1844)
- `[42]` `doc/json` (fc=zig-failed, diff_bytes=1844)
- `[43]` `doc/real-cwd` (fc=zig-failed, diff_bytes=1844)
- `[44]` `doc/real-cwd-json` (fc=zig-failed, diff_bytes=1844)
- `[46]` `ext/no-args` (fc=zig-failed, diff_bytes=1484)
- `[47]` `ext/json` (fc=zig-failed, diff_bytes=1484)
- `[48]` `ext/real-cwd` (fc=zig-failed, diff_bytes=1484)
- `[49]` `ext/real-cwd-json` (fc=zig-failed, diff_bytes=1484)
- `[66]` `links/no-args` (fc=zig-failed, diff_bytes=1812)
- `[67]` `links/json` (fc=zig-failed, diff_bytes=1812)
- `[68]` `links/real-cwd` (fc=zig-failed, diff_bytes=1812)
- `[69]` `links/real-cwd-json` (fc=zig-failed, diff_bytes=1812)
- `[71]` `local/no-args` (fc=zig-failed, diff_bytes=2082)
- `[72]` `local/json` (fc=zig-failed, diff_bytes=2082)
- `[73]` `local/real-cwd` (fc=zig-failed, diff_bytes=2082)
- `[74]` `local/real-cwd-json` (fc=zig-failed, diff_bytes=2082)
- `[86]` `plan/no-args` (fc=zig-failed, diff_bytes=1653)
- `[87]` `plan/json` (fc=zig-failed, diff_bytes=1653)
- `[88]` `plan/real-cwd` (fc=zig-failed, diff_bytes=1653)
- `[89]` `plan/real-cwd-json` (fc=zig-failed, diff_bytes=1653)
- `[96]` `question/no-args` (fc=zig-failed, diff_bytes=1813)
- `[97]` `question/json` (fc=zig-failed, diff_bytes=1813)
- `[98]` `question/real-cwd` (fc=zig-failed, diff_bytes=1813)
- `[99]` `question/real-cwd-json` (fc=zig-failed, diff_bytes=1813)
- `[106]` `scenario/no-args` (fc=zig-failed, diff_bytes=1843)
- `[107]` `scenario/json` (fc=zig-failed, diff_bytes=1843)
- `[108]` `scenario/real-cwd` (fc=zig-failed, diff_bytes=1843)
- `[109]` `scenario/real-cwd-json` (fc=zig-failed, diff_bytes=1843)
- `[111]` `scope/no-args` (fc=zig-failed, diff_bytes=1572)
- `[112]` `scope/json` (fc=zig-failed, diff_bytes=1572)
- `[113]` `scope/real-cwd` (fc=zig-failed, diff_bytes=1572)
- `[114]` `scope/real-cwd-json` (fc=zig-failed, diff_bytes=1572)
- `[121]` `skills/no-args` (fc=zig-failed, diff_bytes=1572)
- `[122]` `skills/json` (fc=zig-failed, diff_bytes=1572)
- `[123]` `skills/real-cwd` (fc=zig-failed, diff_bytes=1572)
- `[124]` `skills/real-cwd-json` (fc=zig-failed, diff_bytes=1572)
- `[126]` `spec/no-args` (fc=zig-failed, diff_bytes=1257)
- `[127]` `spec/json` (fc=zig-failed, diff_bytes=1257)
- `[128]` `spec/real-cwd` (fc=zig-failed, diff_bytes=1257)
- `[129]` `spec/real-cwd-json` (fc=zig-failed, diff_bytes=1257)
- `[131]` `task/no-args` (fc=zig-failed, diff_bytes=2085)
- `[132]` `task/json` (fc=zig-failed, diff_bytes=2085)
- `[133]` `task/real-cwd` (fc=zig-failed, diff_bytes=2085)
- `[134]` `task/real-cwd-json` (fc=zig-failed, diff_bytes=2085)
- `[136]` `templates/no-args` (fc=zig-failed, diff_bytes=2135)
- `[137]` `templates/json` (fc=zig-failed, diff_bytes=2135)
- `[138]` `templates/real-cwd` (fc=zig-failed, diff_bytes=2135)
- `[139]` `templates/real-cwd-json` (fc=zig-failed, diff_bytes=2135)
- `[141]` `test-spec/no-args` (fc=zig-failed, diff_bytes=1388)
- `[142]` `test-spec/json` (fc=zig-failed, diff_bytes=1388)
- `[143]` `test-spec/real-cwd` (fc=zig-failed, diff_bytes=1388)
- `[144]` `test-spec/real-cwd-json` (fc=zig-failed, diff_bytes=1388)
- `[156]` `workbench/no-args` (fc=zig-failed, diff_bytes=2079)
- `[157]` `workbench/json` (fc=zig-failed, diff_bytes=2079)
- `[158]` `workbench/real-cwd` (fc=zig-failed, diff_bytes=2079)
- `[159]` `workbench/real-cwd-json` (fc=zig-failed, diff_bytes=2079)
- `[161]` `workspace/no-args` (fc=zig-failed, diff_bytes=1766)
- `[162]` `workspace/json` (fc=zig-failed, diff_bytes=1766)
- `[163]` `workspace/real-cwd` (fc=zig-failed, diff_bytes=1766)
- `[164]` `workspace/real-cwd-json` (fc=zig-failed, diff_bytes=1766)

</details>

### B-help-text-content-loss — `<verb> --help` lost Go's per-command prose descriptions

**Bucket:** **1** (with subordinate Bucket-4 for format-only diffs)  **Rows:** 32  **Gate:** (none — recommendation stands)

**Summary.** Every `<verb> --help` invocation (33 rows; 1 zig-only addition in cluster D) shows two superimposed differences: (a) zig dropped Go's prose lead-in (e.g. 'Manage tasks — the discrete units of work. Tasks may belong to a plan...') in favor of a one-line summary ('Manage tasks.'), and (b) format-only differences (USAGE: vs Usage:, FLAGS vs Flags, omitted global-flag enumeration in per-command help).

**Intended behavior / action.** Restore the Go prose descriptions in each verb's CLI registration; they document invariants (e.g. 'Status lifecycle: draft → active → paused / done / abandoned') that are part of the contract and currently survive only in the archive. Format-only differences (header style, global-flag omission) are subordinate Bucket-4 cosmetic and can be allowlisted as a single pattern. Phase 3 writes a test per verb that grep's for the lifecycle/scope/etc. prose; Phase 4 restores it.

<details><summary>32 member rows</summary>


- `[5]` `artifact/help` (fc=neither-failed, diff_bytes=2331)
- `[10]` `assoc/help` (fc=neither-failed, diff_bytes=1988)
- `[15]` `audit/help` (fc=neither-failed, diff_bytes=1633)
- `[20]` `capture/help` (fc=neither-failed, diff_bytes=1991)
- `[25]` `config/help` (fc=neither-failed, diff_bytes=1940)
- `[30]` `decision/help` (fc=neither-failed, diff_bytes=2362)
- `[35]` `demote/help` (fc=neither-failed, diff_bytes=1377)
- `[40]` `doc/help` (fc=neither-failed, diff_bytes=2255)
- `[45]` `ext/help` (fc=neither-failed, diff_bytes=1777)
- `[50]` `handoff/help` (fc=neither-failed, diff_bytes=2700)
- `[55]` `health/help` (fc=neither-failed, diff_bytes=1231)
- `[60]` `link/help` (fc=neither-failed, diff_bytes=2468)
- `[65]` `links/help` (fc=neither-failed, diff_bytes=2141)
- `[70]` `local/help` (fc=neither-failed, diff_bytes=2391)
- `[75]` `pl-import/help` (fc=neither-failed, diff_bytes=4835)
- `[80]` `pl-synthesize/help` (fc=neither-failed, diff_bytes=4235)
- `[85]` `plan/help` (fc=neither-failed, diff_bytes=2191)
- `[90]` `promote/help` (fc=neither-failed, diff_bytes=1426)
- `[95]` `question/help` (fc=neither-failed, diff_bytes=2279)
- `[100]` `resume/help` (fc=neither-failed, diff_bytes=1922)
- `[105]` `scenario/help` (fc=neither-failed, diff_bytes=2332)
- `[110]` `scope/help` (fc=neither-failed, diff_bytes=1886)
- `[115]` `search/help` (fc=neither-failed, diff_bytes=2356)
- `[120]` `skills/help` (fc=neither-failed, diff_bytes=1621)
- `[125]` `spec/help` (fc=neither-failed, diff_bytes=1282)
- `[130]` `task/help` (fc=neither-failed, diff_bytes=2845)
- `[135]` `templates/help` (fc=neither-failed, diff_bytes=2410)
- `[140]` `test-spec/help` (fc=neither-failed, diff_bytes=1412)
- `[145]` `tree/help` (fc=neither-failed, diff_bytes=4820)
- `[150]` `unlink/help` (fc=neither-failed, diff_bytes=1402)
- `[155]` `workbench/help` (fc=neither-failed, diff_bytes=2793)
- `[160]` `workspace/help` (fc=neither-failed, diff_bytes=2003)

</details>

### C-health-content-loss — `planar health` lost rich field set + DEGRADED classification

**Bucket:** **1** (rec; gated by Q235)  **Rows:** 4  **Gate:** Q235

**Summary.** Zig health reports schema_version, migration_count, handoffs_pending, db_ok. Go reports db_path, db_ok, schema_current (bool), integrity_ok, inflight_tasks (with resumable / NOT RESUMABLE breakdown), pending_handoffs (with stale count), and an `overall: OK|DEGRADED` classification that drives exit 1 when degraded. Zig never reports degradation; CI / oncall scrapes against `health --json` are silently broken.

**Intended behavior / action.** Restore Go's field set + classification + exit-1-on-DEGRADED. The fix lives in `src/engine/health.zig` (compute the additional counters from in-flight task / handoff tables) and `src/cmd/planar/handlers/health.zig` (render the new shape, exit 1 on DEGRADED). Phase 3 writes a test that creates an in-flight task with no next_action (forces NOT RESUMABLE), runs `health`, asserts exit=1 and `overall: DEGRADED`.

<details><summary>4 member rows</summary>


- `[56]` `health/no-args` (fc=go-failed, diff_bytes=473)
- `[57]` `health/json` (fc=neither-failed, diff_bytes=369)
- `[58]` `health/real-cwd` (fc=neither-failed, diff_bytes=336)
- `[59]` `health/real-cwd-json` (fc=neither-failed, diff_bytes=357)

</details>

### D-zig-only-verb — `annotate` verb is a zig-only addition (Go errors 'unknown command')

**Bucket:** **3** (intentional)  **Rows:** 5  **Gate:** (Q234 reflects bare-invocation rows)

**Summary.** The `annotate` verb (and all its subcommands) was added to zig after the Go archive cutoff (see CLAUDE.md command list: `annotate Manage source annotations`). Every `annotate <invocation>` row shows Go erroring with 'unknown command "annotate"' while zig either renders help (for --help) or hits the parent-verb pattern (Cluster A).

**Intended behavior / action.** Allowlist as intentional zig addition. Phase 3 writes a passing test asserting `planar annotate --help` exits 0 and lists the subcommands. No code change. The 4 bare-invocation rows (no-args / json / real-cwd / real-cwd-json) inherit the Cluster A decision once Q234 resolves.

<details><summary>5 member rows</summary>


- `[0]` `annotate/help` (fc=go-failed, diff_bytes=895)
- `[1]` `annotate/no-args` (fc=both-failed, diff_bytes=217)
- `[2]` `annotate/json` (fc=both-failed, diff_bytes=217)
- `[3]` `annotate/real-cwd` (fc=both-failed, diff_bytes=217)
- `[4]` `annotate/real-cwd-json` (fc=both-failed, diff_bytes=217)

</details>

### E-tree-cwd-derive — `planar tree` from a registered cwd returns global instead of the assoc scope

**Bucket:** **1** (anchor)  **Rows:** 2  **Gate:** (none — recommendation stands; Q to reviewer: is the JSON scope-node shape covered by an existing test-spec?)

**Summary.** The original M20 regression that motivated plan 351. The audit confirms it has not been fully closed: under the real-cwd fixture (registered as `assoc:parity-audit-fixture`), Go's `tree` returns the fixture's assoc; zig's `tree` returns `global`. The `tree --json` variant compounds: same cwd-derive symptom PLUS scope-node JSON pollution (zig emits zero-valued entity fields `id, slug, status, priority, artifact_kind, created_at, updated_at` on scope nodes that don't have those fields).

**Root cause (task 2375, discovered 2026-05-26 mid-Phase-3 dispatch).** The symptom is in `tree` output but the bug is in `init` vs `assoc add` path-handling divergence. On macOS `/var/folders/...` symlinks to `/private/var/folders/...`. Zig's `init --allow-no-repo` calls `realPath()` on the cwd and stores `/private/var/folders/...` in the `projects.root_path` column. Zig's `assoc add <slug> <path>` stores the literal `<path>` argument (`/var/folders/...`) without canonicalization. The two paths differ, so `assoc add` triggers a *second* project-row insert; the assoc binds to the literal-path row; cwd-derive (from inside the directory) finds the realpath-row; no membership match → falls back to global. Go binary stores literal paths consistently in both `init` and `assoc add` — single project row, binding works, tree resolves correctly. `tree.zig`'s cwd-derive call site at line 77 is already fixed (commit `bfa3abc` verified independently); the symptom never disappears because the binding is broken upstream.

**Intended behavior / action.** Policy decision 2026-05-26 (operator): match Go (option a). Drop the `realPath()` call in `init`'s cwd handling; store whatever the shell passed. Sub-fixes after that lands: (1) augment `cwd_scope_test.zig` with a second test that uses a non-canonical fixture path (the existing test dodges the bug via `harness.zig:tmpAbsPath()`'s `realPath()`). (2) Sanitize the `tree --json` scope node to only emit `kind, title, scope_kind, scope_id, scope_label, children` (the JSON-pollution half is independent of the path-canonicalization issue and lives in tree.zig's JSON renderer).

<details><summary>2 member rows</summary>


- `[148]` `tree/real-cwd` (fc=neither-failed, diff_bytes=139)
- `[149]` `tree/real-cwd-json` (fc=neither-failed, diff_bytes=416)

</details>

### K-tree-render-drift — `planar tree` no-args / json: global-tree ordering + title-truncation drift

**Bucket:** **1** (with possible follow-up question)  **Rows:** 2  **Gate:** (possible follow-up if reviewer disagrees)

**Summary.** From a non-registered cwd both binaries fall back to global, both produce large trees, but they diverge in two systematic ways: (a) zig groups all children of a kind together (`artifact:1, artifact:2, ..., artifact:183, ..., question:1, question:2, ...`) while Go interleaves by creation order; (b) Go truncates long titles at ~80 chars with `…`, zig prints full titles.

**Intended behavior / action.** Pick one canonical behavior and pin it. Recommended: match Go on both — creation-order interleaving preserves the temporal narrative of the tree, and ~80-char truncation keeps the tree scannable. If the reviewer disagrees on either, open a follow-up question before writing the Phase 3 test; do not enshrine zig's drift by default. Affects only the 2 tree rows.

<details><summary>2 member rows</summary>


- `[146]` `tree/no-args` (fc=neither-failed, diff_bytes=463983)
- `[147]` `tree/json` (fc=neither-failed, diff_bytes=2150186)

</details>

### J-resume-content-loss — `planar resume` packet missing Section 4 (Operational Plane) external_links rendering

**Bucket:** **1**  **Rows:** 2  **Gate:** (none — recommendation stands)

**Summary.** Both binaries produce a resume packet for task 1 (no-args fixture) but zig's Section 4 says 'operational plane not available in M7 build (external plane lands in M8)' while Go renders `link:6 ext:rdrsss/planar#1 sync:never`. The `--json` variant is worse: zig returns a 1-line minimal JSON while Go returns the full 180-line packet with all 8 sections. Either M8 work was never ported to zig, or the M7-gate string is leftover dead text.

**Intended behavior / action.** Restore the external_links rendering in `src/engine/runtime/resume.zig`'s Section 4 path; remove the 'M7 build' guard string. For `--json`, restore the full 8-section packet shape. Phase 3 writes a test that creates a task with an external_link, runs `resume`, asserts the link appears in Section 4 and in the JSON packet.

<details><summary>2 member rows</summary>


- `[101]` `resume/no-args` (fc=neither-failed, diff_bytes=7081)
- `[102]` `resume/json` (fc=neither-failed, diff_bytes=16451)

</details>

### F-plan-next — `planar plan next <plan>` missing in zig (orchestrator skill dependency)

**Bucket:** **1** (rec; gated by Q236)  **Rows:** 1  **Gate:** Q236

**Summary.** The agents/orchestrator.md role spec and pl-orchestrator skill surfaces all reference `planar plan next <plan>` as the claim-aware next-task verb. Go has it; zig dropped it. This dispatch session itself hit the gap.

**Intended behavior / action.** Port the Go `plan next` handler to zig. Phase 3 writes a failing test asserting `plan next <id>` returns the highest-priority `todo`/`paused` task (claim-awareness layer optional pending the separate `planar agent claim` decision).

<details><summary>1 member rows</summary>


- `[165]` `plan/q233-plan-next` (fc=zig-failed, diff_bytes=1653)

</details>

### F-q233-add-shape — `task add` / `question add` use positional-title + flag-body (Go used all-flag)

**Bucket:** **3** (rec; gated by Q237)  **Rows:** 3  **Gate:** Q237

**Summary.** Zig redesigned the `add` verbs to take `<title>` as a positional argument with body/scope/plan as flags. Go used all-flag (`--title --body --plan`). The shape change is consistent and ergonomic but breaks scripts/skills written against the Go shape. A sub-decision: zig's `question add` dropped `--plan` entirely (operator must call `question link` to associate); this feels accidentally removed rather than redesigned.

**Intended behavior / action.** Confirm Bucket 3 for the positional-title shape; allowlist the 3 audit rows. Open a follow-up to docs/cli-reference.md to highlight the difference for skill-author migration. Separately, re-add `--plan` to `question add` (one-shot creation + link is the common path).

<details><summary>3 member rows</summary>


- `[170]` `question/q233-add-plan-flag` (fc=both-failed, diff_bytes=185)
- `[171]` `task/q233-add-editor-false` (fc=both-failed, diff_bytes=186)
- `[172]` `task/q233-add-no-editor` (fc=both-failed, diff_bytes=186)

</details>

### F-exit-code-not-found — `<verb> <missing-entity>` exit code drift (zig=4 or 2; Go=1)

**Bucket:** **1**  **Rows:** 3  **Gate:** (none — recommendation stands)

**Summary.** Three rows show zig returning a non-1 exit code on 'entity not found' errors where Go returns 1: `test-spec status 351` (Go=1 zig=4), `resume <missing-task>` (Go=1 zig=2), `resume --json <missing-task>` (Go=1 zig=2). Exit-code consistency is part of the script contract.

**Intended behavior / action.** Pin exit code 1 on 'entity not found' across all verbs; update the relevant handlers to return the canonical NotFound exit. Phase 3 writes a test per affected verb. Likely caused by uniform error-mapping in `src/cmd/planar/exit.zig`.

<details><summary>3 member rows</summary>


- `[103]` `resume/real-cwd` (fc=both-failed, diff_bytes=31)
- `[104]` `resume/real-cwd-json` (fc=both-failed, diff_bytes=31)
- `[169]` `test-spec/q233-status-positional` (fc=both-failed, diff_bytes=224)

</details>

### H-handoff-json-shape — `handoff --json` missing `failures` array field

**Bucket:** **1**  **Rows:** 1  **Gate:** (none)

**Summary.** Zig handoff --json returns `{ok, snapshot_id, handoff_id, status, resumable}` (compact, single-line). Go returns the same fields plus a `failures` array (pretty-printed). The `failures` array is load-bearing for the validate-handoff flow (lists why a snapshot isn't resumable). Pretty-print vs compact is subordinate Bucket-4 cosmetic.

**Intended behavior / action.** Add `failures: []` to the zig handoff --json output in `src/cmd/planar/handlers/handoff.zig`. Phase 3 writes a test that creates a snapshot with a known validation failure and asserts `failures: [...]` appears.

<details><summary>1 member rows</summary>


- `[52]` `handoff/json` (fc=neither-failed, diff_bytes=241)

</details>

### H-handoff-cwd-fixture — `handoff` both-failed under real-cwd fixture

**Bucket:** **1** (needs investigation)  **Rows:** 2  **Gate:** (potentially Q to reviewer on fixture vs verb behavior)

**Summary.** Both Go and zig handoff fail under the real-cwd fixture (both-failed, ~212-byte diff containing only stderr + exit). The fixture does not have an active task, so handoff cannot capture state. Either the fixture should pre-create a task (Phase 1 harness gap) or handoff should produce a structured 'no active task' error rather than a generic failure.

**Intended behavior / action.** Extend the Phase 1 fixture helpers to optionally seed a task; or have handoff emit a structured `no active task` error. Phase 3 test depends on the fixture decision. Both rows.

<details><summary>2 member rows</summary>


- `[53]` `handoff/real-cwd` (fc=both-failed, diff_bytes=212)
- `[54]` `handoff/real-cwd-json` (fc=both-failed, diff_bytes=212)

</details>

### G-sequence-id-drift — `handoff` (no-args, text) snapshot/handoff ID sequence drift across runs

**Bucket:** **4** (cosmetic)  **Rows:** 1  **Gate:** (none)

**Summary.** The text output of `handoff` shows `snapshot: 4 vendor: cli` (Go) vs `snapshot: 5 vendor: cli` (zig) because the two binaries run against different sub-DBs in the audit harness and produce different sequence IDs. Not part of the contract.

**Intended behavior / action.** Allowlist as known audit-noise pattern; add a regex to the Phase-5 allowlist for `snapshot: \d+ vendor:` and `handoff: \d+ status:` lines. No verb-side change.

<details><summary>1 member rows</summary>


- `[51]` `handoff/no-args` (fc=neither-failed, diff_bytes=220)

</details>

### F-agent-missing — `planar agent` verb missing from both binaries (q233 forward-looking probe)

**Bucket:** **4** (cosmetic)  **Rows:** 2  **Gate:** (M-scope follow-up — out of Phase 2.5 scope)

**Summary.** The q233 probes `agent` (top-level) and `agent ps` against both binaries. Neither implements `agent` today. Both error, with different wording (Go: 'unknown command "agent" for "planar"' / Zig: 'UnknownSubcommand'). The verb is referenced in the orchestrator skill (`planar agent claim`) but is a larger M-scope decision filed separately.

**Intended behavior / action.** Allowlist the wording difference. Track the missing-verb decision under a separate plan/question (not part of Phase 2.5 triage). 2 rows.

<details><summary>2 member rows</summary>


- `[166]` `agent/q233-agent-top` (fc=both-failed, diff_bytes=212)
- `[167]` `agent/q233-agent-ps` (fc=both-failed, diff_bytes=212)

</details>

### F-flag-wording — `test-spec status --plan 351` flag-rejected with different wording

**Bucket:** **4** (cosmetic)  **Rows:** 1  **Gate:** (rolled up to Q237)

**Summary.** Both binaries reject `--plan` on `test-spec status` (positional argument expected). Go: `unknown flag: --plan`. Zig: `UnknownFlag`. The actual verb shape decision is covered by Q237.

**Intended behavior / action.** Allowlist wording difference. 1 row.

<details><summary>1 member rows</summary>


- `[168]` `test-spec/q233-status-plan-flag` (fc=both-failed, diff_bytes=184)

</details>

### I-leaf-positional-error-wording — Leaf verbs reject bare invocation with different positional/flag error wording

**Bucket:** **4** (cosmetic)  **Rows:** 28  **Gate:** (none)

**Summary.** Every leaf verb that requires a positional argument or a flag (`demote`, `link`, `unlink`, `promote`, `pl-import`, `pl-synthesize`, `search`) rejects bare invocation, bare --json, and the two real-cwd variants. Both binaries refuse the invocation; only the error wording differs (Go: 'accepts N arg(s), received 0' / Zig: 'MissingRequiredPositional' / 'MissingRequired').

**Intended behavior / action.** Allowlist all 28 rows as wording-only cosmetic. Per-verb test (Phase 3) is unnecessary; one cross-verb test asserting 'bare invocation of any leaf verb exits non-zero' suffices as a regression guard.

<details><summary>28 member rows</summary>


- `[36]` `demote/no-args` (fc=both-failed, diff_bytes=215)
- `[37]` `demote/json` (fc=both-failed, diff_bytes=215)
- `[38]` `demote/real-cwd` (fc=both-failed, diff_bytes=215)
- `[39]` `demote/real-cwd-json` (fc=both-failed, diff_bytes=215)
- `[61]` `link/no-args` (fc=both-failed, diff_bytes=198)
- `[62]` `link/json` (fc=both-failed, diff_bytes=198)
- `[63]` `link/real-cwd` (fc=both-failed, diff_bytes=198)
- `[64]` `link/real-cwd-json` (fc=both-failed, diff_bytes=198)
- `[76]` `pl-import/no-args` (fc=both-failed, diff_bytes=221)
- `[77]` `pl-import/json` (fc=both-failed, diff_bytes=221)
- `[78]` `pl-import/real-cwd` (fc=both-failed, diff_bytes=221)
- `[79]` `pl-import/real-cwd-json` (fc=both-failed, diff_bytes=221)
- `[81]` `pl-synthesize/no-args` (fc=both-failed, diff_bytes=221)
- `[82]` `pl-synthesize/json` (fc=both-failed, diff_bytes=221)
- `[83]` `pl-synthesize/real-cwd` (fc=both-failed, diff_bytes=221)
- `[84]` `pl-synthesize/real-cwd-json` (fc=both-failed, diff_bytes=221)
- `[91]` `promote/no-args` (fc=both-failed, diff_bytes=198)
- `[92]` `promote/json` (fc=both-failed, diff_bytes=198)
- `[93]` `promote/real-cwd` (fc=both-failed, diff_bytes=198)
- `[94]` `promote/real-cwd-json` (fc=both-failed, diff_bytes=198)
- `[116]` `search/no-args` (fc=both-failed, diff_bytes=217)
- `[117]` `search/json` (fc=both-failed, diff_bytes=217)
- `[118]` `search/real-cwd` (fc=both-failed, diff_bytes=217)
- `[119]` `search/real-cwd-json` (fc=both-failed, diff_bytes=217)
- `[151]` `unlink/no-args` (fc=both-failed, diff_bytes=219)
- `[152]` `unlink/json` (fc=both-failed, diff_bytes=219)
- `[153]` `unlink/real-cwd` (fc=both-failed, diff_bytes=219)
- `[154]` `unlink/real-cwd-json` (fc=both-failed, diff_bytes=219)

</details>

## Appendix: per-row classification table

Every row in `parity-gap-report.json` cited here for direct Phase-3 reference.

| Row | Verb | Invocation | failure_class | Bucket | Cluster | Action |
|----:|------|------------|---------------|:------:|---------|--------|
| 0 | `annotate` | `help` | `go-failed` | 3 | `D-zig-only-verb` | Intentional: zig added 'annotate' post-archive cutoff. |
| 1 | `annotate` | `no-args` | `both-failed` | 3 | `D-zig-only-verb` | Intentional: zig added 'annotate'; bare invocation hits Cluster A (Q234). |
| 2 | `annotate` | `json` | `both-failed` | 3 | `D-zig-only-verb` | Intentional: zig added 'annotate'; bare invocation hits Cluster A (Q234). |
| 3 | `annotate` | `real-cwd` | `both-failed` | 3 | `D-zig-only-verb` | Intentional: zig added 'annotate'; bare invocation hits Cluster A (Q234). |
| 4 | `annotate` | `real-cwd-json` | `both-failed` | 3 | `D-zig-only-verb` | Intentional: zig added 'annotate'; bare invocation hits Cluster A (Q234). |
| 5 | `artifact` | `help` | `neither-failed` | 1 | `B-help-text-content-loss` | Restore Go per-command prose descriptions; format-only cosmetic is subordinate B |
| 6 | `artifact` | `no-args` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 7 | `artifact` | `json` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 8 | `artifact` | `real-cwd` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 9 | `artifact` | `real-cwd-json` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 10 | `assoc` | `help` | `neither-failed` | 1 | `B-help-text-content-loss` | Restore Go per-command prose descriptions; format-only cosmetic is subordinate B |
| 11 | `assoc` | `no-args` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 12 | `assoc` | `json` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 13 | `assoc` | `real-cwd` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 14 | `assoc` | `real-cwd-json` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 15 | `audit` | `help` | `neither-failed` | 1 | `B-help-text-content-loss` | Restore Go per-command prose descriptions; format-only cosmetic is subordinate B |
| 16 | `audit` | `no-args` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 17 | `audit` | `json` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 18 | `audit` | `real-cwd` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 19 | `audit` | `real-cwd-json` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 20 | `capture` | `help` | `neither-failed` | 1 | `B-help-text-content-loss` | Restore Go per-command prose descriptions; format-only cosmetic is subordinate B |
| 21 | `capture` | `no-args` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 22 | `capture` | `json` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 23 | `capture` | `real-cwd` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 24 | `capture` | `real-cwd-json` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 25 | `config` | `help` | `neither-failed` | 1 | `B-help-text-content-loss` | Restore Go per-command prose descriptions; format-only cosmetic is subordinate B |
| 26 | `config` | `no-args` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 27 | `config` | `json` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 28 | `config` | `real-cwd` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 29 | `config` | `real-cwd-json` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 30 | `decision` | `help` | `neither-failed` | 1 | `B-help-text-content-loss` | Restore Go per-command prose descriptions; format-only cosmetic is subordinate B |
| 31 | `decision` | `no-args` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 32 | `decision` | `json` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 33 | `decision` | `real-cwd` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 34 | `decision` | `real-cwd-json` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 35 | `demote` | `help` | `neither-failed` | 1 | `B-help-text-content-loss` | Restore Go per-command prose descriptions; format-only cosmetic is subordinate B |
| 36 | `demote` | `no-args` | `both-failed` | 4 | `I-leaf-positional-error-wording` | Cosmetic: both reject with positional/flag errors; wording differs. |
| 37 | `demote` | `json` | `both-failed` | 4 | `I-leaf-positional-error-wording` | Cosmetic: both reject with positional/flag errors; wording differs. |
| 38 | `demote` | `real-cwd` | `both-failed` | 4 | `I-leaf-positional-error-wording` | Cosmetic: both reject with positional/flag errors; wording differs. |
| 39 | `demote` | `real-cwd-json` | `both-failed` | 4 | `I-leaf-positional-error-wording` | Cosmetic: both reject with positional/flag errors; wording differs. |
| 40 | `doc` | `help` | `neither-failed` | 1 | `B-help-text-content-loss` | Restore Go per-command prose descriptions; format-only cosmetic is subordinate B |
| 41 | `doc` | `no-args` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 42 | `doc` | `json` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 43 | `doc` | `real-cwd` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 44 | `doc` | `real-cwd-json` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 45 | `ext` | `help` | `neither-failed` | 1 | `B-help-text-content-loss` | Restore Go per-command prose descriptions; format-only cosmetic is subordinate B |
| 46 | `ext` | `no-args` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 47 | `ext` | `json` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 48 | `ext` | `real-cwd` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 49 | `ext` | `real-cwd-json` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 50 | `handoff` | `help` | `neither-failed` | 1 | `B-help-text-content-loss` | Restore Go per-command prose descriptions; format-only cosmetic is subordinate B |
| 51 | `handoff` | `no-args` | `neither-failed` | 4 | `G-sequence-id-drift` | Cosmetic: snapshot/handoff ID drift across runs. |
| 52 | `handoff` | `json` | `neither-failed` | 1 | `H-handoff-json-shape` | Bucket-1: missing 'failures' field in zig handoff --json. |
| 53 | `handoff` | `real-cwd` | `both-failed` | 1 | `H-handoff-cwd-fixture` | Bucket-1: handoff both-failed under real-cwd fixture; investigate. |
| 54 | `handoff` | `real-cwd-json` | `both-failed` | 1 | `H-handoff-cwd-fixture` | Bucket-1: same as handoff/real-cwd. |
| 55 | `health` | `help` | `neither-failed` | 1 | `B-help-text-content-loss` | Restore Go per-command prose descriptions; format-only cosmetic is subordinate B |
| 56 | `health` | `no-args` | `go-failed` | 1 | `C-health-content-loss` | Gated by Q235; rec: restore Go's rich field set + exit-1-on-DEGRADED. |
| 57 | `health` | `json` | `neither-failed` | 1 | `C-health-content-loss` | Gated by Q235; rec: restore Go's rich field set + exit-1-on-DEGRADED. |
| 58 | `health` | `real-cwd` | `neither-failed` | 1 | `C-health-content-loss` | Gated by Q235; rec: restore Go's rich field set + exit-1-on-DEGRADED. |
| 59 | `health` | `real-cwd-json` | `neither-failed` | 1 | `C-health-content-loss` | Gated by Q235; rec: restore Go's rich field set + exit-1-on-DEGRADED. |
| 60 | `link` | `help` | `neither-failed` | 1 | `B-help-text-content-loss` | Restore Go per-command prose descriptions; format-only cosmetic is subordinate B |
| 61 | `link` | `no-args` | `both-failed` | 4 | `I-leaf-positional-error-wording` | Cosmetic: both reject with positional/flag errors; wording differs. |
| 62 | `link` | `json` | `both-failed` | 4 | `I-leaf-positional-error-wording` | Cosmetic: both reject with positional/flag errors; wording differs. |
| 63 | `link` | `real-cwd` | `both-failed` | 4 | `I-leaf-positional-error-wording` | Cosmetic: both reject with positional/flag errors; wording differs. |
| 64 | `link` | `real-cwd-json` | `both-failed` | 4 | `I-leaf-positional-error-wording` | Cosmetic: both reject with positional/flag errors; wording differs. |
| 65 | `links` | `help` | `neither-failed` | 1 | `B-help-text-content-loss` | Restore Go per-command prose descriptions; format-only cosmetic is subordinate B |
| 66 | `links` | `no-args` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 67 | `links` | `json` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 68 | `links` | `real-cwd` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 69 | `links` | `real-cwd-json` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 70 | `local` | `help` | `neither-failed` | 1 | `B-help-text-content-loss` | Restore Go per-command prose descriptions; format-only cosmetic is subordinate B |
| 71 | `local` | `no-args` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 72 | `local` | `json` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 73 | `local` | `real-cwd` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 74 | `local` | `real-cwd-json` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 75 | `pl-import` | `help` | `neither-failed` | 1 | `B-help-text-content-loss` | Restore Go per-command prose descriptions; format-only cosmetic is subordinate B |
| 76 | `pl-import` | `no-args` | `both-failed` | 4 | `I-leaf-positional-error-wording` | Cosmetic: both reject with positional/flag errors; wording differs. |
| 77 | `pl-import` | `json` | `both-failed` | 4 | `I-leaf-positional-error-wording` | Cosmetic: both reject with positional/flag errors; wording differs. |
| 78 | `pl-import` | `real-cwd` | `both-failed` | 4 | `I-leaf-positional-error-wording` | Cosmetic: both reject with positional/flag errors; wording differs. |
| 79 | `pl-import` | `real-cwd-json` | `both-failed` | 4 | `I-leaf-positional-error-wording` | Cosmetic: both reject with positional/flag errors; wording differs. |
| 80 | `pl-synthesize` | `help` | `neither-failed` | 1 | `B-help-text-content-loss` | Restore Go per-command prose descriptions; format-only cosmetic is subordinate B |
| 81 | `pl-synthesize` | `no-args` | `both-failed` | 4 | `I-leaf-positional-error-wording` | Cosmetic: both reject with positional/flag errors; wording differs. |
| 82 | `pl-synthesize` | `json` | `both-failed` | 4 | `I-leaf-positional-error-wording` | Cosmetic: both reject with positional/flag errors; wording differs. |
| 83 | `pl-synthesize` | `real-cwd` | `both-failed` | 4 | `I-leaf-positional-error-wording` | Cosmetic: both reject with positional/flag errors; wording differs. |
| 84 | `pl-synthesize` | `real-cwd-json` | `both-failed` | 4 | `I-leaf-positional-error-wording` | Cosmetic: both reject with positional/flag errors; wording differs. |
| 85 | `plan` | `help` | `neither-failed` | 1 | `B-help-text-content-loss` | Restore Go per-command prose descriptions; format-only cosmetic is subordinate B |
| 86 | `plan` | `no-args` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 87 | `plan` | `json` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 88 | `plan` | `real-cwd` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 89 | `plan` | `real-cwd-json` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 90 | `promote` | `help` | `neither-failed` | 1 | `B-help-text-content-loss` | Restore Go per-command prose descriptions; format-only cosmetic is subordinate B |
| 91 | `promote` | `no-args` | `both-failed` | 4 | `I-leaf-positional-error-wording` | Cosmetic: both reject with positional/flag errors; wording differs. |
| 92 | `promote` | `json` | `both-failed` | 4 | `I-leaf-positional-error-wording` | Cosmetic: both reject with positional/flag errors; wording differs. |
| 93 | `promote` | `real-cwd` | `both-failed` | 4 | `I-leaf-positional-error-wording` | Cosmetic: both reject with positional/flag errors; wording differs. |
| 94 | `promote` | `real-cwd-json` | `both-failed` | 4 | `I-leaf-positional-error-wording` | Cosmetic: both reject with positional/flag errors; wording differs. |
| 95 | `question` | `help` | `neither-failed` | 1 | `B-help-text-content-loss` | Restore Go per-command prose descriptions; format-only cosmetic is subordinate B |
| 96 | `question` | `no-args` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 97 | `question` | `json` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 98 | `question` | `real-cwd` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 99 | `question` | `real-cwd-json` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 100 | `resume` | `help` | `neither-failed` | 1 | `B-help-text-content-loss` | Restore Go per-command prose descriptions; format-only cosmetic is subordinate B |
| 101 | `resume` | `no-args` | `neither-failed` | 1 | `J-resume-content-loss` | Bucket-1: Section 4 (Operational Plane) missing in zig resume. |
| 102 | `resume` | `json` | `neither-failed` | 1 | `J-resume-content-loss` | Bucket-1: zig resume --json returns minimal packet vs 180-line Go shape. |
| 103 | `resume` | `real-cwd` | `both-failed` | 1 | `F-exit-code-not-found` | Bucket-1: exit-code drift (go=1 zig=2) on no-task fixture. |
| 104 | `resume` | `real-cwd-json` | `both-failed` | 1 | `F-exit-code-not-found` | Bucket-1: same exit-code drift. |
| 105 | `scenario` | `help` | `neither-failed` | 1 | `B-help-text-content-loss` | Restore Go per-command prose descriptions; format-only cosmetic is subordinate B |
| 106 | `scenario` | `no-args` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 107 | `scenario` | `json` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 108 | `scenario` | `real-cwd` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 109 | `scenario` | `real-cwd-json` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 110 | `scope` | `help` | `neither-failed` | 1 | `B-help-text-content-loss` | Restore Go per-command prose descriptions; format-only cosmetic is subordinate B |
| 111 | `scope` | `no-args` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 112 | `scope` | `json` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 113 | `scope` | `real-cwd` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 114 | `scope` | `real-cwd-json` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 115 | `search` | `help` | `neither-failed` | 1 | `B-help-text-content-loss` | Restore Go per-command prose descriptions; format-only cosmetic is subordinate B |
| 116 | `search` | `no-args` | `both-failed` | 4 | `I-leaf-positional-error-wording` | Cosmetic: both reject with positional/flag errors; wording differs. |
| 117 | `search` | `json` | `both-failed` | 4 | `I-leaf-positional-error-wording` | Cosmetic: both reject with positional/flag errors; wording differs. |
| 118 | `search` | `real-cwd` | `both-failed` | 4 | `I-leaf-positional-error-wording` | Cosmetic: both reject with positional/flag errors; wording differs. |
| 119 | `search` | `real-cwd-json` | `both-failed` | 4 | `I-leaf-positional-error-wording` | Cosmetic: both reject with positional/flag errors; wording differs. |
| 120 | `skills` | `help` | `neither-failed` | 1 | `B-help-text-content-loss` | Restore Go per-command prose descriptions; format-only cosmetic is subordinate B |
| 121 | `skills` | `no-args` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 122 | `skills` | `json` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 123 | `skills` | `real-cwd` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 124 | `skills` | `real-cwd-json` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 125 | `spec` | `help` | `neither-failed` | 1 | `B-help-text-content-loss` | Restore Go per-command prose descriptions; format-only cosmetic is subordinate B |
| 126 | `spec` | `no-args` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 127 | `spec` | `json` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 128 | `spec` | `real-cwd` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 129 | `spec` | `real-cwd-json` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 130 | `task` | `help` | `neither-failed` | 1 | `B-help-text-content-loss` | Restore Go per-command prose descriptions; format-only cosmetic is subordinate B |
| 131 | `task` | `no-args` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 132 | `task` | `json` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 133 | `task` | `real-cwd` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 134 | `task` | `real-cwd-json` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 135 | `templates` | `help` | `neither-failed` | 1 | `B-help-text-content-loss` | Restore Go per-command prose descriptions; format-only cosmetic is subordinate B |
| 136 | `templates` | `no-args` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 137 | `templates` | `json` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 138 | `templates` | `real-cwd` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 139 | `templates` | `real-cwd-json` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 140 | `test-spec` | `help` | `neither-failed` | 1 | `B-help-text-content-loss` | Restore Go per-command prose descriptions; format-only cosmetic is subordinate B |
| 141 | `test-spec` | `no-args` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 142 | `test-spec` | `json` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 143 | `test-spec` | `real-cwd` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 144 | `test-spec` | `real-cwd-json` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 145 | `tree` | `help` | `neither-failed` | 1 | `B-help-text-content-loss` | Restore Go per-command prose descriptions; format-only cosmetic is subordinate B |
| 146 | `tree` | `no-args` | `neither-failed` | 1 | `K-tree-render-drift` | Bucket-1: tree ordering (artifact/question grouping vs interleave) + title trunc |
| 147 | `tree` | `json` | `neither-failed` | 1 | `K-tree-render-drift` | Bucket-1: same ordering drift in tree --json (2.1MB diff). |
| 148 | `tree` | `real-cwd` | `neither-failed` | 1 | `E-tree-cwd-derive` | Bucket-1 ANCHOR: cwd-derive regression — tree returns 'global' instead of assoc: |
| 149 | `tree` | `real-cwd-json` | `neither-failed` | 1 | `E-tree-cwd-derive` | Bucket-1: same cwd-derive regression + scope-node JSON pollution (extra zero-val |
| 150 | `unlink` | `help` | `neither-failed` | 1 | `B-help-text-content-loss` | Restore Go per-command prose descriptions; format-only cosmetic is subordinate B |
| 151 | `unlink` | `no-args` | `both-failed` | 4 | `I-leaf-positional-error-wording` | Cosmetic: both reject with positional/flag errors; wording differs. |
| 152 | `unlink` | `json` | `both-failed` | 4 | `I-leaf-positional-error-wording` | Cosmetic: both reject with positional/flag errors; wording differs. |
| 153 | `unlink` | `real-cwd` | `both-failed` | 4 | `I-leaf-positional-error-wording` | Cosmetic: both reject with positional/flag errors; wording differs. |
| 154 | `unlink` | `real-cwd-json` | `both-failed` | 4 | `I-leaf-positional-error-wording` | Cosmetic: both reject with positional/flag errors; wording differs. |
| 155 | `workbench` | `help` | `neither-failed` | 1 | `B-help-text-content-loss` | Restore Go per-command prose descriptions; format-only cosmetic is subordinate B |
| 156 | `workbench` | `no-args` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 157 | `workbench` | `json` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 158 | `workbench` | `real-cwd` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 159 | `workbench` | `real-cwd-json` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 160 | `workspace` | `help` | `neither-failed` | 1 | `B-help-text-content-loss` | Restore Go per-command prose descriptions; format-only cosmetic is subordinate B |
| 161 | `workspace` | `no-args` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 162 | `workspace` | `json` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 163 | `workspace` | `real-cwd` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 164 | `workspace` | `real-cwd-json` | `zig-failed` | 1 | `A-bare-parent-verb` | Gated by Q234; rec: Bucket 1 (match Go bare help+exit0). |
| 165 | `plan` | `q233-plan-next` | `zig-failed` | 1 | `F-plan-next` | Reinstate 'plan next'. Gated by Q236. |
| 166 | `agent` | `q233-agent-top` | `both-failed` | 4 | `F-agent-missing` | Cosmetic: both binaries lack 'agent' verb; wording differs. |
| 167 | `agent` | `q233-agent-ps` | `both-failed` | 4 | `F-agent-missing` | Cosmetic: both binaries lack 'agent ps'; wording differs. |
| 168 | `test-spec` | `q233-status-plan-flag` | `both-failed` | 4 | `F-flag-wording` | Cosmetic: both reject --plan; wording differs. |
| 169 | `test-spec` | `q233-status-positional` | `both-failed` | 1 | `F-exit-code-not-found` | Bucket-1: 'plan not found' should exit 1 not 4. |
| 170 | `question` | `q233-add-plan-flag` | `both-failed` | 3 | `F-q233-add-shape` | Intentional positional-title shape (Q237). |
| 171 | `task` | `q233-add-editor-false` | `both-failed` | 3 | `F-q233-add-shape` | Intentional positional-title shape (Q237). |
| 172 | `task` | `q233-add-no-editor` | `both-failed` | 3 | `F-q233-add-shape` | Intentional positional-title shape (Q237). |

## Phase 3 backlog hint

This triage hands Phase 3 the following test backlog shape:

- **Bucket 1 (133 rows → ~12 distinct cluster tests):** A (one per parent verb, ~21 tests), B (one per verb's help-prose, ~32 tests), C (one health test), E (two tree-cwd tests), J (two resume tests), F-plan-next (one), F-exit-code-not-found (three), H-handoff-json-shape (one), H-handoff-cwd-fixture (two; pending fixture decision), K-tree-render-drift (two). Final test count depends on whether per-verb tests for A and B are individual or generated from a table.
- **Bucket 3 (8 rows → ~3 passing tests + allowlist):** D (one passing annotate-help test), F-q233-add-shape (three positional-shape pinning tests).
- **Bucket 4 (32 rows → 0 tests, allowlist only):** all entries here become rows in `scripts/parity-allowlist.txt` (Phase 5).

Total estimated Phase-3 surface: ~60-70 integration tests if per-verb; ~12-15 if table-driven. Recommend table-driven for A and B clusters to keep maintenance tractable.
