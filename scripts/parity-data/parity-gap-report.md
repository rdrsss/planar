# Parity Gap Report

_Generated 2026-05-26T16:00:10Z_

## Summary

37 verbs audited, 172 gaps surfaced, 0 errors, 4 skipped (with reasons), 1 no-diff invocations across 173 total invocations.

- Go binary: `/Users/mn/.planar-archive/bin/planar-go` (sha256 `9b32c7720522…`)
- Zig binary: `/Users/mn/projects/github/rdrsss/planar/bin/planar` (sha256 `18fa4da01a69…`)
- Audit DB: `<AUDIT_DB>`
- Cwd-fixture DB: `<CWD_DB>`
- Cwd-fixture dir: `<CWD_DIR>`

### Failure class distribution

Per-invocation breakdown of which binary (if any) returned a non-zero exit. `neither-failed` rows are the most signal-rich gaps (both binaries ran but disagreed); `both-failed` rows often differ only in the error message phrasing.

| Class | Count |
|-------|-------|
| `neither-failed` | 85 |
| `go-failed` | 3 |
| `zig-failed` | 43 |
| `both-failed` | 42 |

## Preflight health-check

Both binaries opened the audit DB before the matrix ran. Go reported schema **14** (exit 1); Zig reported schema **14** (exit 1).

<details><summary>Go health output</summary>

```
planar health

  db:               <AUDIT_DB>  [ok]
  schema:           14  [current]
  integrity:        ok
  in-flight tasks:  732  (0 resumable, 732 NOT RESUMABLE)
  pending handoffs: 0  (0 stale)

overall: DEGRADED  (732 tasks not resumable)
error: degraded health
```

</details>

<details><summary>Zig health output</summary>

```
db:               ok (<AUDIT_DB>)
schema:           v14 of v14 (current)
integrity:        ok
in-flight tasks:  2 (0 resumable, 2 NOT resumable)
pending handoffs: 3 (3 stale > 24h)
overall:          degraded
```

</details>

## Pre-skipped verbs

| Verb | Reason |
|------|--------|
| `completion` | emits multi-kilobyte shell scripts that diverge in trivial ways across shells |
| `init` | init mutates the DB by registering the cwd as a project; not safe in the audit DB |
| `sync` | hits live external systems (Jira, GitHub) — network-dependent, not deterministic |
| `version` | build SHAs and zig-runtime strings always diverge; surfaces nothing useful pre-triage |

## Gaps (by diff size, descending)

| # | Verb | Invocation | Args | Diff bytes | Go exit | Zig exit | Failure class |
|---|------|------------|------|------------|---------|----------|---------------|
| 1 | `tree` | `json` | `--json` | 2010517 | 0 | 0 | `neither-failed` |
| 2 | `tree` | `no-args` | `` | 390830 | 0 | 0 | `neither-failed` |
| 3 | `resume` | `json` | `--json` | 16560 | 0 | 0 | `neither-failed` |
| 4 | `resume` | `no-args` | `` | 7013 | 0 | 0 | `neither-failed` |
| 5 | `pl-import` | `help` | `--help` | 5085 | 0 | 0 | `neither-failed` |
| 6 | `tree` | `help` | `--help` | 4990 | 0 | 0 | `neither-failed` |
| 7 | `pl-synthesize` | `help` | `--help` | 4458 | 0 | 0 | `neither-failed` |
| 8 | `task` | `help` | `--help` | 3053 | 0 | 0 | `neither-failed` |
| 9 | `task` | `no-args` | `` | 3053 | 0 | 0 | `neither-failed` |
| 10 | `task` | `real-cwd` | `` | 3053 | 0 | 0 | `neither-failed` |
| 11 | `workbench` | `help` | `--help` | 2971 | 0 | 0 | `neither-failed` |
| 12 | `workbench` | `no-args` | `` | 2971 | 0 | 0 | `neither-failed` |
| 13 | `workbench` | `real-cwd` | `` | 2971 | 0 | 0 | `neither-failed` |
| 14 | `handoff` | `help` | `--help` | 2899 | 0 | 0 | `neither-failed` |
| 15 | `templates` | `help` | `--help` | 2829 | 0 | 0 | `neither-failed` |
| 16 | `templates` | `no-args` | `` | 2829 | 0 | 0 | `neither-failed` |
| 17 | `templates` | `real-cwd` | `` | 2829 | 0 | 0 | `neither-failed` |
| 18 | `link` | `help` | `--help` | 2708 | 0 | 0 | `neither-failed` |
| 19 | `local` | `help` | `--help` | 2560 | 0 | 0 | `neither-failed` |
| 20 | `local` | `no-args` | `` | 2560 | 0 | 0 | `neither-failed` |
| 21 | `local` | `real-cwd` | `` | 2560 | 0 | 0 | `neither-failed` |
| 22 | `scenario` | `help` | `--help` | 2541 | 0 | 0 | `neither-failed` |
| 23 | `scenario` | `no-args` | `` | 2541 | 0 | 0 | `neither-failed` |
| 24 | `scenario` | `real-cwd` | `` | 2541 | 0 | 0 | `neither-failed` |
| 25 | `search` | `help` | `--help` | 2506 | 0 | 0 | `neither-failed` |
| 26 | `artifact` | `help` | `--help` | 2497 | 0 | 0 | `neither-failed` |
| 27 | `artifact` | `no-args` | `` | 2497 | 0 | 0 | `neither-failed` |
| 28 | `artifact` | `real-cwd` | `` | 2497 | 0 | 0 | `neither-failed` |
| 29 | `doc` | `help` | `--help` | 2452 | 0 | 0 | `neither-failed` |
| 30 | `doc` | `no-args` | `` | 2452 | 0 | 0 | `neither-failed` |
| 31 | `doc` | `real-cwd` | `` | 2452 | 0 | 0 | `neither-failed` |
| 32 | `decision` | `help` | `--help` | 2450 | 0 | 0 | `neither-failed` |
| 33 | `decision` | `no-args` | `` | 2450 | 0 | 0 | `neither-failed` |
| 34 | `decision` | `real-cwd` | `` | 2450 | 0 | 0 | `neither-failed` |
| 35 | `resume` | `help` | `--help` | 2411 | 0 | 0 | `neither-failed` |
| 36 | `links` | `help` | `--help` | 2404 | 0 | 0 | `neither-failed` |
| 37 | `links` | `no-args` | `` | 2404 | 0 | 0 | `neither-failed` |
| 38 | `links` | `real-cwd` | `` | 2404 | 0 | 0 | `neither-failed` |
| 39 | `plan` | `help` | `--help` | 2388 | 0 | 0 | `neither-failed` |
| 40 | `plan` | `no-args` | `` | 2388 | 0 | 0 | `neither-failed` |
| 41 | `plan` | `real-cwd` | `` | 2388 | 0 | 0 | `neither-failed` |
| 42 | `question` | `help` | `--help` | 2334 | 0 | 0 | `neither-failed` |
| 43 | `question` | `no-args` | `` | 2334 | 0 | 0 | `neither-failed` |
| 44 | `question` | `real-cwd` | `` | 2334 | 0 | 0 | `neither-failed` |
| 45 | `assoc` | `help` | `--help` | 2204 | 0 | 0 | `neither-failed` |
| 46 | `assoc` | `no-args` | `` | 2204 | 0 | 0 | `neither-failed` |
| 47 | `assoc` | `real-cwd` | `` | 2204 | 0 | 0 | `neither-failed` |
| 48 | `capture` | `help` | `--help` | 2195 | 0 | 0 | `neither-failed` |
| 49 | `capture` | `no-args` | `` | 2195 | 0 | 0 | `neither-failed` |
| 50 | `capture` | `real-cwd` | `` | 2195 | 0 | 0 | `neither-failed` |
| 51 | `config` | `help` | `--help` | 2175 | 0 | 0 | `neither-failed` |
| 52 | `config` | `no-args` | `` | 2175 | 0 | 0 | `neither-failed` |
| 53 | `config` | `real-cwd` | `` | 2175 | 0 | 0 | `neither-failed` |
| 54 | `workspace` | `help` | `--help` | 2155 | 0 | 0 | `neither-failed` |
| 55 | `workspace` | `no-args` | `` | 2155 | 0 | 0 | `neither-failed` |
| 56 | `workspace` | `real-cwd` | `` | 2155 | 0 | 0 | `neither-failed` |
| 57 | `templates` | `json` | `--json` | 2135 | 0 | 1 | `zig-failed` |
| 58 | `templates` | `real-cwd-json` | `--json` | 2135 | 0 | 1 | `zig-failed` |
| 59 | `task` | `json` | `--json` | 2085 | 0 | 1 | `zig-failed` |
| 60 | `task` | `real-cwd-json` | `--json` | 2085 | 0 | 1 | `zig-failed` |
| 61 | `local` | `json` | `--json` | 2082 | 0 | 1 | `zig-failed` |
| 62 | `local` | `real-cwd-json` | `--json` | 2082 | 0 | 1 | `zig-failed` |
| 63 | `workbench` | `json` | `--json` | 2079 | 0 | 1 | `zig-failed` |
| 64 | `workbench` | `real-cwd-json` | `--json` | 2079 | 0 | 1 | `zig-failed` |
| 65 | `scope` | `help` | `--help` | 2044 | 0 | 0 | `neither-failed` |
| 66 | `scope` | `no-args` | `` | 2044 | 0 | 0 | `neither-failed` |
| 67 | `scope` | `real-cwd` | `` | 2044 | 0 | 0 | `neither-failed` |
| 68 | `doc` | `json` | `--json` | 1844 | 0 | 1 | `zig-failed` |
| 69 | `doc` | `real-cwd-json` | `--json` | 1844 | 0 | 1 | `zig-failed` |
| 70 | `scenario` | `json` | `--json` | 1843 | 0 | 1 | `zig-failed` |
| 71 | `scenario` | `real-cwd-json` | `--json` | 1843 | 0 | 1 | `zig-failed` |
| 72 | `ext` | `help` | `--help` | 1833 | 0 | 0 | `neither-failed` |
| 73 | `ext` | `no-args` | `` | 1833 | 0 | 0 | `neither-failed` |
| 74 | `ext` | `real-cwd` | `` | 1833 | 0 | 0 | `neither-failed` |
| 75 | `decision` | `json` | `--json` | 1823 | 0 | 1 | `zig-failed` |
| 76 | `decision` | `real-cwd-json` | `--json` | 1823 | 0 | 1 | `zig-failed` |
| 77 | `question` | `json` | `--json` | 1813 | 0 | 1 | `zig-failed` |
| 78 | `question` | `real-cwd-json` | `--json` | 1813 | 0 | 1 | `zig-failed` |
| 79 | `links` | `json` | `--json` | 1812 | 0 | 1 | `zig-failed` |
| 80 | `links` | `real-cwd-json` | `--json` | 1812 | 0 | 1 | `zig-failed` |
| 81 | `artifact` | `json` | `--json` | 1800 | 0 | 1 | `zig-failed` |
| 82 | `artifact` | `real-cwd-json` | `--json` | 1800 | 0 | 1 | `zig-failed` |
| 83 | `skills` | `help` | `--help` | 1787 | 0 | 0 | `neither-failed` |
| 84 | `skills` | `no-args` | `` | 1787 | 0 | 0 | `neither-failed` |
| 85 | `skills` | `real-cwd` | `` | 1787 | 0 | 0 | `neither-failed` |
| 86 | `audit` | `help` | `--help` | 1769 | 0 | 0 | `neither-failed` |
| 87 | `audit` | `no-args` | `` | 1769 | 0 | 0 | `neither-failed` |
| 88 | `audit` | `real-cwd` | `` | 1769 | 0 | 0 | `neither-failed` |
| 89 | `workspace` | `json` | `--json` | 1766 | 0 | 1 | `zig-failed` |
| 90 | `workspace` | `real-cwd-json` | `--json` | 1766 | 0 | 1 | `zig-failed` |
| 91 | `config` | `json` | `--json` | 1724 | 0 | 1 | `zig-failed` |
| 92 | `config` | `real-cwd-json` | `--json` | 1724 | 0 | 1 | `zig-failed` |
| 93 | `assoc` | `json` | `--json` | 1668 | 0 | 1 | `zig-failed` |
| 94 | `assoc` | `real-cwd-json` | `--json` | 1668 | 0 | 1 | `zig-failed` |
| 95 | `capture` | `json` | `--json` | 1667 | 0 | 1 | `zig-failed` |
| 96 | `capture` | `real-cwd-json` | `--json` | 1667 | 0 | 1 | `zig-failed` |
| 97 | `plan` | `json` | `--json` | 1653 | 0 | 1 | `zig-failed` |
| 98 | `plan` | `real-cwd-json` | `--json` | 1653 | 0 | 1 | `zig-failed` |
| 99 | `plan` | `q233-plan-next` | `next 351` | 1602 | 0 | 0 | `neither-failed` |
| 100 | `promote` | `help` | `--help` | 1585 | 0 | 0 | `neither-failed` |
| 101 | `test-spec` | `help` | `--help` | 1584 | 0 | 0 | `neither-failed` |
| 102 | `test-spec` | `no-args` | `` | 1584 | 0 | 0 | `neither-failed` |
| 103 | `test-spec` | `real-cwd` | `` | 1584 | 0 | 0 | `neither-failed` |
| 104 | `scope` | `json` | `--json` | 1572 | 0 | 1 | `zig-failed` |
| 105 | `scope` | `real-cwd-json` | `--json` | 1572 | 0 | 1 | `zig-failed` |
| 106 | `skills` | `json` | `--json` | 1572 | 0 | 1 | `zig-failed` |
| 107 | `skills` | `real-cwd-json` | `--json` | 1572 | 0 | 1 | `zig-failed` |
| 108 | `demote` | `help` | `--help` | 1492 | 0 | 0 | `neither-failed` |
| 109 | `ext` | `json` | `--json` | 1484 | 0 | 1 | `zig-failed` |
| 110 | `ext` | `real-cwd-json` | `--json` | 1484 | 0 | 1 | `zig-failed` |
| 111 | `unlink` | `help` | `--help` | 1465 | 0 | 0 | `neither-failed` |
| 112 | `health` | `help` | `--help` | 1458 | 0 | 0 | `neither-failed` |
| 113 | `spec` | `help` | `--help` | 1420 | 0 | 0 | `neither-failed` |
| 114 | `spec` | `no-args` | `` | 1420 | 0 | 0 | `neither-failed` |
| 115 | `spec` | `real-cwd` | `` | 1420 | 0 | 0 | `neither-failed` |
| 116 | `test-spec` | `json` | `--json` | 1388 | 0 | 1 | `zig-failed` |
| 117 | `test-spec` | `real-cwd-json` | `--json` | 1388 | 0 | 1 | `zig-failed` |
| 118 | `audit` | `json` | `--json` | 1348 | 0 | 1 | `zig-failed` |
| 119 | `audit` | `real-cwd-json` | `--json` | 1348 | 0 | 1 | `zig-failed` |
| 120 | `spec` | `json` | `--json` | 1257 | 0 | 1 | `zig-failed` |
| 121 | `spec` | `real-cwd-json` | `--json` | 1257 | 0 | 1 | `zig-failed` |
| 122 | `annotate` | `help` | `--help` | 895 | 2 | 0 | `go-failed` |
| 123 | `annotate` | `no-args` | `` | 895 | 2 | 0 | `go-failed` |
| 124 | `annotate` | `real-cwd` | `` | 895 | 2 | 0 | `go-failed` |
| 125 | `health` | `json` | `--json` | 587 | 0 | 1 | `zig-failed` |
| 126 | `health` | `no-args` | `` | 567 | 1 | 1 | `both-failed` |
| 127 | `health` | `real-cwd-json` | `--json` | 536 | 0 | 0 | `neither-failed` |
| 128 | `health` | `real-cwd` | `` | 453 | 0 | 0 | `neither-failed` |
| 129 | `tree` | `real-cwd-json` | `--json` | 365 | 0 | 0 | `neither-failed` |
| 130 | `handoff` | `json` | `--json` | 255 | 0 | 0 | `neither-failed` |
| 131 | `pl-import` | `no-args` | `` | 221 | 2 | 1 | `both-failed` |
| 132 | `pl-import` | `json` | `--json` | 221 | 2 | 1 | `both-failed` |
| 133 | `pl-import` | `real-cwd` | `` | 221 | 2 | 1 | `both-failed` |
| 134 | `pl-import` | `real-cwd-json` | `--json` | 221 | 2 | 1 | `both-failed` |
| 135 | `pl-synthesize` | `no-args` | `` | 221 | 2 | 1 | `both-failed` |
| 136 | `pl-synthesize` | `json` | `--json` | 221 | 2 | 1 | `both-failed` |
| 137 | `pl-synthesize` | `real-cwd` | `` | 221 | 2 | 1 | `both-failed` |
| 138 | `pl-synthesize` | `real-cwd-json` | `--json` | 221 | 2 | 1 | `both-failed` |
| 139 | `handoff` | `no-args` | `` | 220 | 0 | 0 | `neither-failed` |
| 140 | `unlink` | `no-args` | `` | 219 | 2 | 1 | `both-failed` |
| 141 | `unlink` | `json` | `--json` | 219 | 2 | 1 | `both-failed` |
| 142 | `unlink` | `real-cwd` | `` | 219 | 2 | 1 | `both-failed` |
| 143 | `unlink` | `real-cwd-json` | `--json` | 219 | 2 | 1 | `both-failed` |
| 144 | `annotate` | `json` | `--json` | 217 | 2 | 1 | `both-failed` |
| 145 | `annotate` | `real-cwd-json` | `--json` | 217 | 2 | 1 | `both-failed` |
| 146 | `search` | `no-args` | `` | 217 | 2 | 1 | `both-failed` |
| 147 | `search` | `json` | `--json` | 217 | 2 | 1 | `both-failed` |
| 148 | `search` | `real-cwd` | `` | 217 | 2 | 1 | `both-failed` |
| 149 | `search` | `real-cwd-json` | `--json` | 217 | 2 | 1 | `both-failed` |
| 150 | `demote` | `no-args` | `` | 215 | 2 | 1 | `both-failed` |
| 151 | `demote` | `json` | `--json` | 215 | 2 | 1 | `both-failed` |
| 152 | `demote` | `real-cwd` | `` | 215 | 2 | 1 | `both-failed` |
| 153 | `demote` | `real-cwd-json` | `--json` | 215 | 2 | 1 | `both-failed` |
| 154 | `handoff` | `real-cwd` | `` | 212 | 1 | 2 | `both-failed` |
| 155 | `handoff` | `real-cwd-json` | `--json` | 212 | 1 | 2 | `both-failed` |
| 156 | `agent` | `q233-agent-top` | `` | 212 | 2 | 1 | `both-failed` |
| 157 | `agent` | `q233-agent-ps` | `ps` | 212 | 2 | 1 | `both-failed` |
| 158 | `link` | `no-args` | `` | 198 | 2 | 1 | `both-failed` |
| 159 | `link` | `json` | `--json` | 198 | 2 | 1 | `both-failed` |
| 160 | `link` | `real-cwd` | `` | 198 | 2 | 1 | `both-failed` |
| 161 | `link` | `real-cwd-json` | `--json` | 198 | 2 | 1 | `both-failed` |
| 162 | `promote` | `no-args` | `` | 198 | 2 | 1 | `both-failed` |
| 163 | `promote` | `json` | `--json` | 198 | 2 | 1 | `both-failed` |
| 164 | `promote` | `real-cwd` | `` | 198 | 2 | 1 | `both-failed` |
| 165 | `promote` | `real-cwd-json` | `--json` | 198 | 2 | 1 | `both-failed` |
| 166 | `test-spec` | `q233-status-positional` | `status 351` | 193 | 1 | 1 | `both-failed` |
| 167 | `question` | `q233-add-plan-flag` | `add --plan 351 --title parity-probe --body x` | 186 | 2 | 1 | `both-failed` |
| 168 | `task` | `q233-add-editor-false` | `add --plan 351 --title parity-probe --next-action x --editor=false` | 186 | 2 | 1 | `both-failed` |
| 169 | `task` | `q233-add-no-editor` | `add --plan 351 --title parity-probe --next-action x --no-editor` | 186 | 2 | 1 | `both-failed` |
| 170 | `test-spec` | `q233-status-plan-flag` | `status --plan 351` | 184 | 2 | 1 | `both-failed` |
| 171 | `resume` | `real-cwd` | `` | 31 | 1 | 2 | `both-failed` |
| 172 | `resume` | `real-cwd-json` | `--json` | 31 | 1 | 2 | `both-failed` |

### Per-gap diffs

#### 1. `tree --json` — invocation `json`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 1145353B, stderr 0B)
- Zig exit: `0` (stdout 835578B, stderr 0B)

```diff
--- go
+++ zig
@@ -1,29552 +1 @@
-{
-  "kind": "scope",
-  "title": "assoc:project:planar",
-  "scope_kind": "association",
-  "scope_id": 1,
-  "scope_label": "assoc:project:planar",
-  "children": [
-    {
-      "kind": "plan",
-      "id": 1,
-      "title": "Post-M7 backlog",
-      "slug": "post-m7",
-      "status": "done",
-      "created_at": "2026-05-14T00:03:08.733Z",
-      "updated_at": "2026-05-16T02:50:34.204Z",
-      "children": [
-        {
-          "kind": "plan",
-          "id": 2,
-          "title": "ext propagate --github-strategy override",
-          "slug": "ext-strategy-override",
-          "status": "done",
-          "created_at": "2026-05-14T00:19:04.518Z",
-          "updated_at": "2026-05-15T15:30:58.594Z",
-          "children": [
-            {
-              "kind": "task",
-              "id": 1,
-              "title": "Add --github-strategy flag to ext propagate with validation",
-              "status": "done",
-              "priority": 10,
-              "created_at": "2026-05-14T00:19:45.786Z",
-              "updated_at": "2026-05-15T15:18:29.739Z",
-              "children": []
-            },
-            {
-              "kind": "task",
-              "id": 2,
-              "title": "Plumb strategy override through engine resolution",
-              "status": "done",
-              "priority": 20,
-              "created_at": "2026-05-14T00:19:45.804Z",
-              "updated_at": "2026-05-15T15:18:29.755Z",
-              "children": []
-            },
-            {
-              "kind": "task",
-              "id": 3,
-              "title": "Update docs and skills for --github-strategy",
-              "status": "done",
-              "priority": 30,
-              "created_at": "2026-05-14T00:19:45.822Z",
-              "updated_at": "2026-05-15T15:30:58.576Z",
-              "children": []
-            },
-            {
-              "kind": "task",
-              "id": 4,
-              "title": "Integration tests E-12 series for strategy override",
-              "status": "done",
-              "priority": 40,
-              "created_at": "2026-05-14T00:19:45.839Z",
-              "updated_at": "2026-05-15T15:27:15.651Z",
-              "children": []
-            }
-          ]
-        },
-        {
-          "kind": "plan",
-          "id": 3,
-          "title": "Scope ergonomics: defaults from active scope + visibility",
-          "slug": "scope-ergonomics",
-          "status": "done",
-          "created_at": "2026-05-14T00:34:55.266Z",
-          "updated_at": "2026-05-14T10:57:41.106Z",
-          "children": [
-            {
-              "kind": "task",
-              "id": 5,
-              "title": "Active scope drives entity-creation defaults + resolved-scope output",
-              "status": "done",
-              "priority": 10,
-              "created_at": "2026-05-14T00:35:33.656Z",
-              "updated_at": "2026-05-14T10:45:36.410Z",
-              "children": []
-            },
-            {
-              "kind": "task",
-              "id": 6,
-              "title": "Improve scope use error for repo:<slug> and global",
-              "status": "done",
-              "priority": 20,
-              "created_at": "2026-05-14T00:35:33.675Z",
-              "updated_at": "2026-05-14T10:51:21.761Z",
-              "children": []
-            },
-            {
-              "kind": "task",
-              "id": 7,
-              "title": "Integration tests for scope defaults + scope use guidance",
-              "status": "done",
-              "priority": 30,
-              "created_at": "2026-05-14T00:35:33.693Z",
-              "updated_at": "2026-05-14T10:57:41.022Z",
-              "children": []
-            }
-          ]
-        },
-        {
-          "kind": "artifact",
-          "id": 1,
-          "title": "Founding tech spec",
-          "status": "active",
-          "artifact_kind": "tech_spec",
-          "created_at": "2026-05-14T00:03:17.244Z",
-          "updated_at": "2026-05-14T00:07:04.466Z",
-          "children": []
-        },
-        {
-          "kind": "artifact",
-          "id": 2,
-          "title": "Project roadmap",
-          "status": "active",
-          "artifact_kind": "roadmap",
-          "created_at": "2026-05-14T00:03:17.259Z",
-          "updated_at": "2026-05-14T00:07:04.482Z",
-          "children": []
-        },
-        {
-          "kind": "artifact",
-          "id": 3,
-          "title": "Product Spec: Entity Visibility",
-          "status": "draft",
-          "artifact_kind": "product_spec",
-          "created_at": "2026-05-14T12:37:08.163Z",
-          "updated_at": "2026-05-14T12:37:08.163Z",
-          "children": []
-        },
-        {
-          "kind": "artifact",
-          "id": 183,
-          "title": "tech",
-          "status": "draft",
-          "artifact_kind": "tech_spec",
-          "created_at": "2026-05-25T00:50:13.923Z",
-          "updated_at": "2026-05-25T00:50:13.923Z",
-          "children": []
-        },
-        {
-          "kind": "artifact",
-          "id": 184,
-          "title": "roadmap",
-          "status": "draft",
-          "artifact_kind": "roadmap",
-          "created_at": "2026-05-25T00:50:13.944Z",
-          "updated_at": "2026-05-25T00:50:13.944Z",
-          "children": []
-        },
-        {
-          "kind": "artifact",
-          "id": 185,
-          "title": "test",
-          "status": "draft",
-          "artifact_kind": "test_spec",
-          "created_at": "2026-05-25T00:50:13.962Z",
-          "updated_at": "2026-05-25T00:50:13.962Z",
-          "children": []
-        },
-        {
-          "kind": "question",
-          "id": 1,
-          "title": "Should the ingestor parse '## Open Questions' H3s with inline resolution markers and emit decisions from resolved ones?",
-          "status": "answered",
-          "created_at": "2026-05-14T12:50:18.920Z",
-          "updated_at": "2026-05-16T02:10:57.686Z",
-          "children": []
-        },
-        {
-          "kind": "question",
-          "id": 2,
-          "title": "Document slug→FS transformation in agents/planner.md",
-          "status": "answered",
-          "created_at": "2026-05-14T12:50:18.937Z",
-          "updated_at": "2026-05-15T19:11:36.406Z",
-          "children": []
-        },
-        {
-          "kind": "question",
-          "id": 3,
-          "title": "cmd_scope.go (and other commands) print errors twice",
-          "status": "answered",
-          "created_at": "2026-05-14T12:50:18.955Z",
-          "updated_at": "2026-05-15T19:11:36.421Z",
-          "children": []
-        },
-        {
-          "kind": "question",
-          "id": 4,
-          "title": "scope use with empty remainder (repo:, assoc:, association:) falls through to bare-slug lookup",
-          "status": "answered",
-          "created_at": "2026-05-14T12:50:18.973Z",
-          "updated_at": "2026-05-15T19:11:36.436Z",
-          "children": []
-        },
-        {
-          "kind": "question",
-          "id": 5,
-          "title": "Should planar grow a 'workbench publish --to <path>' verb for on-demand host-repo export?",
-          "status": "answered",
-          "created_at": "2026-05-14T12:50:18.991Z",
-          "updated_at": "2026-05-16T02:32:41.823Z",
-          "children": []
-        },
-        {
-          "kind": "question",
-          "id": 6,
-          "title": "Should touches links count toward repo detection for global-scoped tasks?",
-          "status": "answered",
-          "created_at": "2026-05-15T11:51:03.315Z",
-          "updated_at": "2026-05-16T02:38:43.988Z",
-          "children": []
-        },
-        {
-          "kind": "question",
-          "id": 7,
-          "title": "Should plan / task scope be mutable post-creation (`update --scope`)?",
-          "status": "answered",
-          "created_at": "2026-05-15T11:51:11.230Z",
-          "up
```

_(diff truncated at 8000 bytes; full body in parity-gap-report.json)_

#### 2. `tree ` — invocation `no-args`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 325702B, stderr 0B)
- Zig exit: `0` (stdout 323702B, stderr 0B)

```diff
--- go
+++ zig
@@ -10,69 +10,69 @@
 │   │   ├── task:6  Improve scope use error for repo:<slug> and global  [done, pri:20]
 │   │   └── task:7  Integration tests for scope defaults + scope use guidance  [done, pri:30]
 │   ├── artifact:1  Founding tech spec  [tech_spec, active]
-│   ├── question:1  Should the ingestor parse '## Open Questions' H3s with inline resolution marker…  [answered]
 │   ├── artifact:2  Project roadmap  [roadmap, active]
+│   ├── question:1  Should the ingestor parse '## Open Questions' H3s with inline resolution mark…  [answered]
 │   ├── question:2  Document slug→FS transformation in agents/planner.md  [answered]
-│   ├── artifact:3  Product Spec: Entity Visibility  [product_spec, draft]
 │   ├── question:3  cmd_scope.go (and other commands) print errors twice  [answered]
-│   ├── question:4  scope use with empty remainder (repo:, assoc:, association:) falls through to b…  [answered]
-│   ├── question:5  Should planar grow a 'workbench publish --to <path>' verb for on-demand host-re…  [answered]
+│   ├── question:4  scope use with empty remainder (repo:, assoc:, association:) falls through to…  [answered]
+│   ├── question:5  Should planar grow a 'workbench publish --to <path>' verb for on-demand host-…  [answered]
 │   ├── question:6  Should touches links count toward repo detection for global-scoped tasks?  [answered]
 │   ├── question:7  Should plan / task scope be mutable post-creation (`update --scope`)?  [answered]
-│   ├── question:8  Real-world projects.slug doesn't match the 'owner/repo' format expected by GitH…  [answered]
-│   ├── question:9  Propagate task/plan enumeration assumes entity_links(derives-from) — pre-inge…  [answered]
-│   ├── question:10  Planner-written workbench .md files lack front matter, breaking bidirectional p…  [answered]
-│   ├── question:11  ext propagate creates read-only links; no easy path to write-back sync without …  [answered]
+│   ├── question:8  Real-world projects.slug doesn't match the 'owner/repo' format expected by Gi…  [answered]
+│   ├── question:9  Propagate task/plan enumeration assumes entity_links(derives-from) — pre-in…  [answered]
+│   ├── question:10  Planner-written workbench .md files lack front matter, breaking bidirectional…  [answered]
+│   ├── question:11  ext propagate creates read-only links; no easy path to write-back sync withou…  [answered]
 │   ├── artifact:183  tech  [tech_spec, draft]
 │   ├── artifact:184  roadmap  [roadmap, draft]
-│   └── artifact:185  test  [test_spec, draft]
+│   ├── artifact:185  test  [test_spec, draft]
+│   └── artifact:3  Product Spec: Entity Visibility  [product_spec, draft]
 ├── plan:4 [done]  Entity visibility: scope columns and planar tree verb
 │   ├── plan:5 [done]  M1 — Scope column on `*-list` commands
-│   │   ├── task:8  Add a `formatScope(kind, id, db)` helper in `internal/output/` that resolves `(…  [done, pri:100]
+│   │   ├── task:8  Add a `formatScope(kind, id, db)` helper in `internal/output/` that resolves …  [done, pri:100]
 │   │   ├── task:9  Thread the helper into the human-format renderer for `artifact list`.  [done, pri:100]
 │   │   ├── task:10  Thread the helper into the human-format renderer for `task list`.  [done, pri:100]
 │   │   ├── task:11  Thread the helper into the human-format renderer for `plan list`.  [done, pri:100]
 │   │   ├── task:12  Thread the helper into the human-format renderer for `question list`.  [done, pri:100]
 │   │   ├── task:13  Thread the helper into the human-format renderer for `scenario list`.  [done, pri:100]
 │   │   ├── task:14  Thread the helper into the human-format renderer for `decision list`.  [done, pri:100]
-│   │   └── task:15  Update `docs/cli-spec.md` per-command Output (human) examples to include the ne…  [done, pri:100]
+│   │   └── task:15  Update `docs/cli-spec.md` per-command Output (human) examples to include the …  [done, pri:100]
 │   ├── plan:6 [done]  M2 — `planar tree` verb in its own internal/tree/ package
-│   │   ├── task:16  Create `internal/tree/` package skeleton with `Node` type, `Walk(db, opts) (Nod…  [done, pri:100]
-│   │   ├── task:17  Implement the recursive CTE plan-tree walker in `internal/tree/walk.go`, mirror…  [done, pri:100]
-│   │   ├── task:18  Implement `Render(n Node, w io.Writer, opts RenderOpts) error` for indented Uni…  [done, pri:100]
-│   │   ├── task:19  Implement `RenderJSON(n Node, w io.Writer) error` for the `--json` shape — ne…  [done, pri:100]
-│   │   ├── task:20  Implement `--ascii` charset toggle in `Render` (ASCII `+--` / `|` / `\--` inste…  [done, pri:100]
-│   │   ├── task:21  Implement `-L <N>` / `--depth <N>` depth-clip in the walker (top-level plan is …  [done, pri:100]
-│   │   ├── task:22  Implement `-I <pattern>` / `--ignore` and `-P <pattern>` / `--match` glob-patte…  [done, pri:100]
+│   │   ├── task:16  Create `internal/tree/` package skeleton with `Node` type, `Walk(db, opts) (N…  [done, pri:100]
+│   │   ├── task:17  Implement the recursive CTE plan-tree walker in `internal/tree/walk.go`, mirr…  [done, pri:100]
+│   │   ├── task:18  Implement `Render(n Node, w io.Writer, opts RenderOpts) error` for indented U…  [done, pri:100]
+│   │   ├── task:19  Implement `RenderJSON(n Node, w io.Writer) error` for the `--json` shape — …  [done, pri:100]
+│   │   ├── task:20  Implement `--ascii` charset toggle in `Render` (ASCII `+--` / `|` / `\--` ins…  [done, pri:100]
+│   │   ├── task:21  Implement `-L <N>` / `--depth <N>` depth-clip in the walker (top-level plan i…  [done, pri:100]
+│   │   ├── task:22  Implement `-I <pattern>` / `--ignore` and `-P <pattern>` / `--match` glob-pat…  [done, pri:100]
 │   │   ├── task:23  Implement `--ignore-case` toggle that makes `-I`/`-P` case-insensitive.  [done, pri:100]
-│   │   ├── task:24  Implement `--kind <list>` filter (repeatable) over entity kinds: `plan`, `task`…  [done, pri:100]
+│   │   ├── task:24  Implement `--kind <list>` filter (repeatable) over entity kinds: `plan`, `tas…  [done, pri:100]
 │   │   ├── task:25  Implement `--status <list>` filter (repeatable) over entity statuses.  [done, pri:100]
-│   │   ├── task:26  Implement `-r` / `--reverse` sort-reversal and `-t` / `--sort updated` / `-c` /…  [done, pri:100]
+│   │   ├── task:26  Implement `-r` / `--reverse` sort-reversal and `-t` / `--sort updated` / `-c`…  [done, pri:100]
 │   │   ├── task:27  Implement `--dirsfirst` (default on) and `--no-dirsfirst` (interleave by id).  [done, pri:100]
-│   │   ├── task:28  Implement `--noreport` (suppress summary footer) and `--prune` (hide empty bran…  [done, pri:100]
+│   │   ├── task:28  Implement `--noreport` (suppress summary footer) and `--prune` (hide empty br…  [done, pri:100]
 │   │   ├── task:29  Implement `-i` / `--no-indent` (disable indentation lines, print flat tree).  [done, pri:100]
 │   │   ├── task:30  Implement long-title truncation at 80 chars with `--no-truncate` opt out.  [done, pri:100]
-│   │   ├── task:31  Implement `--scope <X>` and `--all-scopes` (Planar-specific): `--all-scopes` al…  [done, pri:100]
-│   │   ├── task:32  Wire `cmd/planar/cmd_tree.go` cobra command parsing all flags and dispatching t…  [done, pri:100]
-│   │   └── task:33  Update `docs/cli-spec.md` with a new `## Domain: tree` section documenting the …  [done, pri:100]
+│   │   ├── task:31  Implement `--scope <X>` and `--all-scopes` (Planar-specific): `--all-scopes` …  [done, pri:100]
+│   │   ├── task:32  Wire `cmd/planar/cmd_tree.go` cobra command parsing all flags and dispatching…  [done, pri:100]
+│   │   └── task:33  Update `docs/cli-spec.md` with a new `## Domain: tree` section documenting th…  [done, pri:100]
 │   ├── plan:7 [done]  M3 — Tests and docs
-│   │   ├── task:34  Add `Test_ScopeColumn_ListOutput` to `TaskSuite` covering `task list` JSON-vs-h…  [done, pri:100]
+│   │   ├── task:34  Add `Test_ScopeColumn_ListOutput` to `TaskSuite` covering `task list` JSON-vs…  [done, pri:100]
 │   │   ├── task:35  Add `Test_ScopeColumn_ListOutput` to `PlanSuite` covering 
```

_(diff truncated at 8000 bytes; full body in parity-gap-report.json)_

#### 3. `resume --json` — invocation `json`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 8729B, stderr 0B)
- Zig exit: `0` (stdout 7619B, stderr 0B)

```diff
--- go
+++ zig
@@ -1,180 +1 @@
-{
-  "identity": {
-    "task_id": 1,
-    "plan_id": 2,
-    "title": "Add --github-strategy flag to ext propagate with validation",
-    "status": "done",
-    "scope_kind": "global"
-  },
-  "state": {
-    "status": "done",
-    "next_action": "Read cmd/planar/cmd_ext.go propagateCmd; add the flag def + parse path; validation lives there",
-    "last_action_at": "2026-05-25T16:34:45.311Z",
-    "last_action_body": "plan_status: 351\nplan_title: Behavior parity gap analysis + regression tests\nfrom_status: done\nto_status: active\ntrigger: recompute\ntrigger_task_id: 0\ntask_aggregate: todo=1 doing=0 blocked=0 done=0 cancelled=1\n"
-  },
-  "plan": {
-    "plan_id": 2,
-    "plan_title": "ext propagate --github-strategy override",
-    "completed": [],
-    "current": [],
-    "remaining": []
-  },
-  "operational_plane": {
-    "links": [
-      {
-        "link_id": 6,
-        "external_id": "rdrsss/planar#1",
-        "sync_status": "never"
-      }
-    ],
-    "refresh_note": "operational plane: unable to refresh (no registry)"
-  },
-  "recent_activity": [
-    {
-      "session_id": 2,
-      "prefix": "note",
-      "body": "plan_status: 351\nplan_title: Behavior parity gap analysis + regression tests\nfrom_status: done\nto_status: active\ntrigger: recompute\ntrigger_task_id: 0\ntask_aggregate: todo=1 doing=0 blocked=0 done=0 cancelled=1\n",
-      "created_at": "2026-05-25T16:34:45.311Z"
-    },
-    {
-      "session_id": 2,
-      "prefix": "note",
-      "body": "plan_status: 351\nplan_title: Behavior parity gap analysis + regression tests\nfrom_status: draft\nto_status: done\ntrigger: recompute\ntrigger_task_id: 0\ntask_aggregate: todo=0 doing=0 blocked=0 done=0 cancelled=1\n",
-      "created_at": "2026-05-25T16:34:28.381Z"
-    },
-    {
-      "session_id": 2,
-      "prefix": "note",
-      "body": "plan_status: 334\nplan_title: M21 — Cutover\nfrom_status: draft\nto_status: active\ntrigger: task_update\ntrigger_task_id: 2356\ntask_aggregate: todo=10 doing=1 blocked=0 done=2 cancelled=0\n",
-      "created_at": "2026-05-25T14:33:19.636Z"
-    },
-    {
-      "session_id": 2,
-      "prefix": "note",
-      "body": "plan_status: 348\nplan_title: M20 — Skills renderer (unified source projection)\nfrom_status: active\nto_status: done\ntrigger: task_done\ntrigger_task_id: 2332\ntask_aggregate: todo=0 doing=0 blocked=0 done=7 cancelled=0\n",
-      "created_at": "2026-05-25T07:56:19.506Z"
-    },
-    {
-      "session_id": 2,
-      "prefix": "note",
-      "body": "plan_status: 333\nplan_title: M19 — Integration test suite + JSON-key audit\nfrom_status: active\nto_status: done\ntrigger: task_done\ntrigger_task_id: 2183\ntask_aggregate: todo=0 doing=0 blocked=0 done=16 cancelled=0\n",
-      "created_at": "2026-05-25T07:16:51.967Z"
-    },
-    {
-      "session_id": 2,
-      "prefix": "note",
-      "body": "plan_status: 332\nplan_title: M18 — pl-import + pl-synthesize (LLM-driven)\nfrom_status: active\nto_status: done\ntrigger: task_done\ntrigger_task_id: 2172\ntask_aggregate: todo=0 doing=0 blocked=0 done=7 cancelled=0\n",
-      "created_at": "2026-05-25T05:47:44.823Z"
-    },
-    {
-      "session_id": 2,
-      "prefix": "note",
-      "body": "plan_status: 331\nplan_title: M17 — Local (sandbox skills + agents)\nfrom_status: active\nto_status: done\ntrigger: task_done\ntrigger_task_id: 2165\ntask_aggregate: todo=0 doing=0 blocked=0 done=4 cancelled=0\n",
-      "created_at": "2026-05-25T05:02:02.171Z"
-    },
-    {
-      "session_id": 2,
-      "prefix": "note",
-      "body": "plan_status: 329\nplan_title: M15 — Workspace (cross-repo routing)\nfrom_status: active\nto_status: done\ntrigger: task_done\ntrigger_task_id: 2156\ntask_aggregate: todo=0 doing=0 blocked=0 done=5 cancelled=0\n",
-      "created_at": "2026-05-25T04:40:12.853Z"
-    },
-    {
-      "session_id": 2,
-      "prefix": "note",
-      "body": "plan_status: 318\nplan_title: M4 — Editor flow + workbench parse/render (read-side)\nfrom_status: active\nto_status: done\ntrigger: task_done\ntrigger_task_id: 2290\ntask_aggregate: todo=0 doing=0 blocked=0 done=11 cancelled=0\n",
-      "created_at": "2026-05-25T03:32:49.973Z"
-    },
-    {
-      "session_id": 2,
-      "prefix": "note",
-      "body": "plan_status: 315\nplan_title: M1 — Complete the planning sextet\nfrom_status: active\nto_status: done\ntrigger: task_done\ntrigger_task_id: 2225\ntask_aggregate: todo=0 doing=0 blocked=0 done=42 cancelled=2\n",
-      "created_at": "2026-05-25T03:16:40.294Z"
-    },
-    {
-      "session_id": 2,
-      "prefix": "note",
-      "body": "plan_status: 324\nplan_title: M10 — Templates + ext propagate\nfrom_status: active\nto_status: done\ntrigger: task_done\ntrigger_task_id: 2301\ntask_aggregate: todo=0 doing=0 blocked=0 done=8 cancelled=0\n",
-      "created_at": "2026-05-25T01:57:24.832Z"
-    },
-    {
-      "session_id": 2,
-      "prefix": "note",
-      "body": "plan_status: 315\nplan_title: M1 — Complete the planning sextet\nfrom_status: done\nto_status: active\ntrigger: task_done\ntrigger_task_id: 2197\ntask_aggregate: todo=31 doing=0 blocked=0 done=11 cancelled=2\n",
-      "created_at": "2026-05-25T01:18:30.359Z"
-    },
-    {
-      "session_id": 2,
-      "prefix": "note",
-      "body": "plan_status: 330\nplan_title: M16 — Plan steps + remaining task subverbs + promote / demote\nfrom_status: done\nto_status: active\ntrigger: task_add\ntrigger_task_id: 2320\ntask_aggregate: todo=1 doing=0 blocked=0 done=5 cancelled=0\n",
-      "created_at": "2026-05-25T01:18:13.212Z"
-    },
-    {
-      "session_id": 2,
-      "prefix": "note",
-      "body": "plan_status: 330\nplan_title: M16 — Plan steps + remaining task subverbs + promote / demote\nfrom_status: active\nto_status: done\ntrigger: task_done\ntrigger_task_id: 2161\ntask_aggregate: todo=0 doing=0 blocked=0 done=5 cancelled=0\n",
-      "created_at": "2026-05-25T01:08:44.766Z"
-    },
-    {
-      "session_id": 2,
-      "prefix": "note",
-      "body": "plan_status: 327\nplan_title: M13 — Doc system: lint, manifest, drift, citations\nfrom_status: active\nto_status: done\ntrigger: task_done\ntrigger_task_id: 2148\ntask_aggregate: todo=0 doing=0 blocked=0 done=7 cancelled=0\n",
-      "created_at": "2026-05-25T01:00:52.939Z"
-    },
-    {
-      "session_id": 2,
-      "prefix": "note",
-      "body": "plan_status: 323\nplan_title: M9 — Spec ingestor\nfrom_status: active\nto_status: done\ntrigger: task_done\ntrigger_task_id: 2131\ntask_aggregate: todo=0 doing=0 blocked=0 done=5 cancelled=0\n",
-      "created_at": "2026-05-25T00:54:09.910Z"
-    },
-    {
-      "session_id": 2,
-      "prefix": "note",
-      "body": "plan_status: 328\nplan_title: M14 — Config (~/.planar/config.toml)\nfrom_status: active\nto_status: done\ntrigger: task_done\ntrigger_task_id: 2151\ntask_aggregate: todo=0 doing=0 blocked=0 done=3 cancelled=0\n",
-      "created_at": "2026-05-24T23:36:54.118Z"
-    },
-    {
-      "session_id": 2,
-      "prefix": "note",
-      "body": "plan_status: 322\nplan_title: M8 — External plane: ext_systems + link + sync (Jira + GitHub)\nfrom_status: active\nto_status: done\ntrigger: task_done\ntrigger_task_id: 2297\ntask_aggregate: todo=0 doing=0 blocked=0 done=10 cancelled=0\n",
-      "created_at": "2026-05-24T23:31:14.099Z"
-    },
-    {
-      "session_id": 2,
-      "prefix": "note",
-      "body": "plan_status: 326\nplan_title: M12 — Tree (hierarchical drill-down)\nfrom_status: done\nto_status: active\ntrigger: task_add\ntrigger_task_id: 2298\ntask_aggregate: todo=1 doing=0 blocked=0 done=2 cancelled=0\n",
-      "created_at": "2026-05-24T22:52:54.947Z"
-    },
-    {
-      "session_id": 2,
-      "prefix": "note",
-      "body": "plan_status: 326\nplan_title: M12 — Tree (hierarchical drill-down)\nfrom_status: active\nto_status: done\ntrigger: task_done\ntrigger_task_id: 2141\ntask_aggregate: todo=0 doing=0 blocked=0 done=2 cancelled=0\n",
-      "crea
```

_(diff truncated at 8000 bytes; full body in parity-gap-report.json)_

#### 4. `resume ` — invocation `no-args`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 4041B, stderr 0B)
- Zig exit: `0` (stdout 6620B, stderr 0B)

```diff
--- go
+++ zig
@@ -20,73 +20,180 @@
 
 ## 3. Plan Position
   plan: 2 "ext propagate --github-strategy override"
-  completed (0):  (none)
-  current   (0):  (none)
-  remaining (0):  (none)
+  completed: 0  current: 0  remaining: 0
 
-## 4. Operational Plane (1 link(s))
-  note: operational plane: unable to refresh (no registry)
-  link:6  ext:rdrsss/planar#1  sync:never
+## 4. Operational Plane
+  link 6: rdrsss/planar#1 [never]  
 
 ## 5. Recent Activity (23 entries)
   [note]  session:2  2026-05-25T16:34:45.311Z  — plan_status: 351
 plan_title: Behavior parity gap analysis + regression tests
-fro...
+from_status: done
+to_status: active
+trigger: recompute
+trigger_task_id: 0
+task_aggregate: todo=1 doing=0 blocked=0 done=0 cancelled=1
+
   [note]  session:2  2026-05-25T16:34:28.381Z  — plan_status: 351
 plan_title: Behavior parity gap analysis + regression tests
-fro...
+from_status: draft
+to_status: done
+trigger: recompute
+trigger_task_id: 0
+task_aggregate: todo=0 doing=0 blocked=0 done=0 cancelled=1
+
   [note]  session:2  2026-05-25T14:33:19.636Z  — plan_status: 334
 plan_title: M21 — Cutover
 from_status: draft
-to_status: activ...
+to_status: active
+trigger: task_update
+trigger_task_id: 2356
+task_aggregate: todo=10 doing=1 blocked=0 done=2 cancelled=0
+
   [note]  session:2  2026-05-25T07:56:19.506Z  — plan_status: 348
-plan_title: M20 — Skills renderer (unified source projection)...
+plan_title: M20 — Skills renderer (unified source projection)
+from_status: active
+to_status: done
+trigger: task_done
+trigger_task_id: 2332
+task_aggregate: todo=0 doing=0 blocked=0 done=7 cancelled=0
+
   [note]  session:2  2026-05-25T07:16:51.967Z  — plan_status: 333
 plan_title: M19 — Integration test suite + JSON-key audit
-fro...
+from_status: active
+to_status: done
+trigger: task_done
+trigger_task_id: 2183
+task_aggregate: todo=0 doing=0 blocked=0 done=16 cancelled=0
+
   [note]  session:2  2026-05-25T05:47:44.823Z  — plan_status: 332
 plan_title: M18 — pl-import + pl-synthesize (LLM-driven)
-from...
+from_status: active
+to_status: done
+trigger: task_done
+trigger_task_id: 2172
+task_aggregate: todo=0 doing=0 blocked=0 done=7 cancelled=0
+
   [note]  session:2  2026-05-25T05:02:02.171Z  — plan_status: 331
 plan_title: M17 — Local (sandbox skills + agents)
-from_status...
+from_status: active
+to_status: done
+trigger: task_done
+trigger_task_id: 2165
+task_aggregate: todo=0 doing=0 blocked=0 done=4 cancelled=0
+
   [note]  session:2  2026-05-25T04:40:12.853Z  — plan_status: 329
 plan_title: M15 — Workspace (cross-repo routing)
-from_status:...
+from_status: active
+to_status: done
+trigger: task_done
+trigger_task_id: 2156
+task_aggregate: todo=0 doing=0 blocked=0 done=5 cancelled=0
+
   [note]  session:2  2026-05-25T03:32:49.973Z  — plan_status: 318
-plan_title: M4 — Editor flow + workbench parse/render (read-s...
+plan_title: M4 — Editor flow + workbench parse/render (read-side)
+from_status: active
+to_status: done
+trigger: task_done
+trigger_task_id: 2290
+task_aggregate: todo=0 doing=0 blocked=0 done=11 cancelled=0
+
   [note]  session:2  2026-05-25T03:16:40.294Z  — plan_status: 315
 plan_title: M1 — Complete the planning sextet
-from_status: ac...
+from_status: active
+to_status: done
+trigger: task_done
+trigger_task_id: 2225
+task_aggregate: todo=0 doing=0 blocked=0 done=42 cancelled=2
+
   [note]  session:2  2026-05-25T01:57:24.832Z  — plan_status: 324
 plan_title: M10 — Templates + ext propagate
-from_status: acti...
+from_status: active
+to_status: done
+trigger: task_done
+trigger_task_id: 2301
+task_aggregate: todo=0 doing=0 blocked=0 done=8 cancelled=0
+
   [note]  session:2  2026-05-25T01:18:30.359Z  — plan_status: 315
 plan_title: M1 — Complete the planning sextet
-from_status: do...
+from_status: done
+to_status: active
+trigger: task_done
+trigger_task_id: 2197
+task_aggregate: todo=31 doing=0 blocked=0 done=11 cancelled=2
+
   [note]  session:2  2026-05-25T01:18:13.212Z  — plan_status: 330
-plan_title: M16 — Plan steps + remaining task subverbs + prom...
+plan_title: M16 — Plan steps + remaining task subverbs + promote / demote
+from_status: done
+to_status: active
+trigger: task_add
+trigger_task_id: 2320
+task_aggregate: todo=1 doing=0 blocked=0 done=5 cancelled=0
+
   [note]  session:2  2026-05-25T01:08:44.766Z  — plan_status: 330
-plan_title: M16 — Plan steps + remaining task subverbs + prom...
+plan_title: M16 — Plan steps + remaining task subverbs + promote / demote
+from_status: active
+to_status: done
+trigger: task_done
+trigger_task_id: 2161
+task_aggregate: todo=0 doing=0 blocked=0 done=5 cancelled=0
+
   [note]  session:2  2026-05-25T01:00:52.939Z  — plan_status: 327
-plan_title: M13 — Doc system: lint, manifest, drift, citation...
+plan_title: M13 — Doc system: lint, manifest, drift, citations
+from_status: active
+to_status: done
+trigger: task_done
+trigger_task_id: 2148
+task_aggregate: todo=0 doing=0 blocked=0 done=7 cancelled=0
+
   [note]  session:2  2026-05-25T00:54:09.910Z  — plan_status: 323
 plan_title: M9 — Spec ingestor
 from_status: active
-to_status:...
+to_status: done
+trigger: task_done
+trigger_task_id: 2131
+task_aggregate: todo=0 doing=0 blocked=0 done=5 cancelled=0
+
   [note]  session:2  2026-05-24T23:36:54.118Z  — plan_status: 328
 plan_title: M14 — Config (~/.planar/config.toml)
-from_status:...
+from_status: active
+to_status: done
+trigger: task_done
+trigger_task_id: 2151
+task_aggregate: todo=0 doing=0 blocked=0 done=3 cancelled=0
+
   [note]  session:2  2026-05-24T23:31:14.099Z  — plan_status: 322
-plan_title: M8 — External plane: ext_systems + link + sync (J...
+plan_title: M8 — External plane: ext_systems + link + sync (Jira + GitHub)
+from_status: active
+to_status: done
+trigger: task_done
+trigger_task_id: 2297
+task_aggregate: todo=0 doing=0 blocked=0 done=10 cancelled=0
+
   [note]  session:2  2026-05-24T22:52:54.947Z  — plan_status: 326
 plan_title: M12 — Tree (hierarchical drill-down)
-from_status:...
+from_status: done
+to_status: active
+trigger: task_add
+trigger_task_id: 2298
+task_aggregate: todo=1 doing=0 blocked=0 done=2 cancelled=0
+
   [note]  session:2  2026-05-24T22:47:43.728Z  — plan_status: 326
 plan_title: M12 — Tree (hierarchical drill-down)
-from_status:...
+from_status: active
+to_status: done
+trigger: task_done
+trigger_task_id: 2141
+task_aggregate: todo=0 doing=0 blocked=0 done=2 cancelled=0
+
   [note]  session:2  2026-05-24T21:56:05.928Z  — plan_status: 321
-plan_title: M7 — Runtime entities: handoff, resume, capture, ...
+plan_title: M7 — Runtime entities: handoff, resume, capture, audit-read
+from_status: active
+to_status: done
+trigger: task_done
+trigger_task_id: 2117
+task_aggregate: todo=0 doing=0 blocked=0 done=8 cancelled=0
+
   [action]  session:2  2026-05-24T21:54:14.270Z  — task add: Go smoke (id:2294)
   [action]  session:2  2026-05-24T21:54:14.241Z  — session opened
 
```

#### 5. `pl-import --help` — invocation `help`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 3965B, stderr 0B)
- Zig exit: `0` (stdout 1021B, stderr 0B)

```diff
--- go
+++ zig
@@ -1,47 +1,27 @@
-pl-import translates the planning artefacts of an existing repository into
-Planar's data model. It discovers tech specs, roadmap milestones, ADRs, and
-backlog files, infers completion status from checkbox state and git history,
-and produces an ImportPlan for review before committing.
+pl-import
 
-Pipeline: discover → parse → infer → plan → (optional) apply.
+pl-import translates the planning artefacts of an existing
+  repository into Planar's data model. It discovers
+  tech specs, roadmap milestones, ADRs, and backlog files,
+  infers completion status from checkbox state and git history,
+  and produces an ImportPlan for review before committing.
 
-Default mode is PREVIEW: no writes are made. The diff tree is printed to
-stdout and the command exits 0.
+USAGE:
+  pl-import [flags] <repo-root>
 
-Use --apply to commit the import. Use --dry-run to emit a JSON report instead
-of the human-readable tree (never writes, regardless of --apply).
+FLAGS:
+  --from-github         (bool) default=false — Pull source from GitHub issues
+  --dry-run             (bool) default=false
+  --strict              (bool) default=false
+  --threshold           (string) — Similarity threshold (float as string for now)
+  --roadmap             (string) — Path to a roadmap source
+  --apply               (bool) default=false
+  --apply-removals      (bool) default=false
+  --no-status-inference (bool) default=false
+  --interpret           (bool) default=false
+  --no-interpret        (bool) default=false
+  --scope               (string)
+  --json                (bool) default=false
 
-Idempotency: items already present in the database (matched by title / source-
-path fingerprint) are reported as Skipped and never double-imported.
-
-Usage:
-  planar pl-import <repo-root> [flags]
-
-Flags:
-      --accept-spec all          Accept a specific forward spec proposal (M6). Pass all to accept every proposal. Repeatable.
-      --apply                    Commit the import; without this flag, preview only
-      --apply-removals           Together with --apply, commit the proposed removals from the M7 diff (tasks cancelled, plans abandoned, artifacts retired, decisions superseded). --apply-removals alone (without --apply) is rejected as a user error.
-      --code-layout string       Override the codeprobe project-shape matcher. One of: swift, go, node, python, mixed. Empty (default) auto-detects.
-      --dry-run                  Print JSON report instead of human preview; never writes
-      --from-github              Also import open GitHub issues as tasks (requires gh CLI on PATH)
-  -h, --help                     help for pl-import
-      --interpret                Run the LLM interpretation pass after the deterministic classifier (M3).
-      --no-forward-specs         Skip the forward-specs phase entirely (no proposals materialized).
-      --no-interpret             Skip the LLM interpretation pass (default).
-      --no-status-inference      Default every imported task to status=todo (skip git-log correlation + branch matching). Useful for greenfield or docs-only repos where status correlation is unreliable by construction.
-      --refresh-status           Re-run inference on already-imported tasks and update their status if the inferred status now differs (e.g. new commits match a previously-todo task).
-      --roadmap string           Explicit path to the roadmap file (overrides auto-discovery)
-      --scope string             Scope for created entities (default: active scope or global)
-      --strict                   Require every inferred task to score at or above --threshold. Without --strict, the confidence floor refuses imports where more than half of tasks fall below --threshold (50% floor). With --strict, the floor effectively becomes 100%: a single below-threshold task refuses the import.
-      --threshold float          Confidence threshold for git-log correlation (0.0..1.0). The confidence floor evaluates each inferred task against this value; pass --threshold 0.0 to disable the floor entirely. (default 0.7)
-      --trust-status-inference   Explicit bypass for the plan 215 M1 defect 1 >25% auto-done refusal. Pass when you have a clean repo with high genuinely-done count and have verified the inferred done marks.
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
+POSITIONAL ARGUMENTS:
+  <repo-root>     (string)
```

#### 6. `tree --help` — invocation `help`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 4096B, stderr 0B)
- Zig exit: `0` (stdout 795B, stderr 0B)

```diff
--- go
+++ zig
@@ -1,54 +1,20 @@
-Render a hierarchical view of Planar entities for one or all scopes.
+tree
 
-Walks plans (via parent_plan_id), tasks (via plan_id and parent_task_id),
-and entity_links(derives-from) to gather artifacts, decisions, scenarios,
-and questions linked to each plan.
+Render a hierarchical view of Planar entities for one or all
+  scopes.
 
-Flag surface mirrors tree(1) where the semantic translates. Filesystem-
-specific flags from tree(1) (-a, -d, -f, -s, -h, -p, -u, -g, -D, --inodes,
---device, -Q, -X, -H, -v, --filelimit, --matchdirs, -C, -o) are rejected
-at parse time with an explicit pointer at the Planar alternative — see
-docs/cli-spec.md.
+  Walks plans (via parent_plan_id), tasks (via plan_id and
+  parent_task_id), and entity_links(derives-from) to gather
+  artifacts, decisions, scenarios, and questions linked to each plan.
 
-Usage:
-  planar tree [flags]
+USAGE:
+  tree [flags]
 
-Flags:
-      --all-scopes                    Render every scope as a separate section; always emits the global section even when empty.
-      --artifact-status stringArray   Filter artifacts by status. Repeatable. Overrides --status for artifacts.
-      --ascii                         Use ASCII box-drawing characters (default: Unicode).
-      --decision-status stringArray   Filter decisions by status. Repeatable. Overrides --status for decisions.
-  -L, --depth int                     Maximum recursion depth (0 = unbounded). Top-level plan is depth 0.
-      --dirsfirst                     Group plans first, then tasks, then everything else (default). (default true)
-  -h, --help                          help for tree
-  -I, --ignore stringArray            Glob pattern (title or slug) to exclude. Repeatable.
-      --ignore-case                   Case-insensitive matching for -I / -P.
-  -J, --json                          Emit nested JSON instead of indented text.
-      --kind stringArray              Filter entity kinds. Repeatable. Valid: plan, task, artifact, decision, scenario, question.
-  -P, --match stringArray             Glob pattern (title or slug) to include. Repeatable.
-      --no-dirsfirst                  Interleave entity kinds by id within each level.
-  -i, --no-indent                     Print flat tree without indentation lines.
-      --no-truncate                   Do not truncate long entity titles (default: truncate at 80 chars).
-      --noreport                      Suppress the summary footer at end of output.
-      --plan-status stringArray       Filter plans by status. Repeatable. Overrides --status for plans.
-      --prune                         Hide empty branches (plans with zero descendants).
-      --question-status stringArray   Filter questions by status. Repeatable. Overrides --status for questions.
-  -r, --reverse                       Reverse sort order within each level.
-      --scaffold-all                  Legacy: keep parent plans whenever any child (including artifacts/decisions/scenarios) survives the filter.
-      --scenario-status stringArray   Filter scenarios by status. Repeatable. Overrides --status for scenarios.
-      --scope string                  Render this scope only (global, repo, repo:<slug>, assoc:<slug>). Default: active scope.
-      --sort string                   Sort by: id (default), updated, created, unsorted.
-  -c, --sort-created                  Sort by created_at (equivalent to --sort created).
-  -t, --sort-updated                  Sort by updated_at (equivalent to --sort updated).
-      --status stringArray            Filter by status across all entity kinds (cross-kind). Repeatable.
-      --task-status stringArray       Filter tasks by status. Repeatable. Overrides --status for tasks.
-  -U, --unsorted                      Preserve DB insertion order (equivalent to --sort unsorted).
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
+FLAGS:
+  --scope               (string) — Limit to a single scope slug
+  --all-scopes          (bool) default=false — Include every scope
+  --depth               (int) default=-1 — Max tree depth (-1 = unbounded)
+  --kind                (string) — Restrict to one kind (repeatable in Go; single here for now)
+  --status              (string) — Restrict by status (repeatable in Go; single here for now)
+  --sort                (string) — Sort key
+  --json                (bool) default=false
```

#### 7. `pl-synthesize --help` — invocation `help`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 3478B, stderr 0B)
- Zig exit: `0` (stdout 884B, stderr 0B)

```diff
--- go
+++ zig
@@ -1,46 +1,25 @@
-pl-synthesize reads a repository's existing planning docs, source code,
-and git history AS INPUT for an LLM synthesis pass. It produces fresh
-product-spec / tech-spec / roadmap artifacts (NOT a verbatim
-transcription) and proposes them via the same workbench pipeline
-pl-spec-draft uses.
+pl-synthesize
 
-Pick this verb when the source repo is messy, docs-only, or
-mid-evolution. Pick pl-import for clean structured docs whose contents
-should be transcribed verbatim. The two verbs are architecturally
-similar but contractually distinct.
+pl-synthesize reads a repository's existing planning docs, source
+  code, and git history AS INPUT for an LLM synthesis pass. It
+  produces fresh product-spec / tech-spec / roadmap artifacts (NOT a
+  verbatim transcription) and proposes them via the same workbench
+  pipeline as the planner agent.
 
-Pipeline: discover → codeprobe → build Request → cache check → (skill
-writes Result on miss) → merge → preview / apply.
+USAGE:
+  pl-synthesize [flags] <repo-root>
 
-Default mode is PREVIEW: no writes are made. Use --apply to commit.
-Use --literal to delegate to pl-import (the transcription verb) without
-retyping the args.
+FLAGS:
+  --apply               (bool) default=false
+  --apply-removals      (bool) default=false
+  --scope               (string)
+  --code-layout         (string)
+  --treat-as-greenfield (bool) default=false
+  --treat-as-nongreenfield  (bool) default=false
+  --threshold           (string) — Similarity threshold (float as string for now)
+  --literal             (bool) default=false
+  --dry-run             (bool) default=false
+  --json                (bool) default=false
 
-Usage:
-  planar pl-synthesize <repo-root> [flags]
-
-Flags:
-      --accept-spec all          Accept a specific forward-spec proposal. Pass all to accept every proposal. Repeatable.
-      --apply                    Commit the synthesis; without this flag, preview only.
-      --apply-removals           Together with --apply, commit the proposed removals (tasks cancelled, plans abandoned, artifacts retired, decisions superseded).
-      --code-layout string       Override the codeprobe project-shape matcher. One of: swift, go, node, python, mixed. Empty (default) auto-detects.
-      --dry-run                  Print JSON report instead of human preview; never writes.
-  -h, --help                     help for pl-synthesize
-      --json-out                 Emit machine-readable JSON output (alias for global --json scoped to this verb).
-      --literal                  Operator escape hatch: delegate to pl-import (the transcription verb) with the same args. Use this when you decide mid-invocation that the repo is clean enough for literal transcription.
-      --no-forward-specs         Skip the forward-specs phase entirely (no proposals materialized).
-      --no-status-inference      Default every synthesized task to status=todo (skip status inference). Implied by --treat-as-greenfield.
-      --scope string             Scope for created entities (default: active scope or global).
-      --threshold float          Confidence threshold for the deterministic confidence floor (0.0..1.0). Pass --threshold 0.0 to disable. (default 0.7)
-      --treat-as-greenfield      Force greenfield mode (every task defaults to status=todo) even when code evidence exists. Useful for stale-WIP branches where the code is misleading.
-      --treat-as-nongreenfield   Bypass greenfield auto-detection. Treats the repo as having implementation code even when EvidenceMap shows zero evidence. Rarely needed; mostly an escape hatch for non-conventional layouts where codeprobe under-detects.
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
+POSITIONAL ARGUMENTS:
+  <repo-root>     (string)
```

#### 8. `task --help` — invocation `help`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 1878B, stderr 0B)
- Zig exit: `0` (stdout 1128B, stderr 0B)

```diff
--- go
+++ zig
@@ -1,39 +1,27 @@
+task
+
 Manage tasks — the discrete units of work.
 
-Tasks may belong to a plan (--plan) or another task (--parent), and carry
-the next_action field required by resume validate.
-Status lifecycle: todo → doing → done / cancelled; blocked is set via task block.
+  Tasks may belong to a plan (--plan) or another task (--parent), and
+  carry the next_action field required by resume validate.
+  Status lifecycle: todo → doing → done / cancelled; blocked is set
+  via task block.
 
-Usage:
-  planar task [command]
+USAGE:
+  task <command>
 
-Available Commands:
-  add         Create a new task.
-  block       Mark a task as blocked and record the blocking relationship.
-  cancel      Cancel one or more tasks.
-  diff        Show a unified diff between the DB's task content and the workbench file.
-  done        Mark one or more tasks as done.
-  edit        Edit a task in $EDITOR (editor-first flow).
-  link        Create an entity link from a task to another entity.
-  list        List tasks.
-  reopen      Reopen a done or cancelled task with an audit-trail entry.
-  review      Bulk-review open tasks in $EDITOR with per-task editable fields.
-  show        Show full task details.
-  touches     Manage repo-touches links on a task.
-  update      Update mutable fields on a task.
-  view        View the task's workbench file in $PAGER.
-
-Flags:
-  -h, --help   help for task
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar task [command] --help" for more information about a command.
+COMMANDS:
+  add             Create a new task.
+  show            Show full task details.
+  list            List tasks.
+  update          Update mutable fields on a task.
+  edit            Edit a task in $EDITOR (editor-first flow).
+  view            View task's workbench file.
+  diff            Diff task against its database-stored version.
+  review          Reviewer entry point for task diff.
+  done            Mark a task as done (single-arg form; Go supports variadic).
+  cancel          Cancel a task (single-arg form; Go supports variadic).
+  block           Mark a task as blocked and record the blocking relationship.
+  link            Create an entity link from a task to another entity.
+  reopen          Reopen a done or cancelled task with an audit-trail entry.
+  touches         Manage repo-touches links on a task.
```

#### 9. `task ` — invocation `no-args`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 1878B, stderr 0B)
- Zig exit: `0` (stdout 1128B, stderr 0B)

```diff
--- go
+++ zig
@@ -1,39 +1,27 @@
+task
+
 Manage tasks — the discrete units of work.
 
-Tasks may belong to a plan (--plan) or another task (--parent), and carry
-the next_action field required by resume validate.
-Status lifecycle: todo → doing → done / cancelled; blocked is set via task block.
+  Tasks may belong to a plan (--plan) or another task (--parent), and
+  carry the next_action field required by resume validate.
+  Status lifecycle: todo → doing → done / cancelled; blocked is set
+  via task block.
 
-Usage:
-  planar task [command]
+USAGE:
+  task <command>
 
-Available Commands:
-  add         Create a new task.
-  block       Mark a task as blocked and record the blocking relationship.
-  cancel      Cancel one or more tasks.
-  diff        Show a unified diff between the DB's task content and the workbench file.
-  done        Mark one or more tasks as done.
-  edit        Edit a task in $EDITOR (editor-first flow).
-  link        Create an entity link from a task to another entity.
-  list        List tasks.
-  reopen      Reopen a done or cancelled task with an audit-trail entry.
-  review      Bulk-review open tasks in $EDITOR with per-task editable fields.
-  show        Show full task details.
-  touches     Manage repo-touches links on a task.
-  update      Update mutable fields on a task.
-  view        View the task's workbench file in $PAGER.
-
-Flags:
-  -h, --help   help for task
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar task [command] --help" for more information about a command.
+COMMANDS:
+  add             Create a new task.
+  show            Show full task details.
+  list            List tasks.
+  update          Update mutable fields on a task.
+  edit            Edit a task in $EDITOR (editor-first flow).
+  view            View task's workbench file.
+  diff            Diff task against its database-stored version.
+  review          Reviewer entry point for task diff.
+  done            Mark a task as done (single-arg form; Go supports variadic).
+  cancel          Cancel a task (single-arg form; Go supports variadic).
+  block           Mark a task as blocked and record the blocking relationship.
+  link            Create an entity link from a task to another entity.
+  reopen          Reopen a done or cancelled task with an audit-trail entry.
+  touches         Manage repo-touches links on a task.
```

#### 10. `task ` — invocation `real-cwd`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 1878B, stderr 0B)
- Zig exit: `0` (stdout 1128B, stderr 0B)
- cwd: `<CWD_DIR>`

```diff
--- go
+++ zig
@@ -1,39 +1,27 @@
+task
+
 Manage tasks — the discrete units of work.
 
-Tasks may belong to a plan (--plan) or another task (--parent), and carry
-the next_action field required by resume validate.
-Status lifecycle: todo → doing → done / cancelled; blocked is set via task block.
+  Tasks may belong to a plan (--plan) or another task (--parent), and
+  carry the next_action field required by resume validate.
+  Status lifecycle: todo → doing → done / cancelled; blocked is set
+  via task block.
 
-Usage:
-  planar task [command]
+USAGE:
+  task <command>
 
-Available Commands:
-  add         Create a new task.
-  block       Mark a task as blocked and record the blocking relationship.
-  cancel      Cancel one or more tasks.
-  diff        Show a unified diff between the DB's task content and the workbench file.
-  done        Mark one or more tasks as done.
-  edit        Edit a task in $EDITOR (editor-first flow).
-  link        Create an entity link from a task to another entity.
-  list        List tasks.
-  reopen      Reopen a done or cancelled task with an audit-trail entry.
-  review      Bulk-review open tasks in $EDITOR with per-task editable fields.
-  show        Show full task details.
-  touches     Manage repo-touches links on a task.
-  update      Update mutable fields on a task.
-  view        View the task's workbench file in $PAGER.
-
-Flags:
-  -h, --help   help for task
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar task [command] --help" for more information about a command.
+COMMANDS:
+  add             Create a new task.
+  show            Show full task details.
+  list            List tasks.
+  update          Update mutable fields on a task.
+  edit            Edit a task in $EDITOR (editor-first flow).
+  view            View task's workbench file.
+  diff            Diff task against its database-stored version.
+  review          Reviewer entry point for task diff.
+  done            Mark a task as done (single-arg form; Go supports variadic).
+  cancel          Cancel a task (single-arg form; Go supports variadic).
+  block           Mark a task as blocked and record the blocking relationship.
+  link            Create an entity link from a task to another entity.
+  reopen          Reopen a done or cancelled task with an audit-trail entry.
+  touches         Manage repo-touches links on a task.
```

#### 11. `workbench --help` — invocation `help`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 1870B, stderr 0B)
- Zig exit: `0` (stdout 1017B, stderr 0B)

```diff
--- go
+++ zig
@@ -1,36 +1,23 @@
-Manage the bidirectional sync surface between the workbench filesystem
-and the Planar database.
+workbench
 
-The workbench root defaults to ~/.planar/workbench/ and can be overridden
-with the PLANAR_WORKBENCH_ROOT environment variable.
+Manage the bidirectional sync surface between the workbench
+  filesystem and the Planar database.
 
-Usage:
-  planar workbench [command]
+  The workbench root defaults to ~/.planar/workbench/ and can be
+  overridden with the PLANAR_WORKBENCH_ROOT environment variable.
 
-Available Commands:
-  archive           Remove the FS tree; DB is retained.
-  edit              Open the plan's workbench directory in $EDITOR; sync on exit.
-  extract-questions Parse the Open questions section from each spec in the plan's workbench directory.
-  list              List active features with FS trees.
-  publish           Snapshot a feature's workbench tree to an external path.
-  pull              Apply FS→DB changes; report DB→FS drift.
-  push              Apply DB→FS changes atomically; report FS→DB drift.
-  resolve           Settle a sync conflict by choosing FS or DB.
-  restore           Recreate the FS tree from the DB.
-  status            Show drift and conflicts without writing.
-  sync              Full reconciliation: apply non-conflicting changes in both directions.
+USAGE:
+  workbench <command>
 
-Flags:
-  -h, --help   help for workbench
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar workbench [command] --help" for more information about a command.
+COMMANDS:
+  pull            Apply FS→DB changes; report DB→FS drift.
+  push            Apply DB→FS changes atomically; report FS→DB drift.
+  status          Show drift and conflicts without writing.
+  resolve         Settle a sync conflict by choosing FS or DB.
+  sync            Atomically apply FS and DB changes via a unified sync.
+  archive         Archive a feature's workbench filesystem tree.
+  restore         Restore an archived feature's workbench tree.
+  list            List features with workbench trees.
+  publish         Render and push workbench files to external system.
+  extract-questions  Parse Open questions from top-level workbench specs (read-only).
+  edit            Edit a feature's workbench files in $EDITOR.
```

#### 12. `workbench ` — invocation `no-args`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 1870B, stderr 0B)
- Zig exit: `0` (stdout 1017B, stderr 0B)

```diff
--- go
+++ zig
@@ -1,36 +1,23 @@
-Manage the bidirectional sync surface between the workbench filesystem
-and the Planar database.
+workbench
 
-The workbench root defaults to ~/.planar/workbench/ and can be overridden
-with the PLANAR_WORKBENCH_ROOT environment variable.
+Manage the bidirectional sync surface between the workbench
+  filesystem and the Planar database.
 
-Usage:
-  planar workbench [command]
+  The workbench root defaults to ~/.planar/workbench/ and can be
+  overridden with the PLANAR_WORKBENCH_ROOT environment variable.
 
-Available Commands:
-  archive           Remove the FS tree; DB is retained.
-  edit              Open the plan's workbench directory in $EDITOR; sync on exit.
-  extract-questions Parse the Open questions section from each spec in the plan's workbench directory.
-  list              List active features with FS trees.
-  publish           Snapshot a feature's workbench tree to an external path.
-  pull              Apply FS→DB changes; report DB→FS drift.
-  push              Apply DB→FS changes atomically; report FS→DB drift.
-  resolve           Settle a sync conflict by choosing FS or DB.
-  restore           Recreate the FS tree from the DB.
-  status            Show drift and conflicts without writing.
-  sync              Full reconciliation: apply non-conflicting changes in both directions.
+USAGE:
+  workbench <command>
 
-Flags:
-  -h, --help   help for workbench
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar workbench [command] --help" for more information about a command.
+COMMANDS:
+  pull            Apply FS→DB changes; report DB→FS drift.
+  push            Apply DB→FS changes atomically; report FS→DB drift.
+  status          Show drift and conflicts without writing.
+  resolve         Settle a sync conflict by choosing FS or DB.
+  sync            Atomically apply FS and DB changes via a unified sync.
+  archive         Archive a feature's workbench filesystem tree.
+  restore         Restore an archived feature's workbench tree.
+  list            List features with workbench trees.
+  publish         Render and push workbench files to external system.
+  extract-questions  Parse Open questions from top-level workbench specs (read-only).
+  edit            Edit a feature's workbench files in $EDITOR.
```

#### 13. `workbench ` — invocation `real-cwd`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 1870B, stderr 0B)
- Zig exit: `0` (stdout 1017B, stderr 0B)
- cwd: `<CWD_DIR>`

```diff
--- go
+++ zig
@@ -1,36 +1,23 @@
-Manage the bidirectional sync surface between the workbench filesystem
-and the Planar database.
+workbench
 
-The workbench root defaults to ~/.planar/workbench/ and can be overridden
-with the PLANAR_WORKBENCH_ROOT environment variable.
+Manage the bidirectional sync surface between the workbench
+  filesystem and the Planar database.
 
-Usage:
-  planar workbench [command]
+  The workbench root defaults to ~/.planar/workbench/ and can be
+  overridden with the PLANAR_WORKBENCH_ROOT environment variable.
 
-Available Commands:
-  archive           Remove the FS tree; DB is retained.
-  edit              Open the plan's workbench directory in $EDITOR; sync on exit.
-  extract-questions Parse the Open questions section from each spec in the plan's workbench directory.
-  list              List active features with FS trees.
-  publish           Snapshot a feature's workbench tree to an external path.
-  pull              Apply FS→DB changes; report DB→FS drift.
-  push              Apply DB→FS changes atomically; report FS→DB drift.
-  resolve           Settle a sync conflict by choosing FS or DB.
-  restore           Recreate the FS tree from the DB.
-  status            Show drift and conflicts without writing.
-  sync              Full reconciliation: apply non-conflicting changes in both directions.
+USAGE:
+  workbench <command>
 
-Flags:
-  -h, --help   help for workbench
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar workbench [command] --help" for more information about a command.
+COMMANDS:
+  pull            Apply FS→DB changes; report DB→FS drift.
+  push            Apply DB→FS changes atomically; report FS→DB drift.
+  status          Show drift and conflicts without writing.
+  resolve         Settle a sync conflict by choosing FS or DB.
+  sync            Atomically apply FS and DB changes via a unified sync.
+  archive         Archive a feature's workbench filesystem tree.
+  restore         Restore an archived feature's workbench tree.
+  list            List features with workbench trees.
+  publish         Render and push workbench files to external system.
+  extract-questions  Parse Open questions from top-level workbench specs (read-only).
+  edit            Edit a feature's workbench files in $EDITOR.
```

#### 14. `handoff --help` — invocation `help`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 2017B, stderr 0B)
- Zig exit: `0` (stdout 854B, stderr 0B)

```diff
--- go
+++ zig
@@ -1,45 +1,28 @@
+handoff
+
 Capture a context snapshot for the current session and atomically:
+    1. Insert a context_snapshots row.
+    2. Insert a handoffs row with status='pending'.
+    3. Validate the handoff (pending → validated, validated_at set).
 
-  1. Insert a context_snapshots row.
-  2. Insert a handoffs row with status='pending'.
-  3. Validate the handoff (pending → validated, validated_at set).
-  4. Append a session_entries row with prefix='note'.
+  Subcommands manage the handoff lifecycle: create / validate /
+  consume / abandon / list / show.
 
-Designed to be run before terminating an agent process.
+USAGE:
+  handoff [flags] <command> [task-id]
 
-Subcommands (escape hatches):
-  create    Create a handoff from an existing snapshot.
-  validate  Validate a pending handoff.
-  consume   Mark a handoff as consumed.
-  abandon   Abandon a non-terminal handoff.
-  list      List handoffs.
-  show      Show a handoff.
+COMMANDS:
+  create          Create a handoff from an existing snapshot.
+  validate        Validate a pending handoff.
+  consume         Mark a handoff as consumed.
+  abandon         Abandon a non-terminal handoff.
+  list            List handoffs.
+  show            Show a handoff's details.
 
-Usage:
-  planar handoff [<task-id>] [flags]
-  planar handoff [command]
+FLAGS:
+  --vendor              (string)
+  --note                (string)
+  --json                (bool) default=false
 
-Available Commands:
-  abandon     Abandon a non-terminal handoff.
-  consume     Mark a handoff as consumed (validated → consumed).
-  create      Create a handoff from an existing snapshot (escape hatch).
-  list        List handoffs by status.
-  show        Show a handoff.
-  validate    Validate the latest pending handoff anchored at <snapshot-id> (pending → validated).
-
-Flags:
-  -h, --help            help for handoff
-      --note string     Narrative note for the snapshot body. May be @<file>.
-      --vendor string   Expected destination vendor. Stored in handoffs.to_vendor.
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar handoff [command] --help" for more information about a command.
+POSITIONAL ARGUMENTS:
+  <task-id>       (string) optional
```

#### 15. `templates --help` — invocation `help`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 1924B, stderr 0B)
- Zig exit: `0` (stdout 820B, stderr 0B)

```diff
--- go
+++ zig
@@ -1,38 +1,22 @@
-Manage the template plane: list available templates, show their raw JSON,
-render them against a DB entity (dry run), validate syntax, initialise the
-default set on disk, and print resolution paths.
+templates
 
-Templates resolve via a three-level fallback chain:
-  1. <root>/<set>/<system>/<kind>.json          (user-chosen set on disk)
-  2. <root>/default/<system>/<kind>.json        (baseline set on disk)
-  3. embedded defaults in the binary            (always present)
+Manage the template plane: list available templates, show their
+  raw JSON, render them against a DB entity (dry run), validate
+  syntax, initialise the default set on disk, and print resolution
+  paths.
 
-The root defaults to ~/.planar/templates/ (Config.Templates.Dir).
-The default set is determined by [templates] default_set in config.toml,
-or overridden per-association via [associations."<slug>"] default_template_set.
+  Templates resolve via a three-level fallback chain:
+    1. ~/.planar/templates/<kind>/<slug>.json (operator overrides)
+    2. ~/.planar/templates/defaults/<kind>/<slug>.json (default copies)
+    3. templates/defaults/<kind>/<slug>.json (embedded in the binary)
 
-Usage:
-  planar templates [command]
+USAGE:
+  templates <command>
 
-Available Commands:
-  init        Extract embedded default templates to the templates root.
-  list        List available templates.
-  path        Print the resolved template path or the templates root directory.
-  render      Render a template against a DB entity (dry run).
-  show        Print the raw JSON of a resolved template.
-  validate    Validate a template file or all templates under the root.
-
-Flags:
-  -h, --help   help for templates
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar templates [command] --help" for more information about a command.
+COMMANDS:
+  list            List available templates.
+  show            Show a template's raw JSON.
+  render          Render a template against a database entity.
+  validate        Validate template syntax.
+  init            Extract default templates to disk.
+  path            Show template resolution paths.
```

#### 16. `templates ` — invocation `no-args`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 1924B, stderr 0B)
- Zig exit: `0` (stdout 820B, stderr 0B)

```diff
--- go
+++ zig
@@ -1,38 +1,22 @@
-Manage the template plane: list available templates, show their raw JSON,
-render them against a DB entity (dry run), validate syntax, initialise the
-default set on disk, and print resolution paths.
+templates
 
-Templates resolve via a three-level fallback chain:
-  1. <root>/<set>/<system>/<kind>.json          (user-chosen set on disk)
-  2. <root>/default/<system>/<kind>.json        (baseline set on disk)
-  3. embedded defaults in the binary            (always present)
+Manage the template plane: list available templates, show their
+  raw JSON, render them against a DB entity (dry run), validate
+  syntax, initialise the default set on disk, and print resolution
+  paths.
 
-The root defaults to ~/.planar/templates/ (Config.Templates.Dir).
-The default set is determined by [templates] default_set in config.toml,
-or overridden per-association via [associations."<slug>"] default_template_set.
+  Templates resolve via a three-level fallback chain:
+    1. ~/.planar/templates/<kind>/<slug>.json (operator overrides)
+    2. ~/.planar/templates/defaults/<kind>/<slug>.json (default copies)
+    3. templates/defaults/<kind>/<slug>.json (embedded in the binary)
 
-Usage:
-  planar templates [command]
+USAGE:
+  templates <command>
 
-Available Commands:
-  init        Extract embedded default templates to the templates root.
-  list        List available templates.
-  path        Print the resolved template path or the templates root directory.
-  render      Render a template against a DB entity (dry run).
-  show        Print the raw JSON of a resolved template.
-  validate    Validate a template file or all templates under the root.
-
-Flags:
-  -h, --help   help for templates
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar templates [command] --help" for more information about a command.
+COMMANDS:
+  list            List available templates.
+  show            Show a template's raw JSON.
+  render          Render a template against a database entity.
+  validate        Validate template syntax.
+  init            Extract default templates to disk.
+  path            Show template resolution paths.
```

#### 17. `templates ` — invocation `real-cwd`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 1924B, stderr 0B)
- Zig exit: `0` (stdout 820B, stderr 0B)
- cwd: `<CWD_DIR>`

```diff
--- go
+++ zig
@@ -1,38 +1,22 @@
-Manage the template plane: list available templates, show their raw JSON,
-render them against a DB entity (dry run), validate syntax, initialise the
-default set on disk, and print resolution paths.
+templates
 
-Templates resolve via a three-level fallback chain:
-  1. <root>/<set>/<system>/<kind>.json          (user-chosen set on disk)
-  2. <root>/default/<system>/<kind>.json        (baseline set on disk)
-  3. embedded defaults in the binary            (always present)
+Manage the template plane: list available templates, show their
+  raw JSON, render them against a DB entity (dry run), validate
+  syntax, initialise the default set on disk, and print resolution
+  paths.
 
-The root defaults to ~/.planar/templates/ (Config.Templates.Dir).
-The default set is determined by [templates] default_set in config.toml,
-or overridden per-association via [associations."<slug>"] default_template_set.
+  Templates resolve via a three-level fallback chain:
+    1. ~/.planar/templates/<kind>/<slug>.json (operator overrides)
+    2. ~/.planar/templates/defaults/<kind>/<slug>.json (default copies)
+    3. templates/defaults/<kind>/<slug>.json (embedded in the binary)
 
-Usage:
-  planar templates [command]
+USAGE:
+  templates <command>
 
-Available Commands:
-  init        Extract embedded default templates to the templates root.
-  list        List available templates.
-  path        Print the resolved template path or the templates root directory.
-  render      Render a template against a DB entity (dry run).
-  show        Print the raw JSON of a resolved template.
-  validate    Validate a template file or all templates under the root.
-
-Flags:
-  -h, --help   help for templates
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar templates [command] --help" for more information about a command.
+COMMANDS:
+  list            List available templates.
+  show            Show a template's raw JSON.
+  render          Render a template against a database entity.
+  validate        Validate template syntax.
+  init            Extract default templates to disk.
+  path            Show template resolution paths.
```

#### 18. `link --help` — invocation `help`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 1780B, stderr 0B)
- Zig exit: `0` (stdout 853B, stderr 0B)

```diff
--- go
+++ zig
@@ -1,30 +1,22 @@
-Manually record an external_links row linking a local entity to an already-existing
-external ticket. Use this when the external ticket was created outside of
-'ext create'. Does not push any data to the external system.
+link
 
-<kind:id> is a local entity reference, e.g. task:42, plan:7.
---to is the external reference, e.g. acme-jira:PROJ-1234.
+Manually record an external_links row linking a local entity to
+  an already-existing external ticket. Use this when the external
+  ticket was created outside of 'ext create'. Does not push any data
+  to the external system.
 
-Use --propagate to trigger ext-sync propagation of the feature anchor plan
-(the top-level plan that the linked entity belongs to) after the link is created.
+  <kind:id> is a local entity reference, e.g. task:42, plan:7.
 
-Usage:
-  planar link <kind:id> [flags]
+USAGE:
+  link [flags] <ref>
 
-Flags:
-  -h, --help           help for link
-      --propagate      After creating the link, propagate the feature anchor plan to its registered external system
-      --role string    Link role: mirror, parent, child, reference (default "reference")
-      --scope string   Scope for the cross-scope guard: global, repo, repo:<slug>, or assoc:<slug>. When omitted, scope is resolved from cwd or active stack.
-      --sync string    Sync direction: read-only, write-back, two-way (default "read-only")
-      --to string      <system-slug>:<external-id>  (required)
+FLAGS:
+  --to                  (string) required — <system-slug>:<external-id>
+  --role                (string) — Link role: mirror, parent, child, reference (default: reference)
+  --sync                (string) — Sync direction: read-only, write-back, two-way (default: read-only)
+  --propagate           (bool) default=false — Propagate feature after linking (M10)
+  --scope               (string)
+  --json                (bool) default=false
 
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
+POSITIONAL ARGUMENTS:
+  <ref>           (string) — Entity ref (kind:id)
```

#### 19. `local --help` — invocation `help`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 1877B, stderr 0B)
- Zig exit: `0` (stdout 671B, stderr 0B)

```diff
--- go
+++ zig
@@ -1,36 +1,18 @@
+local
+
 Manage the operator's local sandbox for personal skills and agents.
 
-Authors a single source file per skill or agent under ~/.planar/local/
-and creates per-vendor symlinks (with copy fallback) into each vendor's
-install directory. Edits to the source file propagate immediately to
-every vendor because the vendor paths are symlinks.
+  Authors a single source file per skill or agent under
+  ~/.planar/local/ and creates per-vendor symlinks (with copy
+  fallback) into each vendor's install directory.
+  Edits to the source file propagate immediately to every vendor.
 
-Promotion to the canonical Planar repo is intentionally manual — copy
-the file into the repo and follow the normal contribution flow.
+USAGE:
+  local <command>
 
-See docs/concepts.md § "Local sandbox" for the full design.
-
-Usage:
-  planar local [command]
-
-Available Commands:
-  import      Import skill or agent files from an external directory into the sandbox and link them.
-  link        Install per-vendor symlinks for one (or all) sandbox source files.
-  list        List every recorded sandbox install across all vendors. Marks broken links.
-  migrate     Convert legacy flat sandbox skills (~/.planar/local/skills/<name>.md) into the dir-shape layout (<name>/SKILL.md).
-  unlink      Remove per-vendor installs for a sandbox source. Optionally purge the source file.
-
-Flags:
-  -h, --help   help for local
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar local [command] --help" for more information about a command.
+COMMANDS:
+  list            List locally-installed skills and agents.
+  link            Create or reuse symlinks from vendor paths to local source.
+  unlink          Remove symlinks from vendor paths.
+  import          Import a skill or agent from an external directory.
+  migrate         Migrate skills/agents to new Planar version.
```

#### 20. `local ` — invocation `no-args`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 1877B, stderr 0B)
- Zig exit: `0` (stdout 671B, stderr 0B)

```diff
--- go
+++ zig
@@ -1,36 +1,18 @@
+local
+
 Manage the operator's local sandbox for personal skills and agents.
 
-Authors a single source file per skill or agent under ~/.planar/local/
-and creates per-vendor symlinks (with copy fallback) into each vendor's
-install directory. Edits to the source file propagate immediately to
-every vendor because the vendor paths are symlinks.
+  Authors a single source file per skill or agent under
+  ~/.planar/local/ and creates per-vendor symlinks (with copy
+  fallback) into each vendor's install directory.
+  Edits to the source file propagate immediately to every vendor.
 
-Promotion to the canonical Planar repo is intentionally manual — copy
-the file into the repo and follow the normal contribution flow.
+USAGE:
+  local <command>
 
-See docs/concepts.md § "Local sandbox" for the full design.
-
-Usage:
-  planar local [command]
-
-Available Commands:
-  import      Import skill or agent files from an external directory into the sandbox and link them.
-  link        Install per-vendor symlinks for one (or all) sandbox source files.
-  list        List every recorded sandbox install across all vendors. Marks broken links.
-  migrate     Convert legacy flat sandbox skills (~/.planar/local/skills/<name>.md) into the dir-shape layout (<name>/SKILL.md).
-  unlink      Remove per-vendor installs for a sandbox source. Optionally purge the source file.
-
-Flags:
-  -h, --help   help for local
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar local [command] --help" for more information about a command.
+COMMANDS:
+  list            List locally-installed skills and agents.
+  link            Create or reuse symlinks from vendor paths to local source.
+  unlink          Remove symlinks from vendor paths.
+  import          Import a skill or agent from an external directory.
+  migrate         Migrate skills/agents to new Planar version.
```

#### 21. `local ` — invocation `real-cwd`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 1877B, stderr 0B)
- Zig exit: `0` (stdout 671B, stderr 0B)
- cwd: `<CWD_DIR>`

```diff
--- go
+++ zig
@@ -1,36 +1,18 @@
+local
+
 Manage the operator's local sandbox for personal skills and agents.
 
-Authors a single source file per skill or agent under ~/.planar/local/
-and creates per-vendor symlinks (with copy fallback) into each vendor's
-install directory. Edits to the source file propagate immediately to
-every vendor because the vendor paths are symlinks.
+  Authors a single source file per skill or agent under
+  ~/.planar/local/ and creates per-vendor symlinks (with copy
+  fallback) into each vendor's install directory.
+  Edits to the source file propagate immediately to every vendor.
 
-Promotion to the canonical Planar repo is intentionally manual — copy
-the file into the repo and follow the normal contribution flow.
+USAGE:
+  local <command>
 
-See docs/concepts.md § "Local sandbox" for the full design.
-
-Usage:
-  planar local [command]
-
-Available Commands:
-  import      Import skill or agent files from an external directory into the sandbox and link them.
-  link        Install per-vendor symlinks for one (or all) sandbox source files.
-  list        List every recorded sandbox install across all vendors. Marks broken links.
-  migrate     Convert legacy flat sandbox skills (~/.planar/local/skills/<name>.md) into the dir-shape layout (<name>/SKILL.md).
-  unlink      Remove per-vendor installs for a sandbox source. Optionally purge the source file.
-
-Flags:
-  -h, --help   help for local
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar local [command] --help" for more information about a command.
+COMMANDS:
+  list            List locally-installed skills and agents.
+  link            Create or reuse symlinks from vendor paths to local source.
+  unlink          Remove symlinks from vendor paths.
+  import          Import a skill or agent from an external directory.
+  migrate         Migrate skills/agents to new Planar version.
```

#### 22. `scenario --help` — invocation `help`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 1638B, stderr 0B)
- Zig exit: `0` (stdout 822B, stderr 0B)

```diff
--- go
+++ zig
@@ -1,33 +1,23 @@
-Manage test scenarios — verification artifacts tied to specs, plans, or tasks.
+scenario
 
-Planar records scenarios and their outcomes; it does not execute them.
-Status lifecycle: draft → ready → verified / failing → retired.
+Manage test scenarios — verification artifacts tied to specs,
+  plans, or tasks.
 
-Usage:
-  planar scenario [command]
+  Planar records scenarios and their outcomes; it does not execute
+  them.
+  Status lifecycle: draft → ready → verified / failing → retired.
 
-Available Commands:
-  add         Create a new test scenario.
-  diff        Show a unified diff between the DB's scenario content and the workbench file.
-  edit        Edit a test scenario in $EDITOR (editor-first flow).
-  link        Create an entity link from a scenario to another entity.
-  list        List test scenarios.
-  retire      Mark a scenario as retired (no longer relevant).
-  show        Show a scenario's full details including last run outcome.
-  verify      Record the outcome of running a scenario.
-  view        View the scenario's workbench file in $PAGER.
+USAGE:
+  scenario <command>
 
-Flags:
-  -h, --help   help for scenario
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar scenario [command] --help" for more information about a command.
+COMMANDS:
+  add             Create a new test scenario.
+  edit            Edit a scenario in $EDITOR (editor-first flow).
+  view            View scenario's workbench file.
+  diff            Diff scenario against database version.
+  review          Reviewer entry point for scenario diff.
+  verify          Record a successful test run for a scenario.
+  retire          Mark a scenario as retired.
+  list            List scenarios.
+  show            Show a scenario's details.
+  link            Create an entity link from a scenario to another entity.
```

#### 23. `scenario ` — invocation `no-args`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 1638B, stderr 0B)
- Zig exit: `0` (stdout 822B, stderr 0B)

```diff
--- go
+++ zig
@@ -1,33 +1,23 @@
-Manage test scenarios — verification artifacts tied to specs, plans, or tasks.
+scenario
 
-Planar records scenarios and their outcomes; it does not execute them.
-Status lifecycle: draft → ready → verified / failing → retired.
+Manage test scenarios — verification artifacts tied to specs,
+  plans, or tasks.
 
-Usage:
-  planar scenario [command]
+  Planar records scenarios and their outcomes; it does not execute
+  them.
+  Status lifecycle: draft → ready → verified / failing → retired.
 
-Available Commands:
-  add         Create a new test scenario.
-  diff        Show a unified diff between the DB's scenario content and the workbench file.
-  edit        Edit a test scenario in $EDITOR (editor-first flow).
-  link        Create an entity link from a scenario to another entity.
-  list        List test scenarios.
-  retire      Mark a scenario as retired (no longer relevant).
-  show        Show a scenario's full details including last run outcome.
-  verify      Record the outcome of running a scenario.
-  view        View the scenario's workbench file in $PAGER.
+USAGE:
+  scenario <command>
 
-Flags:
-  -h, --help   help for scenario
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar scenario [command] --help" for more information about a command.
+COMMANDS:
+  add             Create a new test scenario.
+  edit            Edit a scenario in $EDITOR (editor-first flow).
+  view            View scenario's workbench file.
+  diff            Diff scenario against database version.
+  review          Reviewer entry point for scenario diff.
+  verify          Record a successful test run for a scenario.
+  retire          Mark a scenario as retired.
+  list            List scenarios.
+  show            Show a scenario's details.
+  link            Create an entity link from a scenario to another entity.
```

#### 24. `scenario ` — invocation `real-cwd`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 1638B, stderr 0B)
- Zig exit: `0` (stdout 822B, stderr 0B)
- cwd: `<CWD_DIR>`

```diff
--- go
+++ zig
@@ -1,33 +1,23 @@
-Manage test scenarios — verification artifacts tied to specs, plans, or tasks.
+scenario
 
-Planar records scenarios and their outcomes; it does not execute them.
-Status lifecycle: draft → ready → verified / failing → retired.
+Manage test scenarios — verification artifacts tied to specs,
+  plans, or tasks.
 
-Usage:
-  planar scenario [command]
+  Planar records scenarios and their outcomes; it does not execute
+  them.
+  Status lifecycle: draft → ready → verified / failing → retired.
 
-Available Commands:
-  add         Create a new test scenario.
-  diff        Show a unified diff between the DB's scenario content and the workbench file.
-  edit        Edit a test scenario in $EDITOR (editor-first flow).
-  link        Create an entity link from a scenario to another entity.
-  list        List test scenarios.
-  retire      Mark a scenario as retired (no longer relevant).
-  show        Show a scenario's full details including last run outcome.
-  verify      Record the outcome of running a scenario.
-  view        View the scenario's workbench file in $PAGER.
+USAGE:
+  scenario <command>
 
-Flags:
-  -h, --help   help for scenario
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar scenario [command] --help" for more information about a command.
+COMMANDS:
+  add             Create a new test scenario.
+  edit            Edit a scenario in $EDITOR (editor-first flow).
+  view            View scenario's workbench file.
+  diff            Diff scenario against database version.
+  review          Reviewer entry point for scenario diff.
+  verify          Record a successful test run for a scenario.
+  retire          Mark a scenario as retired.
+  list            List scenarios.
+  show            Show a scenario's details.
+  link            Create an entity link from a scenario to another entity.
```

#### 25. `search --help` — invocation `help`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 1704B, stderr 0B)
- Zig exit: `0` (stdout 787B, stderr 0B)

```diff
--- go
+++ zig
@@ -1,29 +1,22 @@
+search
+
 Run a full-text search across every searchable entity kind.
 
-Queries are passed to SQLite's FTS5 MATCH operator directly. Multi-word
-queries are AND'd unless the operator is given explicitly (OR, NOT, NEAR,
-"phrase"). Tokens are unicode61-folded (case-insensitive, diacritic-stripped).
+  Queries are passed to SQLite's FTS5 MATCH operator directly.
+  Multi-word queries are AND'd unless the operator is given
+  explicitly (OR, NOT, NEAR, "phrase"). Tokens are unicode61-folded
+  (case-insensitive, diacritic-stripped).
 
-Filters (--kind, --status, --scope, --plan) compose as AND; --limit caps results.
-Results are ordered by bm25 relevance (most relevant first).
+USAGE:
+  search [flags] <query>
 
-Usage:
-  planar search "<query>" [flags]
+FLAGS:
+  --kind                (string) — Restrict to one kind (repeatable in Go)
+  --status              (string) — Restrict by status (repeatable in Go)
+  --scope               (string) — Restrict to a scope slug
+  --plan                (int) — Restrict to a plan id
+  --limit               (int) default=50 — Max results
+  --json                (bool) default=false
 
-Flags:
-  -h, --help                 help for search
-      --json                 Emit machine-readable JSON, one hit per line.
-      --kind stringArray     Restrict to these entity kinds. Repeatable. Valid: plan, task, question, scenario, decision, artifact.
-      --limit int            Maximum number of hits to return (default 50). (default 50)
-      --plan int             Restrict to entities associated with this plan id.
-      --scope string         Restrict to this scope (global, repo, repo:<slug>, assoc:<slug>). Default: cwd-derived scope.
-      --status stringArray   Restrict to these status values. Repeatable.
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
+POSITIONAL ARGUMENTS:
+  <query>         (string) — FTS5 query string
```

#### 26. `artifact --help` — invocation `help`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 1596B, stderr 0B)
- Zig exit: `0` (stdout 888B, stderr 0B)

```diff
--- go
+++ zig
@@ -1,32 +1,22 @@
+artifact
+
 Manage artifacts — durable documents that crystallize from work.
 
-Kinds: tech_spec, adr, design_note, summary, readme, generated, other, product_spec, roadmap, research, getting_started, changelog_entry, glossary_term.
-Status lifecycle: draft → active → superseded/retired.
+  Kinds: tech_spec, adr, design_note, summary, readme, generated,
+  other, product_spec, roadmap, research, getting_started,
+  changelog_entry, glossary_term.
+  Status lifecycle: draft → active → superseded/retired.
 
-Usage:
-  planar artifact [command]
+USAGE:
+  artifact <command>
 
-Available Commands:
-  add         Register a new artifact.
-  diff        Show a unified diff between the DB's artifact content and the workbench file.
-  edit        Edit an artifact in $EDITOR (editor-first flow).
-  link        Create an entity link from an artifact to another entity.
-  list        List artifacts.
-  show        Show an artifact's metadata and body.
-  update      Update mutable fields on an artifact.
-  view        View the artifact's workbench file in $PAGER.
-
-Flags:
-  -h, --help   help for artifact
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar artifact [command] --help" for more information about a command.
+COMMANDS:
+  add             Register a new artifact.
+  show            Show an artifact's metadata and body.
+  list            List artifacts.
+  update          Update mutable fields on an artifact.
+  edit            Edit an artifact in $EDITOR (editor-first flow).
+  view            View the artifact's workbench file in $PAGER.
+  diff            Show a unified diff between the DB's artifact content and the workbench file.
+  review          Reviewer entry point for artifact diff.
+  link            Create an entity link from an artifact to another entity.
```

#### 27. `artifact ` — invocation `no-args`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 1596B, stderr 0B)
- Zig exit: `0` (stdout 888B, stderr 0B)

```diff
--- go
+++ zig
@@ -1,32 +1,22 @@
+artifact
+
 Manage artifacts — durable documents that crystallize from work.
 
-Kinds: tech_spec, adr, design_note, summary, readme, generated, other, product_spec, roadmap, research, getting_started, changelog_entry, glossary_term.
-Status lifecycle: draft → active → superseded/retired.
+  Kinds: tech_spec, adr, design_note, summary, readme, generated,
+  other, product_spec, roadmap, research, getting_started,
+  changelog_entry, glossary_term.
+  Status lifecycle: draft → active → superseded/retired.
 
-Usage:
-  planar artifact [command]
+USAGE:
+  artifact <command>
 
-Available Commands:
-  add         Register a new artifact.
-  diff        Show a unified diff between the DB's artifact content and the workbench file.
-  edit        Edit an artifact in $EDITOR (editor-first flow).
-  link        Create an entity link from an artifact to another entity.
-  list        List artifacts.
-  show        Show an artifact's metadata and body.
-  update      Update mutable fields on an artifact.
-  view        View the artifact's workbench file in $PAGER.
-
-Flags:
-  -h, --help   help for artifact
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar artifact [command] --help" for more information about a command.
+COMMANDS:
+  add             Register a new artifact.
+  show            Show an artifact's metadata and body.
+  list            List artifacts.
+  update          Update mutable fields on an artifact.
+  edit            Edit an artifact in $EDITOR (editor-first flow).
+  view            View the artifact's workbench file in $PAGER.
+  diff            Show a unified diff between the DB's artifact content and the workbench file.
+  review          Reviewer entry point for artifact diff.
+  link            Create an entity link from an artifact to another entity.
```

#### 28. `artifact ` — invocation `real-cwd`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 1596B, stderr 0B)
- Zig exit: `0` (stdout 888B, stderr 0B)
- cwd: `<CWD_DIR>`

```diff
--- go
+++ zig
@@ -1,32 +1,22 @@
+artifact
+
 Manage artifacts — durable documents that crystallize from work.
 
-Kinds: tech_spec, adr, design_note, summary, readme, generated, other, product_spec, roadmap, research, getting_started, changelog_entry, glossary_term.
-Status lifecycle: draft → active → superseded/retired.
+  Kinds: tech_spec, adr, design_note, summary, readme, generated,
+  other, product_spec, roadmap, research, getting_started,
+  changelog_entry, glossary_term.
+  Status lifecycle: draft → active → superseded/retired.
 
-Usage:
-  planar artifact [command]
+USAGE:
+  artifact <command>
 
-Available Commands:
-  add         Register a new artifact.
-  diff        Show a unified diff between the DB's artifact content and the workbench file.
-  edit        Edit an artifact in $EDITOR (editor-first flow).
-  link        Create an entity link from an artifact to another entity.
-  list        List artifacts.
-  show        Show an artifact's metadata and body.
-  update      Update mutable fields on an artifact.
-  view        View the artifact's workbench file in $PAGER.
-
-Flags:
-  -h, --help   help for artifact
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar artifact [command] --help" for more information about a command.
+COMMANDS:
+  add             Register a new artifact.
+  show            Show an artifact's metadata and body.
+  list            List artifacts.
+  update          Update mutable fields on an artifact.
+  edit            Edit an artifact in $EDITOR (editor-first flow).
+  view            View the artifact's workbench file in $PAGER.
+  diff            Show a unified diff between the DB's artifact content and the workbench file.
+  review          Reviewer entry point for artifact diff.
+  link            Create an entity link from an artifact to another entity.
```

#### 29. `doc --help` — invocation `help`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 1644B, stderr 0B)
- Zig exit: `0` (stdout 767B, stderr 0B)

```diff
--- go
+++ zig
@@ -1,33 +1,20 @@
+doc
+
 Outward-facing documentation tooling.
 
-The lint subcommand validates citations and reference declarations in
-markdown docs. The manifest subcommands track per-doc content + source
-hashes in .manifest-docs so drift between published docs and the
-artifacts they were synthesized from can be detected in O(1).
+  The lint subcommand validates citations and reference declarations
+  in markdown docs. The manifest subcommands track per-doc content +
+  source hashes in .manifest-docs so drift between published docs and
+  the underlying source artifacts is detectable.
 
-Usage:
-  planar doc [command]
+USAGE:
+  doc <command>
 
-Available Commands:
-  backlinks   List every published doc whose sources cite the given entity.
-  coverage    Report plans with status=done that no published doc cites.
-  lint        Lint citations and references across docs.
-  manifest    Manage .manifest-docs (build, verify, diff, info).
-  orphans     List artifacts of the given kind with zero backlinks in .manifest-docs.
-  promote     Promote internal Planar entities into a published doc.
-  regenerate  Re-synthesise existing docs from their current sources.
-
-Flags:
-  -h, --help   help for doc
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar doc [command] --help" for more information about a command.
+COMMANDS:
+  lint            Lint citations and references across docs.
+  manifest        Manage documentation manifest.
+  promote         Promote internal Planar entities into published docs.
+  regenerate      Re-synthesise existing docs from their current sources.
+  backlinks       Report which docs link to a given entity.
+  orphans         Find docs not referenced by any entity.
+  coverage        Report documentation coverage metrics.
```

#### 30. `doc ` — invocation `no-args`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 1644B, stderr 0B)
- Zig exit: `0` (stdout 767B, stderr 0B)

```diff
--- go
+++ zig
@@ -1,33 +1,20 @@
+doc
+
 Outward-facing documentation tooling.
 
-The lint subcommand validates citations and reference declarations in
-markdown docs. The manifest subcommands track per-doc content + source
-hashes in .manifest-docs so drift between published docs and the
-artifacts they were synthesized from can be detected in O(1).
+  The lint subcommand validates citations and reference declarations
+  in markdown docs. The manifest subcommands track per-doc content +
+  source hashes in .manifest-docs so drift between published docs and
+  the underlying source artifacts is detectable.
 
-Usage:
-  planar doc [command]
+USAGE:
+  doc <command>
 
-Available Commands:
-  backlinks   List every published doc whose sources cite the given entity.
-  coverage    Report plans with status=done that no published doc cites.
-  lint        Lint citations and references across docs.
-  manifest    Manage .manifest-docs (build, verify, diff, info).
-  orphans     List artifacts of the given kind with zero backlinks in .manifest-docs.
-  promote     Promote internal Planar entities into a published doc.
-  regenerate  Re-synthesise existing docs from their current sources.
-
-Flags:
-  -h, --help   help for doc
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar doc [command] --help" for more information about a command.
+COMMANDS:
+  lint            Lint citations and references across docs.
+  manifest        Manage documentation manifest.
+  promote         Promote internal Planar entities into published docs.
+  regenerate      Re-synthesise existing docs from their current sources.
+  backlinks       Report which docs link to a given entity.
+  orphans         Find docs not referenced by any entity.
+  coverage        Report documentation coverage metrics.
```

#### 31. `doc ` — invocation `real-cwd`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 1644B, stderr 0B)
- Zig exit: `0` (stdout 767B, stderr 0B)
- cwd: `<CWD_DIR>`

```diff
--- go
+++ zig
@@ -1,33 +1,20 @@
+doc
+
 Outward-facing documentation tooling.
 
-The lint subcommand validates citations and reference declarations in
-markdown docs. The manifest subcommands track per-doc content + source
-hashes in .manifest-docs so drift between published docs and the
-artifacts they were synthesized from can be detected in O(1).
+  The lint subcommand validates citations and reference declarations
+  in markdown docs. The manifest subcommands track per-doc content +
+  source hashes in .manifest-docs so drift between published docs and
+  the underlying source artifacts is detectable.
 
-Usage:
-  planar doc [command]
+USAGE:
+  doc <command>
 
-Available Commands:
-  backlinks   List every published doc whose sources cite the given entity.
-  coverage    Report plans with status=done that no published doc cites.
-  lint        Lint citations and references across docs.
-  manifest    Manage .manifest-docs (build, verify, diff, info).
-  orphans     List artifacts of the given kind with zero backlinks in .manifest-docs.
-  promote     Promote internal Planar entities into a published doc.
-  regenerate  Re-synthesise existing docs from their current sources.
-
-Flags:
-  -h, --help   help for doc
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar doc [command] --help" for more information about a command.
+COMMANDS:
+  lint            Lint citations and references across docs.
+  manifest        Manage documentation manifest.
+  promote         Promote internal Planar entities into published docs.
+  regenerate      Re-synthesise existing docs from their current sources.
+  backlinks       Report which docs link to a given entity.
+  orphans         Find docs not referenced by any entity.
+  coverage        Report documentation coverage metrics.
```

#### 32. `decision --help` — invocation `help`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 1617B, stderr 0B)
- Zig exit: `0` (stdout 819B, stderr 0B)

```diff
--- go
+++ zig
@@ -1,34 +1,22 @@
+decision
+
 Manage decision records — rationale for choices made during work.
 
-Status lifecycle: proposed → accepted / superseded / withdrawn.
-Terminal statuses: superseded, withdrawn.
+  Status lifecycle: proposed → accepted / superseded / withdrawn.
+  Terminal statuses: superseded, withdrawn.
 
-Usage:
-  planar decision [command]
+USAGE:
+  decision <command>
 
-Available Commands:
-  accept      Mark a decision as accepted.
-  add         Record a new decision.
-  diff        Show a unified diff between the DB's decision content and the workbench file.
-  edit        Edit a decision in $EDITOR (editor-first flow).
-  link        Create an entity link from a decision to another entity.
-  list        List decisions.
-  show        Show a decision with body, rationale, status, and linked session.
-  supersede   Mark a decision as superseded by a newer decision.
-  view        View the decision's workbench file in $PAGER.
-  withdraw    Mark a decision as withdrawn.
-
-Flags:
-  -h, --help   help for decision
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar decision [command] --help" for more information about a command.
+COMMANDS:
+  add             Create a new decision record.
+  show            Show a decision's details.
+  list            List decisions.
+  accept          Accept a proposed decision.
+  supersede       Mark a decision as superseded by a newer decision.
+  withdraw        Withdraw a decision.
+  edit            Edit a decision in $EDITOR (editor-first flow).
+  view            View decision's workbench file.
+  diff            Diff decision against database version.
+  review          Reviewer entry point for decision diff.
+  link            Create an entity link from a decision to another entity.
```

#### 33. `decision ` — invocation `no-args`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 1617B, stderr 0B)
- Zig exit: `0` (stdout 819B, stderr 0B)

```diff
--- go
+++ zig
@@ -1,34 +1,22 @@
+decision
+
 Manage decision records — rationale for choices made during work.
 
-Status lifecycle: proposed → accepted / superseded / withdrawn.
-Terminal statuses: superseded, withdrawn.
+  Status lifecycle: proposed → accepted / superseded / withdrawn.
+  Terminal statuses: superseded, withdrawn.
 
-Usage:
-  planar decision [command]
+USAGE:
+  decision <command>
 
-Available Commands:
-  accept      Mark a decision as accepted.
-  add         Record a new decision.
-  diff        Show a unified diff between the DB's decision content and the workbench file.
-  edit        Edit a decision in $EDITOR (editor-first flow).
-  link        Create an entity link from a decision to another entity.
-  list        List decisions.
-  show        Show a decision with body, rationale, status, and linked session.
-  supersede   Mark a decision as superseded by a newer decision.
-  view        View the decision's workbench file in $PAGER.
-  withdraw    Mark a decision as withdrawn.
-
-Flags:
-  -h, --help   help for decision
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar decision [command] --help" for more information about a command.
+COMMANDS:
+  add             Create a new decision record.
+  show            Show a decision's details.
+  list            List decisions.
+  accept          Accept a proposed decision.
+  supersede       Mark a decision as superseded by a newer decision.
+  withdraw        Withdraw a decision.
+  edit            Edit a decision in $EDITOR (editor-first flow).
+  view            View decision's workbench file.
+  diff            Diff decision against database version.
+  review          Reviewer entry point for decision diff.
+  link            Create an entity link from a decision to another entity.
```

#### 34. `decision ` — invocation `real-cwd`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 1617B, stderr 0B)
- Zig exit: `0` (stdout 819B, stderr 0B)
- cwd: `<CWD_DIR>`

```diff
--- go
+++ zig
@@ -1,34 +1,22 @@
+decision
+
 Manage decision records — rationale for choices made during work.
 
-Status lifecycle: proposed → accepted / superseded / withdrawn.
-Terminal statuses: superseded, withdrawn.
+  Status lifecycle: proposed → accepted / superseded / withdrawn.
+  Terminal statuses: superseded, withdrawn.
 
-Usage:
-  planar decision [command]
+USAGE:
+  decision <command>
 
-Available Commands:
-  accept      Mark a decision as accepted.
-  add         Record a new decision.
-  diff        Show a unified diff between the DB's decision content and the workbench file.
-  edit        Edit a decision in $EDITOR (editor-first flow).
-  link        Create an entity link from a decision to another entity.
-  list        List decisions.
-  show        Show a decision with body, rationale, status, and linked session.
-  supersede   Mark a decision as superseded by a newer decision.
-  view        View the decision's workbench file in $PAGER.
-  withdraw    Mark a decision as withdrawn.
-
-Flags:
-  -h, --help   help for decision
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar decision [command] --help" for more information about a command.
+COMMANDS:
+  add             Create a new decision record.
+  show            Show a decision's details.
+  list            List decisions.
+  accept          Accept a proposed decision.
+  supersede       Mark a decision as superseded by a newer decision.
+  withdraw        Withdraw a decision.
+  edit            Edit a decision in $EDITOR (editor-first flow).
+  view            View decision's workbench file.
+  diff            Diff decision against database version.
+  review          Reviewer entry point for decision diff.
+  link            Create an entity link from a decision to another entity.
```

#### 35. `resume --help` — invocation `help`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 1564B, stderr 0B)
- Zig exit: `0` (stdout 765B, stderr 0B)

```diff
--- go
+++ zig
@@ -1,35 +1,26 @@
-Produce a structured 8-section resume packet for the specified task.
+resume
 
-The packet contains:
-  1. Identity       — task id, plan id, title, scope
-  2. State          — status, next_action, last action
-  3. Plan position  — parent plan, completed/current/remaining steps
-  4. Operational    — external_links for the task; refreshed from remote if stale
-  5. Recent activity — session entries from recent sessions
-  6. Decisions and questions
-  7. Linked artifacts
-  8. Audit footer   — previous session vendor and timestamp
+Produce a structured 8-section resume packet for the specified
+  task.
 
-Use 'resume validate <task-id>' to check resumability before producing the packet.
+  The packet contains:
+    1. Identity       — task id, plan id, title, scope
+    2. State          — status, next_action, last action
+    3. Plan position  — parent plan, completed/current/remaining steps
+    4. Operational    — external_links for the task; refreshed if stale
+    5. Recent activity — session entries from recent sessions
+    6. Decisions and questions
+    7. Linked artifacts
+    8. Audit footer   — previous session vendor and timestamp
 
-Usage:
-  planar resume [<task-id>] [flags]
-  planar resume [command]
+USAGE:
+  resume [flags] <command> [task-id]
 
-Available Commands:
-  validate    Check whether the specified task is resumable.
+COMMANDS:
+  validate        Check if a task is resumable.
 
-Flags:
-  -h, --help   help for resume
+FLAGS:
+  --json                (bool) default=false
 
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar resume [command] --help" for more information about a command.
+POSITIONAL ARGUMENTS:
+  <task-id>       (string) optional
```

#### 36. `links --help` — invocation `help`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 1611B, stderr 0B)
- Zig exit: `0` (stdout 718B, stderr 0B)

```diff
--- go
+++ zig
@@ -1,32 +1,18 @@
-Manage internal cross-cutting entity relationships stored in entity_links.
+links
 
-Entity links record typed relationships between any two Planar entities
-(e.g. a task cites an artifact, a plan blocks another plan). This domain is
-distinct from the top-level link/unlink commands, which operate on
-external_links (operational plane bindings to Jira, GitHub Issues, etc.).
-Exception: the "update" subcommand mutates the sync_direction column on an
-existing external_links row (it does not touch entity_links).
+Manage internal cross-cutting entity_links relationships.
 
-Usage:
-  planar links [command]
+  Entity links record typed relationships between any two Planar
+  entities (e.g. a task cites an artifact, a plan blocks another
+  plan). This domain is distinct from the top-level link/unlink
+  commands, which operate on external-system ticket linkage.
 
-Available Commands:
-  add         Create an entity_links row between two entities.
-  list        List entity_links where the given entity is source or target.
-  remove      Delete an entity_links row by its id.
-  update      Change sync_direction on an existing external_links row.
+USAGE:
+  links <command>
 
-Flags:
-  -h, --help   help for links
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar links [command] --help" for more information about a command.
+COMMANDS:
+  add             Create an entity_links row between two entities.
+  list            List entity_links where the given entity is source or target.
+  remove          Delete an entity_links row by its id.
+  trail           Show the audit trail for an entity_links row.
+  update          Change sync_direction on an existing external_links row (deferred to M11).
```

#### 37. `links ` — invocation `no-args`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 1611B, stderr 0B)
- Zig exit: `0` (stdout 718B, stderr 0B)

```diff
--- go
+++ zig
@@ -1,32 +1,18 @@
-Manage internal cross-cutting entity relationships stored in entity_links.
+links
 
-Entity links record typed relationships between any two Planar entities
-(e.g. a task cites an artifact, a plan blocks another plan). This domain is
-distinct from the top-level link/unlink commands, which operate on
-external_links (operational plane bindings to Jira, GitHub Issues, etc.).
-Exception: the "update" subcommand mutates the sync_direction column on an
-existing external_links row (it does not touch entity_links).
+Manage internal cross-cutting entity_links relationships.
 
-Usage:
-  planar links [command]
+  Entity links record typed relationships between any two Planar
+  entities (e.g. a task cites an artifact, a plan blocks another
+  plan). This domain is distinct from the top-level link/unlink
+  commands, which operate on external-system ticket linkage.
 
-Available Commands:
-  add         Create an entity_links row between two entities.
-  list        List entity_links where the given entity is source or target.
-  remove      Delete an entity_links row by its id.
-  update      Change sync_direction on an existing external_links row.
+USAGE:
+  links <command>
 
-Flags:
-  -h, --help   help for links
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar links [command] --help" for more information about a command.
+COMMANDS:
+  add             Create an entity_links row between two entities.
+  list            List entity_links where the given entity is source or target.
+  remove          Delete an entity_links row by its id.
+  trail           Show the audit trail for an entity_links row.
+  update          Change sync_direction on an existing external_links row (deferred to M11).
```

#### 38. `links ` — invocation `real-cwd`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 1611B, stderr 0B)
- Zig exit: `0` (stdout 718B, stderr 0B)
- cwd: `<CWD_DIR>`

```diff
--- go
+++ zig
@@ -1,32 +1,18 @@
-Manage internal cross-cutting entity relationships stored in entity_links.
+links
 
-Entity links record typed relationships between any two Planar entities
-(e.g. a task cites an artifact, a plan blocks another plan). This domain is
-distinct from the top-level link/unlink commands, which operate on
-external_links (operational plane bindings to Jira, GitHub Issues, etc.).
-Exception: the "update" subcommand mutates the sync_direction column on an
-existing external_links row (it does not touch entity_links).
+Manage internal cross-cutting entity_links relationships.
 
-Usage:
-  planar links [command]
+  Entity links record typed relationships between any two Planar
+  entities (e.g. a task cites an artifact, a plan blocks another
+  plan). This domain is distinct from the top-level link/unlink
+  commands, which operate on external-system ticket linkage.
 
-Available Commands:
-  add         Create an entity_links row between two entities.
-  list        List entity_links where the given entity is source or target.
-  remove      Delete an entity_links row by its id.
-  update      Change sync_direction on an existing external_links row.
+USAGE:
+  links <command>
 
-Flags:
-  -h, --help   help for links
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar links [command] --help" for more information about a command.
+COMMANDS:
+  add             Create an entity_links row between two entities.
+  list            List entity_links where the given entity is source or target.
+  remove          Delete an entity_links row by its id.
+  trail           Show the audit trail for an entity_links row.
+  update          Change sync_direction on an existing external_links row (deferred to M11).
```

#### 39. `plan --help` — invocation `help`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 1454B, stderr 0B)
- Zig exit: `0` (stdout 922B, stderr 0B)

```diff
--- go
+++ zig
@@ -1,31 +1,24 @@
+plan
+
 Manage plans — the top-level structured intent for a body of work.
 
-Plans may be hierarchical (--parent) and contain ordered steps (plan step add).
-Status lifecycle: draft → active → paused / done / abandoned.
+  Plans may be hierarchical (--parent) and contain ordered steps
+  (plan step add).
+  Status lifecycle: draft → active → paused / done / abandoned.
 
-Usage:
-  planar plan [command]
+USAGE:
+  plan <command>
 
-Available Commands:
-  create           Create a new plan.
-  link             Create an entity link from a plan to another entity.
-  list             List plans.
-  recompute-status Re-fire the plan-status auto-promotion invariant against a plan or all plans.
-  show             Show a plan's details, steps, and child plans.
-  step             Manage plan steps.
-  update           Update mutable fields on a plan.
-
-Flags:
-  -h, --help   help for plan
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar plan [command] --help" for more information about a command.
+COMMANDS:
+  create          Create a new plan.
+  show            Show a plan's details, steps, and child plans.
+  list            List plans.
+  update          Update mutable fields on a plan.
+  edit            Edit a plan in $EDITOR (editor-first flow).
+  view            View a plan's workbench file.
+  diff            Diff plan against database version.
+  review          Reviewer entry point for plan diff.
+  link            Create an entity link from a plan to another entity.
+  next            Return the highest-priority eligible task on a plan.
+  recompute-status  Recompute a plan's roll-up status (--plan <id> or --all).
+  step            Manage plan steps.
```

#### 40. `plan ` — invocation `no-args`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 1454B, stderr 0B)
- Zig exit: `0` (stdout 922B, stderr 0B)

```diff
--- go
+++ zig
@@ -1,31 +1,24 @@
+plan
+
 Manage plans — the top-level structured intent for a body of work.
 
-Plans may be hierarchical (--parent) and contain ordered steps (plan step add).
-Status lifecycle: draft → active → paused / done / abandoned.
+  Plans may be hierarchical (--parent) and contain ordered steps
+  (plan step add).
+  Status lifecycle: draft → active → paused / done / abandoned.
 
-Usage:
-  planar plan [command]
+USAGE:
+  plan <command>
 
-Available Commands:
-  create           Create a new plan.
-  link             Create an entity link from a plan to another entity.
-  list             List plans.
-  recompute-status Re-fire the plan-status auto-promotion invariant against a plan or all plans.
-  show             Show a plan's details, steps, and child plans.
-  step             Manage plan steps.
-  update           Update mutable fields on a plan.
-
-Flags:
-  -h, --help   help for plan
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar plan [command] --help" for more information about a command.
+COMMANDS:
+  create          Create a new plan.
+  show            Show a plan's details, steps, and child plans.
+  list            List plans.
+  update          Update mutable fields on a plan.
+  edit            Edit a plan in $EDITOR (editor-first flow).
+  view            View a plan's workbench file.
+  diff            Diff plan against database version.
+  review          Reviewer entry point for plan diff.
+  link            Create an entity link from a plan to another entity.
+  next            Return the highest-priority eligible task on a plan.
+  recompute-status  Recompute a plan's roll-up status (--plan <id> or --all).
+  step            Manage plan steps.
```

#### 41. `plan ` — invocation `real-cwd`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 1454B, stderr 0B)
- Zig exit: `0` (stdout 922B, stderr 0B)
- cwd: `<CWD_DIR>`

```diff
--- go
+++ zig
@@ -1,31 +1,24 @@
+plan
+
 Manage plans — the top-level structured intent for a body of work.
 
-Plans may be hierarchical (--parent) and contain ordered steps (plan step add).
-Status lifecycle: draft → active → paused / done / abandoned.
+  Plans may be hierarchical (--parent) and contain ordered steps
+  (plan step add).
+  Status lifecycle: draft → active → paused / done / abandoned.
 
-Usage:
-  planar plan [command]
+USAGE:
+  plan <command>
 
-Available Commands:
-  create           Create a new plan.
-  link             Create an entity link from a plan to another entity.
-  list             List plans.
-  recompute-status Re-fire the plan-status auto-promotion invariant against a plan or all plans.
-  show             Show a plan's details, steps, and child plans.
-  step             Manage plan steps.
-  update           Update mutable fields on a plan.
-
-Flags:
-  -h, --help   help for plan
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar plan [command] --help" for more information about a command.
+COMMANDS:
+  create          Create a new plan.
+  show            Show a plan's details, steps, and child plans.
+  list            List plans.
+  update          Update mutable fields on a plan.
+  edit            Edit a plan in $EDITOR (editor-first flow).
+  view            View a plan's workbench file.
+  diff            Diff plan against database version.
+  review          Reviewer entry point for plan diff.
+  link            Create an entity link from a plan to another entity.
+  next            Return the highest-priority eligible task on a plan.
+  recompute-status  Recompute a plan's roll-up status (--plan <id> or --all).
+  step            Manage plan steps.
```

#### 42. `question --help` — invocation `help`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 1608B, stderr 0B)
- Zig exit: `0` (stdout 709B, stderr 0B)

```diff
--- go
+++ zig
@@ -1,33 +1,20 @@
+question
+
 Manage questions — open uncertainties surfaced during work.
 
-Status lifecycle: open → answered (via 'question answer') / wontfix.
+  Status lifecycle: open → answered (via 'question answer') / wontfix.
 
-Usage:
-  planar question [command]
+USAGE:
+  question <command>
 
-Available Commands:
-  add         Record an open question.
-  answer      Provide an answer to an open question.
-  diff        Show a unified diff between the DB's question content and the workbench file.
-  edit        Edit a question in $EDITOR (editor-first flow).
-  link        Create an entity link from a question to another entity.
-  list        List questions.
-  review      Bulk-review open questions in $EDITOR with originating context.
-  show        Show a question with its body and (if answered) the answer.
-  view        View the question's workbench file in $PAGER.
-  wontfix     Mark a question as not going to be answered.
-
-Flags:
-  -h, --help   help for question
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar question [command] --help" for more information about a command.
+COMMANDS:
+  add             Create a new question.
+  edit            Edit a question in $EDITOR (editor-first flow).
+  view            View question's workbench file.
+  diff            Diff question against database version.
+  review          Reviewer entry point for question diff.
+  answer          Record an answer to a question.
+  wontfix         Mark a question as wontfix.
+  list            List questions.
+  show            Show a question's details.
+  link            Create an entity link from a question to another entity.
```

#### 43. `question ` — invocation `no-args`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 1608B, stderr 0B)
- Zig exit: `0` (stdout 709B, stderr 0B)

```diff
--- go
+++ zig
@@ -1,33 +1,20 @@
+question
+
 Manage questions — open uncertainties surfaced during work.
 
-Status lifecycle: open → answered (via 'question answer') / wontfix.
+  Status lifecycle: open → answered (via 'question answer') / wontfix.
 
-Usage:
-  planar question [command]
+USAGE:
+  question <command>
 
-Available Commands:
-  add         Record an open question.
-  answer      Provide an answer to an open question.
-  diff        Show a unified diff between the DB's question content and the workbench file.
-  edit        Edit a question in $EDITOR (editor-first flow).
-  link        Create an entity link from a question to another entity.
-  list        List questions.
-  review      Bulk-review open questions in $EDITOR with originating context.
-  show        Show a question with its body and (if answered) the answer.
-  view        View the question's workbench file in $PAGER.
-  wontfix     Mark a question as not going to be answered.
-
-Flags:
-  -h, --help   help for question
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar question [command] --help" for more information about a command.
+COMMANDS:
+  add             Create a new question.
+  edit            Edit a question in $EDITOR (editor-first flow).
+  view            View question's workbench file.
+  diff            Diff question against database version.
+  review          Reviewer entry point for question diff.
+  answer          Record an answer to a question.
+  wontfix         Mark a question as wontfix.
+  list            List questions.
+  show            Show a question's details.
+  link            Create an entity link from a question to another entity.
```

#### 44. `question ` — invocation `real-cwd`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 1608B, stderr 0B)
- Zig exit: `0` (stdout 709B, stderr 0B)
- cwd: `<CWD_DIR>`

```diff
--- go
+++ zig
@@ -1,33 +1,20 @@
+question
+
 Manage questions — open uncertainties surfaced during work.
 
-Status lifecycle: open → answered (via 'question answer') / wontfix.
+  Status lifecycle: open → answered (via 'question answer') / wontfix.
 
-Usage:
-  planar question [command]
+USAGE:
+  question <command>
 
-Available Commands:
-  add         Record an open question.
-  answer      Provide an answer to an open question.
-  diff        Show a unified diff between the DB's question content and the workbench file.
-  edit        Edit a question in $EDITOR (editor-first flow).
-  link        Create an entity link from a question to another entity.
-  list        List questions.
-  review      Bulk-review open questions in $EDITOR with originating context.
-  show        Show a question with its body and (if answered) the answer.
-  view        View the question's workbench file in $PAGER.
-  wontfix     Mark a question as not going to be answered.
-
-Flags:
-  -h, --help   help for question
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar question [command] --help" for more information about a command.
+COMMANDS:
+  add             Create a new question.
+  edit            Edit a question in $EDITOR (editor-first flow).
+  view            View question's workbench file.
+  diff            Diff question against database version.
+  review          Reviewer entry point for question diff.
+  answer          Record an answer to a question.
+  wontfix         Mark a question as wontfix.
+  list            List questions.
+  show            Show a question's details.
+  link            Create an entity link from a question to another entity.
```

#### 45. `assoc --help` — invocation `help`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 1464B, stderr 0B)
- Zig exit: `0` (stdout 661B, stderr 0B)

```diff
--- go
+++ zig
@@ -1,35 +1,19 @@
-Manage associations — the many-to-many tags that group repos into named scopes.
+assoc
 
-Both 'assoc' and 'association' are valid subcommand names.
+Manage associations — the many-to-many tags that group repos into
+  named scopes.
 
-User-creatable kinds: org, project, client, personal, ad-hoc.
-Auto-detected kinds (via 'assoc detect'): host, path, lang.
+  Both 'assoc' and 'association' are valid subcommand names.
+  User-creatable kinds: org, project, client, personal, ad-hoc.
+  Auto-detected kinds (via 'assoc detect'): host, path, lang.
 
-Usage:
-  planar assoc [command]
+USAGE:
+  assoc <command>
 
-Aliases:
-  assoc, association
-
-Available Commands:
-  add         Add a repo to an association.
-  create      Create a new association.
-  detect      Propose (or apply) auto-detected associations for the current directory.
-  list        List all known associations.
-  members     List all project members of an association.
-  remove      Remove a repo from an association.
-
-Flags:
-  -h, --help   help for assoc
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar assoc [command] --help" for more information about a command.
+COMMANDS:
+  list            List all known associations.
+  create          Create a new association.
+  add             Add a repo to an association.
+  remove          Remove a repo from an association.
+  members         List all project members of an association.
+  detect          Propose (or apply) auto-detected associations for the current directory.
```

#### 46. `assoc ` — invocation `no-args`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 1464B, stderr 0B)
- Zig exit: `0` (stdout 661B, stderr 0B)

```diff
--- go
+++ zig
@@ -1,35 +1,19 @@
-Manage associations — the many-to-many tags that group repos into named scopes.
+assoc
 
-Both 'assoc' and 'association' are valid subcommand names.
+Manage associations — the many-to-many tags that group repos into
+  named scopes.
 
-User-creatable kinds: org, project, client, personal, ad-hoc.
-Auto-detected kinds (via 'assoc detect'): host, path, lang.
+  Both 'assoc' and 'association' are valid subcommand names.
+  User-creatable kinds: org, project, client, personal, ad-hoc.
+  Auto-detected kinds (via 'assoc detect'): host, path, lang.
 
-Usage:
-  planar assoc [command]
+USAGE:
+  assoc <command>
 
-Aliases:
-  assoc, association
-
-Available Commands:
-  add         Add a repo to an association.
-  create      Create a new association.
-  detect      Propose (or apply) auto-detected associations for the current directory.
-  list        List all known associations.
-  members     List all project members of an association.
-  remove      Remove a repo from an association.
-
-Flags:
-  -h, --help   help for assoc
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar assoc [command] --help" for more information about a command.
+COMMANDS:
+  list            List all known associations.
+  create          Create a new association.
+  add             Add a repo to an association.
+  remove          Remove a repo from an association.
+  members         List all project members of an association.
+  detect          Propose (or apply) auto-detected associations for the current directory.
```

#### 47. `assoc ` — invocation `real-cwd`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 1464B, stderr 0B)
- Zig exit: `0` (stdout 661B, stderr 0B)
- cwd: `<CWD_DIR>`

```diff
--- go
+++ zig
@@ -1,35 +1,19 @@
-Manage associations — the many-to-many tags that group repos into named scopes.
+assoc
 
-Both 'assoc' and 'association' are valid subcommand names.
+Manage associations — the many-to-many tags that group repos into
+  named scopes.
 
-User-creatable kinds: org, project, client, personal, ad-hoc.
-Auto-detected kinds (via 'assoc detect'): host, path, lang.
+  Both 'assoc' and 'association' are valid subcommand names.
+  User-creatable kinds: org, project, client, personal, ad-hoc.
+  Auto-detected kinds (via 'assoc detect'): host, path, lang.
 
-Usage:
-  planar assoc [command]
+USAGE:
+  assoc <command>
 
-Aliases:
-  assoc, association
-
-Available Commands:
-  add         Add a repo to an association.
-  create      Create a new association.
-  detect      Propose (or apply) auto-detected associations for the current directory.
-  list        List all known associations.
-  members     List all project members of an association.
-  remove      Remove a repo from an association.
-
-Flags:
-  -h, --help   help for assoc
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar assoc [command] --help" for more information about a command.
+COMMANDS:
+  list            List all known associations.
+  create          Create a new association.
+  add             Add a repo to an association.
+  remove          Remove a repo from an association.
+  members         List all project members of an association.
+  detect          Propose (or apply) auto-detected associations for the current directory.
```

#### 48. `capture --help` — invocation `help`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 1466B, stderr 0B)
- Zig exit: `0` (stdout 655B, stderr 0B)

```diff
--- go
+++ zig
@@ -1,30 +1,19 @@
-Capture commands manage explicit session management and context capture.
+capture
 
-Automatic capture happens on every write command; use these subcommands for
-explicit session management, narrative notes, command history, and snapshots.
+Capture commands manage explicit session management and context
+  capture.
 
-Usage:
-  planar capture [command]
+  Automatic capture happens on every write command; use these
+  subcommands for explicit session management, narrative notes,
+  command history, and snapshots.
 
-Available Commands:
-  command     Record a command that was run.
-  end         Close the current (or specified) session.
-  file        Record a file that was touched during the session.
-  note        Append a note entry to the current session.
-  session     Open or reuse a session for the current (vendor, vendor-session-id) tuple.
-  snapshot    Produce a context snapshot for the current or named task.
+USAGE:
+  capture <command>
 
-Flags:
-  -h, --help   help for capture
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar capture [command] --help" for more information about a command.
+COMMANDS:
+  session         Open or reuse a session for the current (vendor, vendor-session-id) tuple.
+  end             End the active or specified session.
+  note            Append a narrative note to the active session.
+  command         Append a command to the active session.
+  file            Attach a file to the active session.
+  snapshot        Create a context snapshot.
```

#### 49. `capture ` — invocation `no-args`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 1466B, stderr 0B)
- Zig exit: `0` (stdout 655B, stderr 0B)

```diff
--- go
+++ zig
@@ -1,30 +1,19 @@
-Capture commands manage explicit session management and context capture.
+capture
 
-Automatic capture happens on every write command; use these subcommands for
-explicit session management, narrative notes, command history, and snapshots.
+Capture commands manage explicit session management and context
+  capture.
 
-Usage:
-  planar capture [command]
+  Automatic capture happens on every write command; use these
+  subcommands for explicit session management, narrative notes,
+  command history, and snapshots.
 
-Available Commands:
-  command     Record a command that was run.
-  end         Close the current (or specified) session.
-  file        Record a file that was touched during the session.
-  note        Append a note entry to the current session.
-  session     Open or reuse a session for the current (vendor, vendor-session-id) tuple.
-  snapshot    Produce a context snapshot for the current or named task.
+USAGE:
+  capture <command>
 
-Flags:
-  -h, --help   help for capture
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar capture [command] --help" for more information about a command.
+COMMANDS:
+  session         Open or reuse a session for the current (vendor, vendor-session-id) tuple.
+  end             End the active or specified session.
+  note            Append a narrative note to the active session.
+  command         Append a command to the active session.
+  file            Attach a file to the active session.
+  snapshot        Create a context snapshot.
```

#### 50. `capture ` — invocation `real-cwd`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 1466B, stderr 0B)
- Zig exit: `0` (stdout 655B, stderr 0B)
- cwd: `<CWD_DIR>`

```diff
--- go
+++ zig
@@ -1,30 +1,19 @@
-Capture commands manage explicit session management and context capture.
+capture
 
-Automatic capture happens on every write command; use these subcommands for
-explicit session management, narrative notes, command history, and snapshots.
+Capture commands manage explicit session management and context
+  capture.
 
-Usage:
-  planar capture [command]
+  Automatic capture happens on every write command; use these
+  subcommands for explicit session management, narrative notes,
+  command history, and snapshots.
 
-Available Commands:
-  command     Record a command that was run.
-  end         Close the current (or specified) session.
-  file        Record a file that was touched during the session.
-  note        Append a note entry to the current session.
-  session     Open or reuse a session for the current (vendor, vendor-session-id) tuple.
-  snapshot    Produce a context snapshot for the current or named task.
+USAGE:
+  capture <command>
 
-Flags:
-  -h, --help   help for capture
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar capture [command] --help" for more information about a command.
+COMMANDS:
+  session         Open or reuse a session for the current (vendor, vendor-session-id) tuple.
+  end             End the active or specified session.
+  note            Append a narrative note to the active session.
+  command         Append a command to the active session.
+  file            Attach a file to the active session.
+  snapshot        Create a context snapshot.
```

#### 51. `config --help` — invocation `help`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 1519B, stderr 0B)
- Zig exit: `0` (stdout 634B, stderr 0B)

```diff
--- go
+++ zig
@@ -1,35 +1,20 @@
+config
+
 Read, inspect, and validate the Planar configuration file.
 
-The configuration file lives at ~/.planar/config.toml by default.
-Set $PLANAR_CONFIG_PATH to use a different path.
+  The configuration file lives at ~/.planar/config.toml by default.
+  Set $PLANAR_CONFIG_PATH to use a different path.
+  Resolution order (highest to lowest priority):
+    1. Explicit --config-path flag
+    2. $PLANAR_CONFIG_PATH
+    3. ~/.planar/config.toml
 
-Resolution order (highest to lowest priority):
-  1. Environment variables
-  2. Per-association overrides ([associations."<slug>"] sections)
-  3. Top-level config file values
-  4. Embedded defaults (shipped with the binary)
+USAGE:
+  config <command>
 
-Usage:
-  planar config [command]
-
-Available Commands:
-  edit        Open the config file in $EDITOR.
-  init        Idempotently write a starter config.toml if absent.
-  path        Print the resolved config file path.
-  show        Print the resolved configuration.
-  validate    Validate the config file syntax and schema.
-
-Flags:
-  -h, --help   help for config
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar config [command] --help" for more information about a command.
+COMMANDS:
+  show            Print the resolved configuration.
+  edit            Edit the configuration file in $EDITOR.
+  validate        Validate configuration file syntax.
+  init            Initialize the configuration file.
+  path            Show the configuration file path.
```

#### 52. `config ` — invocation `no-args`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 1519B, stderr 0B)
- Zig exit: `0` (stdout 634B, stderr 0B)

```diff
--- go
+++ zig
@@ -1,35 +1,20 @@
+config
+
 Read, inspect, and validate the Planar configuration file.
 
-The configuration file lives at ~/.planar/config.toml by default.
-Set $PLANAR_CONFIG_PATH to use a different path.
+  The configuration file lives at ~/.planar/config.toml by default.
+  Set $PLANAR_CONFIG_PATH to use a different path.
+  Resolution order (highest to lowest priority):
+    1. Explicit --config-path flag
+    2. $PLANAR_CONFIG_PATH
+    3. ~/.planar/config.toml
 
-Resolution order (highest to lowest priority):
-  1. Environment variables
-  2. Per-association overrides ([associations."<slug>"] sections)
-  3. Top-level config file values
-  4. Embedded defaults (shipped with the binary)
+USAGE:
+  config <command>
 
-Usage:
-  planar config [command]
-
-Available Commands:
-  edit        Open the config file in $EDITOR.
-  init        Idempotently write a starter config.toml if absent.
-  path        Print the resolved config file path.
-  show        Print the resolved configuration.
-  validate    Validate the config file syntax and schema.
-
-Flags:
-  -h, --help   help for config
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar config [command] --help" for more information about a command.
+COMMANDS:
+  show            Print the resolved configuration.
+  edit            Edit the configuration file in $EDITOR.
+  validate        Validate configuration file syntax.
+  init            Initialize the configuration file.
+  path            Show the configuration file path.
```

#### 53. `config ` — invocation `real-cwd`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 1519B, stderr 0B)
- Zig exit: `0` (stdout 634B, stderr 0B)
- cwd: `<CWD_DIR>`

```diff
--- go
+++ zig
@@ -1,35 +1,20 @@
+config
+
 Read, inspect, and validate the Planar configuration file.
 
-The configuration file lives at ~/.planar/config.toml by default.
-Set $PLANAR_CONFIG_PATH to use a different path.
+  The configuration file lives at ~/.planar/config.toml by default.
+  Set $PLANAR_CONFIG_PATH to use a different path.
+  Resolution order (highest to lowest priority):
+    1. Explicit --config-path flag
+    2. $PLANAR_CONFIG_PATH
+    3. ~/.planar/config.toml
 
-Resolution order (highest to lowest priority):
-  1. Environment variables
-  2. Per-association overrides ([associations."<slug>"] sections)
-  3. Top-level config file values
-  4. Embedded defaults (shipped with the binary)
+USAGE:
+  config <command>
 
-Usage:
-  planar config [command]
-
-Available Commands:
-  edit        Open the config file in $EDITOR.
-  init        Idempotently write a starter config.toml if absent.
-  path        Print the resolved config file path.
-  show        Print the resolved configuration.
-  validate    Validate the config file syntax and schema.
-
-Flags:
-  -h, --help   help for config
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar config [command] --help" for more information about a command.
+COMMANDS:
+  show            Print the resolved configuration.
+  edit            Edit the configuration file in $EDITOR.
+  validate        Validate configuration file syntax.
+  init            Initialize the configuration file.
+  path            Show the configuration file path.
```

#### 54. `workspace --help` — invocation `help`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 1561B, stderr 0B)
- Zig exit: `0` (stdout 545B, stderr 0B)

```diff
--- go
+++ zig
@@ -1,32 +1,17 @@
+workspace
+
 Workspace administration.
 
-A workspace is identified by an associations row of kind=org. Each
-workspace owns a state directory under
-${PLANAR_HOME:-~/.planar}/workspaces/<org_id>/ holding the canonical
-AGENTS.md, routing-table.json, config.toml, and README.md. Symlinks
-(AGENTS.md and CLAUDE.md) at the workspace root project that content
-into the directory where agents expect to find it.
+  A workspace is identified by an associations row of kind=org. Each
+  workspace owns a state directory under
+  ${PLANAR_HOME:-~/.planar}/workspaces/<org_id>/ holding the canonical
+  AGENTS.md surface for the org.
 
-Usage:
-  planar workspace [command]
+USAGE:
+  workspace <command>
 
-Available Commands:
-  doctor      Detect and repair workspace state directories and symlinks.
-  init        Initialize the current directory as a workspace and register each child repo as a project.
-  regenerate  Render AGENTS.md for the workspace from the routing table.
-  routing     Build or inspect the workspace routing table.
-
-Flags:
-  -h, --help   help for workspace
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar workspace [command] --help" for more information about a command.
+COMMANDS:
+  init            Initialize a workspace (org-level association).
+  doctor          Scan and fix workspace registration and state consistency.
+  routing         Manage workspace routing table.
+  regenerate      Regenerate AGENTS.md from current state.
```

#### 55. `workspace ` — invocation `no-args`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 1561B, stderr 0B)
- Zig exit: `0` (stdout 545B, stderr 0B)

```diff
--- go
+++ zig
@@ -1,32 +1,17 @@
+workspace
+
 Workspace administration.
 
-A workspace is identified by an associations row of kind=org. Each
-workspace owns a state directory under
-${PLANAR_HOME:-~/.planar}/workspaces/<org_id>/ holding the canonical
-AGENTS.md, routing-table.json, config.toml, and README.md. Symlinks
-(AGENTS.md and CLAUDE.md) at the workspace root project that content
-into the directory where agents expect to find it.
+  A workspace is identified by an associations row of kind=org. Each
+  workspace owns a state directory under
+  ${PLANAR_HOME:-~/.planar}/workspaces/<org_id>/ holding the canonical
+  AGENTS.md surface for the org.
 
-Usage:
-  planar workspace [command]
+USAGE:
+  workspace <command>
 
-Available Commands:
-  doctor      Detect and repair workspace state directories and symlinks.
-  init        Initialize the current directory as a workspace and register each child repo as a project.
-  regenerate  Render AGENTS.md for the workspace from the routing table.
-  routing     Build or inspect the workspace routing table.
-
-Flags:
-  -h, --help   help for workspace
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar workspace [command] --help" for more information about a command.
+COMMANDS:
+  init            Initialize a workspace (org-level association).
+  doctor          Scan and fix workspace registration and state consistency.
+  routing         Manage workspace routing table.
+  regenerate      Regenerate AGENTS.md from current state.
```

#### 56. `workspace ` — invocation `real-cwd`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 1561B, stderr 0B)
- Zig exit: `0` (stdout 545B, stderr 0B)
- cwd: `<CWD_DIR>`

```diff
--- go
+++ zig
@@ -1,32 +1,17 @@
+workspace
+
 Workspace administration.
 
-A workspace is identified by an associations row of kind=org. Each
-workspace owns a state directory under
-${PLANAR_HOME:-~/.planar}/workspaces/<org_id>/ holding the canonical
-AGENTS.md, routing-table.json, config.toml, and README.md. Symlinks
-(AGENTS.md and CLAUDE.md) at the workspace root project that content
-into the directory where agents expect to find it.
+  A workspace is identified by an associations row of kind=org. Each
+  workspace owns a state directory under
+  ${PLANAR_HOME:-~/.planar}/workspaces/<org_id>/ holding the canonical
+  AGENTS.md surface for the org.
 
-Usage:
-  planar workspace [command]
+USAGE:
+  workspace <command>
 
-Available Commands:
-  doctor      Detect and repair workspace state directories and symlinks.
-  init        Initialize the current directory as a workspace and register each child repo as a project.
-  regenerate  Render AGENTS.md for the workspace from the routing table.
-  routing     Build or inspect the workspace routing table.
-
-Flags:
-  -h, --help   help for workspace
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar workspace [command] --help" for more information about a command.
+COMMANDS:
+  init            Initialize a workspace (org-level association).
+  doctor          Scan and fix workspace registration and state consistency.
+  routing         Manage workspace routing table.
+  regenerate      Regenerate AGENTS.md from current state.
```

#### 57. `templates --json` — invocation `json`

- Failure class: `zig-failed`
- Go exit: `0` (stdout 1924B, stderr 0B)
- Zig exit: `1` (stdout 42B, stderr 25B)

```diff
--- go
+++ zig
@@ -1,38 +1 @@
-Manage the template plane: list available templates, show their raw JSON,
-render them against a DB entity (dry run), validate syntax, initialise the
-default set on disk, and print resolution paths.
-
-Templates resolve via a three-level fallback chain:
-  1. <root>/<set>/<system>/<kind>.json          (user-chosen set on disk)
-  2. <root>/default/<system>/<kind>.json        (baseline set on disk)
-  3. embedded defaults in the binary            (always present)
-
-The root defaults to ~/.planar/templates/ (Config.Templates.Dir).
-The default set is determined by [templates] default_set in config.toml,
-or overridden per-association via [associations."<slug>"] default_template_set.
-
-Usage:
-  planar templates [command]
-
-Available Commands:
-  init        Extract embedded default templates to the templates root.
-  list        List available templates.
-  path        Print the resolved template path or the templates root directory.
-  render      Render a template against a DB entity (dry run).
-  show        Print the raw JSON of a resolved template.
-  validate    Validate a template file or all templates under the root.
-
-Flags:
-  -h, --help   help for templates
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar templates [command] --help" for more information about a command.
+error: unknown subcommand [in: templates]
--- go.stderr
+++ zig.stderr
@@ -0,0 +1 @@
+error: UnknownSubcommand
--- exit
+++ exit
-go=0
+zig=1
```

#### 58. `templates --json` — invocation `real-cwd-json`

- Failure class: `zig-failed`
- Go exit: `0` (stdout 1924B, stderr 0B)
- Zig exit: `1` (stdout 42B, stderr 25B)
- cwd: `<CWD_DIR>`

```diff
--- go
+++ zig
@@ -1,38 +1 @@
-Manage the template plane: list available templates, show their raw JSON,
-render them against a DB entity (dry run), validate syntax, initialise the
-default set on disk, and print resolution paths.
-
-Templates resolve via a three-level fallback chain:
-  1. <root>/<set>/<system>/<kind>.json          (user-chosen set on disk)
-  2. <root>/default/<system>/<kind>.json        (baseline set on disk)
-  3. embedded defaults in the binary            (always present)
-
-The root defaults to ~/.planar/templates/ (Config.Templates.Dir).
-The default set is determined by [templates] default_set in config.toml,
-or overridden per-association via [associations."<slug>"] default_template_set.
-
-Usage:
-  planar templates [command]
-
-Available Commands:
-  init        Extract embedded default templates to the templates root.
-  list        List available templates.
-  path        Print the resolved template path or the templates root directory.
-  render      Render a template against a DB entity (dry run).
-  show        Print the raw JSON of a resolved template.
-  validate    Validate a template file or all templates under the root.
-
-Flags:
-  -h, --help   help for templates
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar templates [command] --help" for more information about a command.
+error: unknown subcommand [in: templates]
--- go.stderr
+++ zig.stderr
@@ -0,0 +1 @@
+error: UnknownSubcommand
--- exit
+++ exit
-go=0
+zig=1
```

#### 59. `task --json` — invocation `json`

- Failure class: `zig-failed`
- Go exit: `0` (stdout 1878B, stderr 0B)
- Zig exit: `1` (stdout 37B, stderr 25B)

```diff
--- go
+++ zig
@@ -1,39 +1 @@
-Manage tasks — the discrete units of work.
-
-Tasks may belong to a plan (--plan) or another task (--parent), and carry
-the next_action field required by resume validate.
-Status lifecycle: todo → doing → done / cancelled; blocked is set via task block.
-
-Usage:
-  planar task [command]
-
-Available Commands:
-  add         Create a new task.
-  block       Mark a task as blocked and record the blocking relationship.
-  cancel      Cancel one or more tasks.
-  diff        Show a unified diff between the DB's task content and the workbench file.
-  done        Mark one or more tasks as done.
-  edit        Edit a task in $EDITOR (editor-first flow).
-  link        Create an entity link from a task to another entity.
-  list        List tasks.
-  reopen      Reopen a done or cancelled task with an audit-trail entry.
-  review      Bulk-review open tasks in $EDITOR with per-task editable fields.
-  show        Show full task details.
-  touches     Manage repo-touches links on a task.
-  update      Update mutable fields on a task.
-  view        View the task's workbench file in $PAGER.
-
-Flags:
-  -h, --help   help for task
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar task [command] --help" for more information about a command.
+error: unknown subcommand [in: task]
--- go.stderr
+++ zig.stderr
@@ -0,0 +1 @@
+error: UnknownSubcommand
--- exit
+++ exit
-go=0
+zig=1
```

#### 60. `task --json` — invocation `real-cwd-json`

- Failure class: `zig-failed`
- Go exit: `0` (stdout 1878B, stderr 0B)
- Zig exit: `1` (stdout 37B, stderr 25B)
- cwd: `<CWD_DIR>`

```diff
--- go
+++ zig
@@ -1,39 +1 @@
-Manage tasks — the discrete units of work.
-
-Tasks may belong to a plan (--plan) or another task (--parent), and carry
-the next_action field required by resume validate.
-Status lifecycle: todo → doing → done / cancelled; blocked is set via task block.
-
-Usage:
-  planar task [command]
-
-Available Commands:
-  add         Create a new task.
-  block       Mark a task as blocked and record the blocking relationship.
-  cancel      Cancel one or more tasks.
-  diff        Show a unified diff between the DB's task content and the workbench file.
-  done        Mark one or more tasks as done.
-  edit        Edit a task in $EDITOR (editor-first flow).
-  link        Create an entity link from a task to another entity.
-  list        List tasks.
-  reopen      Reopen a done or cancelled task with an audit-trail entry.
-  review      Bulk-review open tasks in $EDITOR with per-task editable fields.
-  show        Show full task details.
-  touches     Manage repo-touches links on a task.
-  update      Update mutable fields on a task.
-  view        View the task's workbench file in $PAGER.
-
-Flags:
-  -h, --help   help for task
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar task [command] --help" for more information about a command.
+error: unknown subcommand [in: task]
--- go.stderr
+++ zig.stderr
@@ -0,0 +1 @@
+error: UnknownSubcommand
--- exit
+++ exit
-go=0
+zig=1
```

#### 61. `local --json` — invocation `json`

- Failure class: `zig-failed`
- Go exit: `0` (stdout 1877B, stderr 0B)
- Zig exit: `1` (stdout 38B, stderr 25B)

```diff
--- go
+++ zig
@@ -1,36 +1 @@
-Manage the operator's local sandbox for personal skills and agents.
-
-Authors a single source file per skill or agent under ~/.planar/local/
-and creates per-vendor symlinks (with copy fallback) into each vendor's
-install directory. Edits to the source file propagate immediately to
-every vendor because the vendor paths are symlinks.
-
-Promotion to the canonical Planar repo is intentionally manual — copy
-the file into the repo and follow the normal contribution flow.
-
-See docs/concepts.md § "Local sandbox" for the full design.
-
-Usage:
-  planar local [command]
-
-Available Commands:
-  import      Import skill or agent files from an external directory into the sandbox and link them.
-  link        Install per-vendor symlinks for one (or all) sandbox source files.
-  list        List every recorded sandbox install across all vendors. Marks broken links.
-  migrate     Convert legacy flat sandbox skills (~/.planar/local/skills/<name>.md) into the dir-shape layout (<name>/SKILL.md).
-  unlink      Remove per-vendor installs for a sandbox source. Optionally purge the source file.
-
-Flags:
-  -h, --help   help for local
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar local [command] --help" for more information about a command.
+error: unknown subcommand [in: local]
--- go.stderr
+++ zig.stderr
@@ -0,0 +1 @@
+error: UnknownSubcommand
--- exit
+++ exit
-go=0
+zig=1
```

#### 62. `local --json` — invocation `real-cwd-json`

- Failure class: `zig-failed`
- Go exit: `0` (stdout 1877B, stderr 0B)
- Zig exit: `1` (stdout 38B, stderr 25B)
- cwd: `<CWD_DIR>`

```diff
--- go
+++ zig
@@ -1,36 +1 @@
-Manage the operator's local sandbox for personal skills and agents.
-
-Authors a single source file per skill or agent under ~/.planar/local/
-and creates per-vendor symlinks (with copy fallback) into each vendor's
-install directory. Edits to the source file propagate immediately to
-every vendor because the vendor paths are symlinks.
-
-Promotion to the canonical Planar repo is intentionally manual — copy
-the file into the repo and follow the normal contribution flow.
-
-See docs/concepts.md § "Local sandbox" for the full design.
-
-Usage:
-  planar local [command]
-
-Available Commands:
-  import      Import skill or agent files from an external directory into the sandbox and link them.
-  link        Install per-vendor symlinks for one (or all) sandbox source files.
-  list        List every recorded sandbox install across all vendors. Marks broken links.
-  migrate     Convert legacy flat sandbox skills (~/.planar/local/skills/<name>.md) into the dir-shape layout (<name>/SKILL.md).
-  unlink      Remove per-vendor installs for a sandbox source. Optionally purge the source file.
-
-Flags:
-  -h, --help   help for local
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar local [command] --help" for more information about a command.
+error: unknown subcommand [in: local]
--- go.stderr
+++ zig.stderr
@@ -0,0 +1 @@
+error: UnknownSubcommand
--- exit
+++ exit
-go=0
+zig=1
```

#### 63. `workbench --json` — invocation `json`

- Failure class: `zig-failed`
- Go exit: `0` (stdout 1870B, stderr 0B)
- Zig exit: `1` (stdout 42B, stderr 25B)

```diff
--- go
+++ zig
@@ -1,36 +1 @@
-Manage the bidirectional sync surface between the workbench filesystem
-and the Planar database.
-
-The workbench root defaults to ~/.planar/workbench/ and can be overridden
-with the PLANAR_WORKBENCH_ROOT environment variable.
-
-Usage:
-  planar workbench [command]
-
-Available Commands:
-  archive           Remove the FS tree; DB is retained.
-  edit              Open the plan's workbench directory in $EDITOR; sync on exit.
-  extract-questions Parse the Open questions section from each spec in the plan's workbench directory.
-  list              List active features with FS trees.
-  publish           Snapshot a feature's workbench tree to an external path.
-  pull              Apply FS→DB changes; report DB→FS drift.
-  push              Apply DB→FS changes atomically; report FS→DB drift.
-  resolve           Settle a sync conflict by choosing FS or DB.
-  restore           Recreate the FS tree from the DB.
-  status            Show drift and conflicts without writing.
-  sync              Full reconciliation: apply non-conflicting changes in both directions.
-
-Flags:
-  -h, --help   help for workbench
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar workbench [command] --help" for more information about a command.
+error: unknown subcommand [in: workbench]
--- go.stderr
+++ zig.stderr
@@ -0,0 +1 @@
+error: UnknownSubcommand
--- exit
+++ exit
-go=0
+zig=1
```

#### 64. `workbench --json` — invocation `real-cwd-json`

- Failure class: `zig-failed`
- Go exit: `0` (stdout 1870B, stderr 0B)
- Zig exit: `1` (stdout 42B, stderr 25B)
- cwd: `<CWD_DIR>`

```diff
--- go
+++ zig
@@ -1,36 +1 @@
-Manage the bidirectional sync surface between the workbench filesystem
-and the Planar database.
-
-The workbench root defaults to ~/.planar/workbench/ and can be overridden
-with the PLANAR_WORKBENCH_ROOT environment variable.
-
-Usage:
-  planar workbench [command]
-
-Available Commands:
-  archive           Remove the FS tree; DB is retained.
-  edit              Open the plan's workbench directory in $EDITOR; sync on exit.
-  extract-questions Parse the Open questions section from each spec in the plan's workbench directory.
-  list              List active features with FS trees.
-  publish           Snapshot a feature's workbench tree to an external path.
-  pull              Apply FS→DB changes; report DB→FS drift.
-  push              Apply DB→FS changes atomically; report FS→DB drift.
-  resolve           Settle a sync conflict by choosing FS or DB.
-  restore           Recreate the FS tree from the DB.
-  status            Show drift and conflicts without writing.
-  sync              Full reconciliation: apply non-conflicting changes in both directions.
-
-Flags:
-  -h, --help   help for workbench
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar workbench [command] --help" for more information about a command.
+error: unknown subcommand [in: workbench]
--- go.stderr
+++ zig.stderr
@@ -0,0 +1 @@
+error: UnknownSubcommand
--- exit
+++ exit
-go=0
+zig=1
```

#### 65. `scope --help` — invocation `help`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 1375B, stderr 0B)
- Zig exit: `0` (stdout 599B, stderr 0B)

```diff
--- go
+++ zig
@@ -1,28 +1,17 @@
-Inspect the scope Planar will resolve for the current working directory.
-Plan 153 removed the active scope stack; scope is now derived from cwd and
-overridden by passing --scope <slug> to individual verbs.
+scope
 
-Usage:
-  planar scope [command]
+Inspect the scope Planar will resolve for the current working
+  directory.
 
-Available Commands:
-  clear       Removed in plan 153 M5 — see `planar scope show`.
-  pop         Removed in plan 153 M5 — see `planar scope show`.
-  show        Show the cwd-derived scope (and any --scope override).
-  suggest     Suggest scope associations based on cwd.
-  use         Removed in plan 153 M5 — see `planar scope show`.
+  Plan 153 removed the active scope stack; scope is now derived from
+  cwd and overridden by passing --scope <slug> to individual verbs.
 
-Flags:
-  -h, --help   help for scope
+USAGE:
+  scope <command>
 
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar scope [command] --help" for more information about a command.
+COMMANDS:
+  show            Show the cwd-derived scope (and any --scope override).
+  suggest         Suggest scope associations based on cwd.
+  use             Removed in plan 153 M5 — see `planar scope show`.
+  pop             Removed in plan 153 M5 — see `planar scope show`.
+  clear           Removed in plan 153 M5 — see `planar scope show`.
```

#### 66. `scope ` — invocation `no-args`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 1375B, stderr 0B)
- Zig exit: `0` (stdout 599B, stderr 0B)

```diff
--- go
+++ zig
@@ -1,28 +1,17 @@
-Inspect the scope Planar will resolve for the current working directory.
-Plan 153 removed the active scope stack; scope is now derived from cwd and
-overridden by passing --scope <slug> to individual verbs.
+scope
 
-Usage:
-  planar scope [command]
+Inspect the scope Planar will resolve for the current working
+  directory.
 
-Available Commands:
-  clear       Removed in plan 153 M5 — see `planar scope show`.
-  pop         Removed in plan 153 M5 — see `planar scope show`.
-  show        Show the cwd-derived scope (and any --scope override).
-  suggest     Suggest scope associations based on cwd.
-  use         Removed in plan 153 M5 — see `planar scope show`.
+  Plan 153 removed the active scope stack; scope is now derived from
+  cwd and overridden by passing --scope <slug> to individual verbs.
 
-Flags:
-  -h, --help   help for scope
+USAGE:
+  scope <command>
 
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar scope [command] --help" for more information about a command.
+COMMANDS:
+  show            Show the cwd-derived scope (and any --scope override).
+  suggest         Suggest scope associations based on cwd.
+  use             Removed in plan 153 M5 — see `planar scope show`.
+  pop             Removed in plan 153 M5 — see `planar scope show`.
+  clear           Removed in plan 153 M5 — see `planar scope show`.
```

#### 67. `scope ` — invocation `real-cwd`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 1375B, stderr 0B)
- Zig exit: `0` (stdout 599B, stderr 0B)
- cwd: `<CWD_DIR>`

```diff
--- go
+++ zig
@@ -1,28 +1,17 @@
-Inspect the scope Planar will resolve for the current working directory.
-Plan 153 removed the active scope stack; scope is now derived from cwd and
-overridden by passing --scope <slug> to individual verbs.
+scope
 
-Usage:
-  planar scope [command]
+Inspect the scope Planar will resolve for the current working
+  directory.
 
-Available Commands:
-  clear       Removed in plan 153 M5 — see `planar scope show`.
-  pop         Removed in plan 153 M5 — see `planar scope show`.
-  show        Show the cwd-derived scope (and any --scope override).
-  suggest     Suggest scope associations based on cwd.
-  use         Removed in plan 153 M5 — see `planar scope show`.
+  Plan 153 removed the active scope stack; scope is now derived from
+  cwd and overridden by passing --scope <slug> to individual verbs.
 
-Flags:
-  -h, --help   help for scope
+USAGE:
+  scope <command>
 
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar scope [command] --help" for more information about a command.
+COMMANDS:
+  show            Show the cwd-derived scope (and any --scope override).
+  suggest         Suggest scope associations based on cwd.
+  use             Removed in plan 153 M5 — see `planar scope show`.
+  pop             Removed in plan 153 M5 — see `planar scope show`.
+  clear           Removed in plan 153 M5 — see `planar scope show`.
```

#### 68. `doc --json` — invocation `json`

- Failure class: `zig-failed`
- Go exit: `0` (stdout 1644B, stderr 0B)
- Zig exit: `1` (stdout 36B, stderr 25B)

```diff
--- go
+++ zig
@@ -1,33 +1 @@
-Outward-facing documentation tooling.
-
-The lint subcommand validates citations and reference declarations in
-markdown docs. The manifest subcommands track per-doc content + source
-hashes in .manifest-docs so drift between published docs and the
-artifacts they were synthesized from can be detected in O(1).
-
-Usage:
-  planar doc [command]
-
-Available Commands:
-  backlinks   List every published doc whose sources cite the given entity.
-  coverage    Report plans with status=done that no published doc cites.
-  lint        Lint citations and references across docs.
-  manifest    Manage .manifest-docs (build, verify, diff, info).
-  orphans     List artifacts of the given kind with zero backlinks in .manifest-docs.
-  promote     Promote internal Planar entities into a published doc.
-  regenerate  Re-synthesise existing docs from their current sources.
-
-Flags:
-  -h, --help   help for doc
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar doc [command] --help" for more information about a command.
+error: unknown subcommand [in: doc]
--- go.stderr
+++ zig.stderr
@@ -0,0 +1 @@
+error: UnknownSubcommand
--- exit
+++ exit
-go=0
+zig=1
```

#### 69. `doc --json` — invocation `real-cwd-json`

- Failure class: `zig-failed`
- Go exit: `0` (stdout 1644B, stderr 0B)
- Zig exit: `1` (stdout 36B, stderr 25B)
- cwd: `<CWD_DIR>`

```diff
--- go
+++ zig
@@ -1,33 +1 @@
-Outward-facing documentation tooling.
-
-The lint subcommand validates citations and reference declarations in
-markdown docs. The manifest subcommands track per-doc content + source
-hashes in .manifest-docs so drift between published docs and the
-artifacts they were synthesized from can be detected in O(1).
-
-Usage:
-  planar doc [command]
-
-Available Commands:
-  backlinks   List every published doc whose sources cite the given entity.
-  coverage    Report plans with status=done that no published doc cites.
-  lint        Lint citations and references across docs.
-  manifest    Manage .manifest-docs (build, verify, diff, info).
-  orphans     List artifacts of the given kind with zero backlinks in .manifest-docs.
-  promote     Promote internal Planar entities into a published doc.
-  regenerate  Re-synthesise existing docs from their current sources.
-
-Flags:
-  -h, --help   help for doc
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar doc [command] --help" for more information about a command.
+error: unknown subcommand [in: doc]
--- go.stderr
+++ zig.stderr
@@ -0,0 +1 @@
+error: UnknownSubcommand
--- exit
+++ exit
-go=0
+zig=1
```

#### 70. `scenario --json` — invocation `json`

- Failure class: `zig-failed`
- Go exit: `0` (stdout 1638B, stderr 0B)
- Zig exit: `1` (stdout 41B, stderr 25B)

```diff
--- go
+++ zig
@@ -1,33 +1 @@
-Manage test scenarios — verification artifacts tied to specs, plans, or tasks.
-
-Planar records scenarios and their outcomes; it does not execute them.
-Status lifecycle: draft → ready → verified / failing → retired.
-
-Usage:
-  planar scenario [command]
-
-Available Commands:
-  add         Create a new test scenario.
-  diff        Show a unified diff between the DB's scenario content and the workbench file.
-  edit        Edit a test scenario in $EDITOR (editor-first flow).
-  link        Create an entity link from a scenario to another entity.
-  list        List test scenarios.
-  retire      Mark a scenario as retired (no longer relevant).
-  show        Show a scenario's full details including last run outcome.
-  verify      Record the outcome of running a scenario.
-  view        View the scenario's workbench file in $PAGER.
-
-Flags:
-  -h, --help   help for scenario
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar scenario [command] --help" for more information about a command.
+error: unknown subcommand [in: scenario]
--- go.stderr
+++ zig.stderr
@@ -0,0 +1 @@
+error: UnknownSubcommand
--- exit
+++ exit
-go=0
+zig=1
```

#### 71. `scenario --json` — invocation `real-cwd-json`

- Failure class: `zig-failed`
- Go exit: `0` (stdout 1638B, stderr 0B)
- Zig exit: `1` (stdout 41B, stderr 25B)
- cwd: `<CWD_DIR>`

```diff
--- go
+++ zig
@@ -1,33 +1 @@
-Manage test scenarios — verification artifacts tied to specs, plans, or tasks.
-
-Planar records scenarios and their outcomes; it does not execute them.
-Status lifecycle: draft → ready → verified / failing → retired.
-
-Usage:
-  planar scenario [command]
-
-Available Commands:
-  add         Create a new test scenario.
-  diff        Show a unified diff between the DB's scenario content and the workbench file.
-  edit        Edit a test scenario in $EDITOR (editor-first flow).
-  link        Create an entity link from a scenario to another entity.
-  list        List test scenarios.
-  retire      Mark a scenario as retired (no longer relevant).
-  show        Show a scenario's full details including last run outcome.
-  verify      Record the outcome of running a scenario.
-  view        View the scenario's workbench file in $PAGER.
-
-Flags:
-  -h, --help   help for scenario
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar scenario [command] --help" for more information about a command.
+error: unknown subcommand [in: scenario]
--- go.stderr
+++ zig.stderr
@@ -0,0 +1 @@
+error: UnknownSubcommand
--- exit
+++ exit
-go=0
+zig=1
```

#### 72. `ext --help` — invocation `help`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 1289B, stderr 0B)
- Zig exit: `0` (stdout 544B, stderr 0B)

```diff
--- go
+++ zig
@@ -1,28 +1,16 @@
+ext
+
 Register and interact with external systems on the operational plane.
 
-Sub-commands: register, list, test, create, propagate.
+  Sub-commands: register, list, test, create, propagate.
+  Currently supported systems: Jira, GitHub Issues, GitHub Projects.
 
-Usage:
-  planar ext [command]
+USAGE:
+  ext <command>
 
-Available Commands:
-  create      Create an external counterpart for a local entity.
-  list        List all registered external systems.
-  propagate   Propagate a feature (anchor plan + descendants) to a registered external system.
-  register    Register an external system.
-  test        Verify auth and reachability for an external system.
-
-Flags:
-  -h, --help   help for ext
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar ext [command] --help" for more information about a command.
+COMMANDS:
+  register        Register an external system.
+  list            List registered external systems.
+  test            Test connection to an external system.
+  create          Create an external counterpart for a local entity.
+  propagate       Propagate a feature (plan + descendants) to an external system.
```

#### 73. `ext ` — invocation `no-args`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 1289B, stderr 0B)
- Zig exit: `0` (stdout 544B, stderr 0B)

```diff
--- go
+++ zig
@@ -1,28 +1,16 @@
+ext
+
 Register and interact with external systems on the operational plane.
 
-Sub-commands: register, list, test, create, propagate.
+  Sub-commands: register, list, test, create, propagate.
+  Currently supported systems: Jira, GitHub Issues, GitHub Projects.
 
-Usage:
-  planar ext [command]
+USAGE:
+  ext <command>
 
-Available Commands:
-  create      Create an external counterpart for a local entity.
-  list        List all registered external systems.
-  propagate   Propagate a feature (anchor plan + descendants) to a registered external system.
-  register    Register an external system.
-  test        Verify auth and reachability for an external system.
-
-Flags:
-  -h, --help   help for ext
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar ext [command] --help" for more information about a command.
+COMMANDS:
+  register        Register an external system.
+  list            List registered external systems.
+  test            Test connection to an external system.
+  create          Create an external counterpart for a local entity.
+  propagate       Propagate a feature (plan + descendants) to an external system.
```

#### 74. `ext ` — invocation `real-cwd`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 1289B, stderr 0B)
- Zig exit: `0` (stdout 544B, stderr 0B)
- cwd: `<CWD_DIR>`

```diff
--- go
+++ zig
@@ -1,28 +1,16 @@
+ext
+
 Register and interact with external systems on the operational plane.
 
-Sub-commands: register, list, test, create, propagate.
+  Sub-commands: register, list, test, create, propagate.
+  Currently supported systems: Jira, GitHub Issues, GitHub Projects.
 
-Usage:
-  planar ext [command]
+USAGE:
+  ext <command>
 
-Available Commands:
-  create      Create an external counterpart for a local entity.
-  list        List all registered external systems.
-  propagate   Propagate a feature (anchor plan + descendants) to a registered external system.
-  register    Register an external system.
-  test        Verify auth and reachability for an external system.
-
-Flags:
-  -h, --help   help for ext
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar ext [command] --help" for more information about a command.
+COMMANDS:
+  register        Register an external system.
+  list            List registered external systems.
+  test            Test connection to an external system.
+  create          Create an external counterpart for a local entity.
+  propagate       Propagate a feature (plan + descendants) to an external system.
```

#### 75. `decision --json` — invocation `json`

- Failure class: `zig-failed`
- Go exit: `0` (stdout 1617B, stderr 0B)
- Zig exit: `1` (stdout 41B, stderr 25B)

```diff
--- go
+++ zig
@@ -1,34 +1 @@
-Manage decision records — rationale for choices made during work.
-
-Status lifecycle: proposed → accepted / superseded / withdrawn.
-Terminal statuses: superseded, withdrawn.
-
-Usage:
-  planar decision [command]
-
-Available Commands:
-  accept      Mark a decision as accepted.
-  add         Record a new decision.
-  diff        Show a unified diff between the DB's decision content and the workbench file.
-  edit        Edit a decision in $EDITOR (editor-first flow).
-  link        Create an entity link from a decision to another entity.
-  list        List decisions.
-  show        Show a decision with body, rationale, status, and linked session.
-  supersede   Mark a decision as superseded by a newer decision.
-  view        View the decision's workbench file in $PAGER.
-  withdraw    Mark a decision as withdrawn.
-
-Flags:
-  -h, --help   help for decision
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar decision [command] --help" for more information about a command.
+error: unknown subcommand [in: decision]
--- go.stderr
+++ zig.stderr
@@ -0,0 +1 @@
+error: UnknownSubcommand
--- exit
+++ exit
-go=0
+zig=1
```

#### 76. `decision --json` — invocation `real-cwd-json`

- Failure class: `zig-failed`
- Go exit: `0` (stdout 1617B, stderr 0B)
- Zig exit: `1` (stdout 41B, stderr 25B)
- cwd: `<CWD_DIR>`

```diff
--- go
+++ zig
@@ -1,34 +1 @@
-Manage decision records — rationale for choices made during work.
-
-Status lifecycle: proposed → accepted / superseded / withdrawn.
-Terminal statuses: superseded, withdrawn.
-
-Usage:
-  planar decision [command]
-
-Available Commands:
-  accept      Mark a decision as accepted.
-  add         Record a new decision.
-  diff        Show a unified diff between the DB's decision content and the workbench file.
-  edit        Edit a decision in $EDITOR (editor-first flow).
-  link        Create an entity link from a decision to another entity.
-  list        List decisions.
-  show        Show a decision with body, rationale, status, and linked session.
-  supersede   Mark a decision as superseded by a newer decision.
-  view        View the decision's workbench file in $PAGER.
-  withdraw    Mark a decision as withdrawn.
-
-Flags:
-  -h, --help   help for decision
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar decision [command] --help" for more information about a command.
+error: unknown subcommand [in: decision]
--- go.stderr
+++ zig.stderr
@@ -0,0 +1 @@
+error: UnknownSubcommand
--- exit
+++ exit
-go=0
+zig=1
```

#### 77. `question --json` — invocation `json`

- Failure class: `zig-failed`
- Go exit: `0` (stdout 1608B, stderr 0B)
- Zig exit: `1` (stdout 41B, stderr 25B)

```diff
--- go
+++ zig
@@ -1,33 +1 @@
-Manage questions — open uncertainties surfaced during work.
-
-Status lifecycle: open → answered (via 'question answer') / wontfix.
-
-Usage:
-  planar question [command]
-
-Available Commands:
-  add         Record an open question.
-  answer      Provide an answer to an open question.
-  diff        Show a unified diff between the DB's question content and the workbench file.
-  edit        Edit a question in $EDITOR (editor-first flow).
-  link        Create an entity link from a question to another entity.
-  list        List questions.
-  review      Bulk-review open questions in $EDITOR with originating context.
-  show        Show a question with its body and (if answered) the answer.
-  view        View the question's workbench file in $PAGER.
-  wontfix     Mark a question as not going to be answered.
-
-Flags:
-  -h, --help   help for question
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar question [command] --help" for more information about a command.
+error: unknown subcommand [in: question]
--- go.stderr
+++ zig.stderr
@@ -0,0 +1 @@
+error: UnknownSubcommand
--- exit
+++ exit
-go=0
+zig=1
```

#### 78. `question --json` — invocation `real-cwd-json`

- Failure class: `zig-failed`
- Go exit: `0` (stdout 1608B, stderr 0B)
- Zig exit: `1` (stdout 41B, stderr 25B)
- cwd: `<CWD_DIR>`

```diff
--- go
+++ zig
@@ -1,33 +1 @@
-Manage questions — open uncertainties surfaced during work.
-
-Status lifecycle: open → answered (via 'question answer') / wontfix.
-
-Usage:
-  planar question [command]
-
-Available Commands:
-  add         Record an open question.
-  answer      Provide an answer to an open question.
-  diff        Show a unified diff between the DB's question content and the workbench file.
-  edit        Edit a question in $EDITOR (editor-first flow).
-  link        Create an entity link from a question to another entity.
-  list        List questions.
-  review      Bulk-review open questions in $EDITOR with originating context.
-  show        Show a question with its body and (if answered) the answer.
-  view        View the question's workbench file in $PAGER.
-  wontfix     Mark a question as not going to be answered.
-
-Flags:
-  -h, --help   help for question
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar question [command] --help" for more information about a command.
+error: unknown subcommand [in: question]
--- go.stderr
+++ zig.stderr
@@ -0,0 +1 @@
+error: UnknownSubcommand
--- exit
+++ exit
-go=0
+zig=1
```

#### 79. `links --json` — invocation `json`

- Failure class: `zig-failed`
- Go exit: `0` (stdout 1611B, stderr 0B)
- Zig exit: `1` (stdout 38B, stderr 25B)

```diff
--- go
+++ zig
@@ -1,32 +1 @@
-Manage internal cross-cutting entity relationships stored in entity_links.
-
-Entity links record typed relationships between any two Planar entities
-(e.g. a task cites an artifact, a plan blocks another plan). This domain is
-distinct from the top-level link/unlink commands, which operate on
-external_links (operational plane bindings to Jira, GitHub Issues, etc.).
-Exception: the "update" subcommand mutates the sync_direction column on an
-existing external_links row (it does not touch entity_links).
-
-Usage:
-  planar links [command]
-
-Available Commands:
-  add         Create an entity_links row between two entities.
-  list        List entity_links where the given entity is source or target.
-  remove      Delete an entity_links row by its id.
-  update      Change sync_direction on an existing external_links row.
-
-Flags:
-  -h, --help   help for links
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar links [command] --help" for more information about a command.
+error: unknown subcommand [in: links]
--- go.stderr
+++ zig.stderr
@@ -0,0 +1 @@
+error: UnknownSubcommand
--- exit
+++ exit
-go=0
+zig=1
```

#### 80. `links --json` — invocation `real-cwd-json`

- Failure class: `zig-failed`
- Go exit: `0` (stdout 1611B, stderr 0B)
- Zig exit: `1` (stdout 38B, stderr 25B)
- cwd: `<CWD_DIR>`

```diff
--- go
+++ zig
@@ -1,32 +1 @@
-Manage internal cross-cutting entity relationships stored in entity_links.
-
-Entity links record typed relationships between any two Planar entities
-(e.g. a task cites an artifact, a plan blocks another plan). This domain is
-distinct from the top-level link/unlink commands, which operate on
-external_links (operational plane bindings to Jira, GitHub Issues, etc.).
-Exception: the "update" subcommand mutates the sync_direction column on an
-existing external_links row (it does not touch entity_links).
-
-Usage:
-  planar links [command]
-
-Available Commands:
-  add         Create an entity_links row between two entities.
-  list        List entity_links where the given entity is source or target.
-  remove      Delete an entity_links row by its id.
-  update      Change sync_direction on an existing external_links row.
-
-Flags:
-  -h, --help   help for links
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar links [command] --help" for more information about a command.
+error: unknown subcommand [in: links]
--- go.stderr
+++ zig.stderr
@@ -0,0 +1 @@
+error: UnknownSubcommand
--- exit
+++ exit
-go=0
+zig=1
```

#### 81. `artifact --json` — invocation `json`

- Failure class: `zig-failed`
- Go exit: `0` (stdout 1596B, stderr 0B)
- Zig exit: `1` (stdout 41B, stderr 25B)

```diff
--- go
+++ zig
@@ -1,32 +1 @@
-Manage artifacts — durable documents that crystallize from work.
-
-Kinds: tech_spec, adr, design_note, summary, readme, generated, other, product_spec, roadmap, research, getting_started, changelog_entry, glossary_term.
-Status lifecycle: draft → active → superseded/retired.
-
-Usage:
-  planar artifact [command]
-
-Available Commands:
-  add         Register a new artifact.
-  diff        Show a unified diff between the DB's artifact content and the workbench file.
-  edit        Edit an artifact in $EDITOR (editor-first flow).
-  link        Create an entity link from an artifact to another entity.
-  list        List artifacts.
-  show        Show an artifact's metadata and body.
-  update      Update mutable fields on an artifact.
-  view        View the artifact's workbench file in $PAGER.
-
-Flags:
-  -h, --help   help for artifact
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar artifact [command] --help" for more information about a command.
+error: unknown subcommand [in: artifact]
--- go.stderr
+++ zig.stderr
@@ -0,0 +1 @@
+error: UnknownSubcommand
--- exit
+++ exit
-go=0
+zig=1
```

#### 82. `artifact --json` — invocation `real-cwd-json`

- Failure class: `zig-failed`
- Go exit: `0` (stdout 1596B, stderr 0B)
- Zig exit: `1` (stdout 41B, stderr 25B)
- cwd: `<CWD_DIR>`

```diff
--- go
+++ zig
@@ -1,32 +1 @@
-Manage artifacts — durable documents that crystallize from work.
-
-Kinds: tech_spec, adr, design_note, summary, readme, generated, other, product_spec, roadmap, research, getting_started, changelog_entry, glossary_term.
-Status lifecycle: draft → active → superseded/retired.
-
-Usage:
-  planar artifact [command]
-
-Available Commands:
-  add         Register a new artifact.
-  diff        Show a unified diff between the DB's artifact content and the workbench file.
-  edit        Edit an artifact in $EDITOR (editor-first flow).
-  link        Create an entity link from an artifact to another entity.
-  list        List artifacts.
-  show        Show an artifact's metadata and body.
-  update      Update mutable fields on an artifact.
-  view        View the artifact's workbench file in $PAGER.
-
-Flags:
-  -h, --help   help for artifact
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar artifact [command] --help" for more information about a command.
+error: unknown subcommand [in: artifact]
--- go.stderr
+++ zig.stderr
@@ -0,0 +1 @@
+error: UnknownSubcommand
--- exit
+++ exit
-go=0
+zig=1
```

#### 83. `skills --help` — invocation `help`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 1372B, stderr 0B)
- Zig exit: `0` (stdout 383B, stderr 0B)

```diff
--- go
+++ zig
@@ -1,30 +1,14 @@
+skills
+
 Manage the unified skill source tree.
 
-Skill sources live under skills/src/<slug>.md as Markdown with YAML
-frontmatter. The 'render' subcommand produces the per-vendor output trees
-(commands/claude/, skills/codex/, skills/copilot/) from those sources via
-the renderer in internal/skillrender. The vendor profile table is
-embedded into the binary at src/configs/vendors.yaml.
+  Skill sources live under skills/src/<slug>.md as Markdown with YAML
+  frontmatter. The 'render' subcommand produces the per-vendor output
+  trees (commands/claude/, skills/codex/, skills/copilot/) from those
+  sources.
 
-This subcommand performs no database, network, or operational-plane I/O.
+USAGE:
+  skills <command>
 
-Usage:
-  planar skills [command]
-
-Available Commands:
-  render      Render unified skill sources into per-vendor output trees.
-
-Flags:
-  -h, --help   help for skills
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar skills [command] --help" for more information about a command.
+COMMANDS:
+  render          Render unified skill sources into per-vendor output trees.
```

#### 84. `skills ` — invocation `no-args`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 1372B, stderr 0B)
- Zig exit: `0` (stdout 383B, stderr 0B)

```diff
--- go
+++ zig
@@ -1,30 +1,14 @@
+skills
+
 Manage the unified skill source tree.
 
-Skill sources live under skills/src/<slug>.md as Markdown with YAML
-frontmatter. The 'render' subcommand produces the per-vendor output trees
-(commands/claude/, skills/codex/, skills/copilot/) from those sources via
-the renderer in internal/skillrender. The vendor profile table is
-embedded into the binary at src/configs/vendors.yaml.
+  Skill sources live under skills/src/<slug>.md as Markdown with YAML
+  frontmatter. The 'render' subcommand produces the per-vendor output
+  trees (commands/claude/, skills/codex/, skills/copilot/) from those
+  sources.
 
-This subcommand performs no database, network, or operational-plane I/O.
+USAGE:
+  skills <command>
 
-Usage:
-  planar skills [command]
-
-Available Commands:
-  render      Render unified skill sources into per-vendor output trees.
-
-Flags:
-  -h, --help   help for skills
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar skills [command] --help" for more information about a command.
+COMMANDS:
+  render          Render unified skill sources into per-vendor output trees.
```

#### 85. `skills ` — invocation `real-cwd`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 1372B, stderr 0B)
- Zig exit: `0` (stdout 383B, stderr 0B)
- cwd: `<CWD_DIR>`

```diff
--- go
+++ zig
@@ -1,30 +1,14 @@
+skills
+
 Manage the unified skill source tree.
 
-Skill sources live under skills/src/<slug>.md as Markdown with YAML
-frontmatter. The 'render' subcommand produces the per-vendor output trees
-(commands/claude/, skills/codex/, skills/copilot/) from those sources via
-the renderer in internal/skillrender. The vendor profile table is
-embedded into the binary at src/configs/vendors.yaml.
+  Skill sources live under skills/src/<slug>.md as Markdown with YAML
+  frontmatter. The 'render' subcommand produces the per-vendor output
+  trees (commands/claude/, skills/codex/, skills/copilot/) from those
+  sources.
 
-This subcommand performs no database, network, or operational-plane I/O.
+USAGE:
+  skills <command>
 
-Usage:
-  planar skills [command]
-
-Available Commands:
-  render      Render unified skill sources into per-vendor output trees.
-
-Flags:
-  -h, --help   help for skills
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar skills [command] --help" for more information about a command.
+COMMANDS:
+  render          Render unified skill sources into per-vendor output trees.
```

#### 86. `audit --help` — invocation `help`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 1154B, stderr 0B)
- Zig exit: `0` (stdout 582B, stderr 0B)

```diff
--- go
+++ zig
@@ -1,25 +1,16 @@
+audit
+
 Cross-plane audit trail commands.
 
-Usage:
-  planar audit [command]
+  Subcommands inspect external-link history, recompute decision
+  publication targets, render session timelines, and walk the full
+  audit trail for any external link.
 
-Available Commands:
-  handoff-readiness Check resume-readiness for all in-flight tasks.
-  publish-decision  Post the decision body to linked operational-plane targets.
-  session           Show the full timeline of a session.
-  trail             Show the full audit trail for an external link.
+USAGE:
+  audit <command>
 
-Flags:
-  -h, --help   help for audit
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar audit [command] --help" for more information about a command.
+COMMANDS:
+  trail           Show audit history for an entity (audit_log + entity_links) or an external link (external_links + sync_events).
+  session         Show the timeline for a session.
+  publish-decision  Post the decision body to linked operational-plane targets (M8).
+  handoff-readiness  Check resume-readiness for all in-flight tasks.
```

#### 87. `audit ` — invocation `no-args`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 1154B, stderr 0B)
- Zig exit: `0` (stdout 582B, stderr 0B)

```diff
--- go
+++ zig
@@ -1,25 +1,16 @@
+audit
+
 Cross-plane audit trail commands.
 
-Usage:
-  planar audit [command]
+  Subcommands inspect external-link history, recompute decision
+  publication targets, render session timelines, and walk the full
+  audit trail for any external link.
 
-Available Commands:
-  handoff-readiness Check resume-readiness for all in-flight tasks.
-  publish-decision  Post the decision body to linked operational-plane targets.
-  session           Show the full timeline of a session.
-  trail             Show the full audit trail for an external link.
+USAGE:
+  audit <command>
 
-Flags:
-  -h, --help   help for audit
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar audit [command] --help" for more information about a command.
+COMMANDS:
+  trail           Show audit history for an entity (audit_log + entity_links) or an external link (external_links + sync_events).
+  session         Show the timeline for a session.
+  publish-decision  Post the decision body to linked operational-plane targets (M8).
+  handoff-readiness  Check resume-readiness for all in-flight tasks.
```

#### 88. `audit ` — invocation `real-cwd`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 1154B, stderr 0B)
- Zig exit: `0` (stdout 582B, stderr 0B)
- cwd: `<CWD_DIR>`

```diff
--- go
+++ zig
@@ -1,25 +1,16 @@
+audit
+
 Cross-plane audit trail commands.
 
-Usage:
-  planar audit [command]
+  Subcommands inspect external-link history, recompute decision
+  publication targets, render session timelines, and walk the full
+  audit trail for any external link.
 
-Available Commands:
-  handoff-readiness Check resume-readiness for all in-flight tasks.
-  publish-decision  Post the decision body to linked operational-plane targets.
-  session           Show the full timeline of a session.
-  trail             Show the full audit trail for an external link.
+USAGE:
+  audit <command>
 
-Flags:
-  -h, --help   help for audit
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar audit [command] --help" for more information about a command.
+COMMANDS:
+  trail           Show audit history for an entity (audit_log + entity_links) or an external link (external_links + sync_events).
+  session         Show the timeline for a session.
+  publish-decision  Post the decision body to linked operational-plane targets (M8).
+  handoff-readiness  Check resume-readiness for all in-flight tasks.
```

#### 89. `workspace --json` — invocation `json`

- Failure class: `zig-failed`
- Go exit: `0` (stdout 1561B, stderr 0B)
- Zig exit: `1` (stdout 42B, stderr 25B)

```diff
--- go
+++ zig
@@ -1,32 +1 @@
-Workspace administration.
-
-A workspace is identified by an associations row of kind=org. Each
-workspace owns a state directory under
-${PLANAR_HOME:-~/.planar}/workspaces/<org_id>/ holding the canonical
-AGENTS.md, routing-table.json, config.toml, and README.md. Symlinks
-(AGENTS.md and CLAUDE.md) at the workspace root project that content
-into the directory where agents expect to find it.
-
-Usage:
-  planar workspace [command]
-
-Available Commands:
-  doctor      Detect and repair workspace state directories and symlinks.
-  init        Initialize the current directory as a workspace and register each child repo as a project.
-  regenerate  Render AGENTS.md for the workspace from the routing table.
-  routing     Build or inspect the workspace routing table.
-
-Flags:
-  -h, --help   help for workspace
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar workspace [command] --help" for more information about a command.
+error: unknown subcommand [in: workspace]
--- go.stderr
+++ zig.stderr
@@ -0,0 +1 @@
+error: UnknownSubcommand
--- exit
+++ exit
-go=0
+zig=1
```

#### 90. `workspace --json` — invocation `real-cwd-json`

- Failure class: `zig-failed`
- Go exit: `0` (stdout 1561B, stderr 0B)
- Zig exit: `1` (stdout 42B, stderr 25B)
- cwd: `<CWD_DIR>`

```diff
--- go
+++ zig
@@ -1,32 +1 @@
-Workspace administration.
-
-A workspace is identified by an associations row of kind=org. Each
-workspace owns a state directory under
-${PLANAR_HOME:-~/.planar}/workspaces/<org_id>/ holding the canonical
-AGENTS.md, routing-table.json, config.toml, and README.md. Symlinks
-(AGENTS.md and CLAUDE.md) at the workspace root project that content
-into the directory where agents expect to find it.
-
-Usage:
-  planar workspace [command]
-
-Available Commands:
-  doctor      Detect and repair workspace state directories and symlinks.
-  init        Initialize the current directory as a workspace and register each child repo as a project.
-  regenerate  Render AGENTS.md for the workspace from the routing table.
-  routing     Build or inspect the workspace routing table.
-
-Flags:
-  -h, --help   help for workspace
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar workspace [command] --help" for more information about a command.
+error: unknown subcommand [in: workspace]
--- go.stderr
+++ zig.stderr
@@ -0,0 +1 @@
+error: UnknownSubcommand
--- exit
+++ exit
-go=0
+zig=1
```

#### 91. `config --json` — invocation `json`

- Failure class: `zig-failed`
- Go exit: `0` (stdout 1519B, stderr 0B)
- Zig exit: `1` (stdout 39B, stderr 25B)

```diff
--- go
+++ zig
@@ -1,35 +1 @@
-Read, inspect, and validate the Planar configuration file.
-
-The configuration file lives at ~/.planar/config.toml by default.
-Set $PLANAR_CONFIG_PATH to use a different path.
-
-Resolution order (highest to lowest priority):
-  1. Environment variables
-  2. Per-association overrides ([associations."<slug>"] sections)
-  3. Top-level config file values
-  4. Embedded defaults (shipped with the binary)
-
-Usage:
-  planar config [command]
-
-Available Commands:
-  edit        Open the config file in $EDITOR.
-  init        Idempotently write a starter config.toml if absent.
-  path        Print the resolved config file path.
-  show        Print the resolved configuration.
-  validate    Validate the config file syntax and schema.
-
-Flags:
-  -h, --help   help for config
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar config [command] --help" for more information about a command.
+error: unknown subcommand [in: config]
--- go.stderr
+++ zig.stderr
@@ -0,0 +1 @@
+error: UnknownSubcommand
--- exit
+++ exit
-go=0
+zig=1
```

#### 92. `config --json` — invocation `real-cwd-json`

- Failure class: `zig-failed`
- Go exit: `0` (stdout 1519B, stderr 0B)
- Zig exit: `1` (stdout 39B, stderr 25B)
- cwd: `<CWD_DIR>`

```diff
--- go
+++ zig
@@ -1,35 +1 @@
-Read, inspect, and validate the Planar configuration file.
-
-The configuration file lives at ~/.planar/config.toml by default.
-Set $PLANAR_CONFIG_PATH to use a different path.
-
-Resolution order (highest to lowest priority):
-  1. Environment variables
-  2. Per-association overrides ([associations."<slug>"] sections)
-  3. Top-level config file values
-  4. Embedded defaults (shipped with the binary)
-
-Usage:
-  planar config [command]
-
-Available Commands:
-  edit        Open the config file in $EDITOR.
-  init        Idempotently write a starter config.toml if absent.
-  path        Print the resolved config file path.
-  show        Print the resolved configuration.
-  validate    Validate the config file syntax and schema.
-
-Flags:
-  -h, --help   help for config
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar config [command] --help" for more information about a command.
+error: unknown subcommand [in: config]
--- go.stderr
+++ zig.stderr
@@ -0,0 +1 @@
+error: UnknownSubcommand
--- exit
+++ exit
-go=0
+zig=1
```

#### 93. `assoc --json` — invocation `json`

- Failure class: `zig-failed`
- Go exit: `0` (stdout 1464B, stderr 0B)
- Zig exit: `1` (stdout 38B, stderr 25B)

```diff
--- go
+++ zig
@@ -1,35 +1 @@
-Manage associations — the many-to-many tags that group repos into named scopes.
-
-Both 'assoc' and 'association' are valid subcommand names.
-
-User-creatable kinds: org, project, client, personal, ad-hoc.
-Auto-detected kinds (via 'assoc detect'): host, path, lang.
-
-Usage:
-  planar assoc [command]
-
-Aliases:
-  assoc, association
-
-Available Commands:
-  add         Add a repo to an association.
-  create      Create a new association.
-  detect      Propose (or apply) auto-detected associations for the current directory.
-  list        List all known associations.
-  members     List all project members of an association.
-  remove      Remove a repo from an association.
-
-Flags:
-  -h, --help   help for assoc
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar assoc [command] --help" for more information about a command.
+error: unknown subcommand [in: assoc]
--- go.stderr
+++ zig.stderr
@@ -0,0 +1 @@
+error: UnknownSubcommand
--- exit
+++ exit
-go=0
+zig=1
```

#### 94. `assoc --json` — invocation `real-cwd-json`

- Failure class: `zig-failed`
- Go exit: `0` (stdout 1464B, stderr 0B)
- Zig exit: `1` (stdout 38B, stderr 25B)
- cwd: `<CWD_DIR>`

```diff
--- go
+++ zig
@@ -1,35 +1 @@
-Manage associations — the many-to-many tags that group repos into named scopes.
-
-Both 'assoc' and 'association' are valid subcommand names.
-
-User-creatable kinds: org, project, client, personal, ad-hoc.
-Auto-detected kinds (via 'assoc detect'): host, path, lang.
-
-Usage:
-  planar assoc [command]
-
-Aliases:
-  assoc, association
-
-Available Commands:
-  add         Add a repo to an association.
-  create      Create a new association.
-  detect      Propose (or apply) auto-detected associations for the current directory.
-  list        List all known associations.
-  members     List all project members of an association.
-  remove      Remove a repo from an association.
-
-Flags:
-  -h, --help   help for assoc
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar assoc [command] --help" for more information about a command.
+error: unknown subcommand [in: assoc]
--- go.stderr
+++ zig.stderr
@@ -0,0 +1 @@
+error: UnknownSubcommand
--- exit
+++ exit
-go=0
+zig=1
```

#### 95. `capture --json` — invocation `json`

- Failure class: `zig-failed`
- Go exit: `0` (stdout 1466B, stderr 0B)
- Zig exit: `1` (stdout 40B, stderr 25B)

```diff
--- go
+++ zig
@@ -1,30 +1 @@
-Capture commands manage explicit session management and context capture.
-
-Automatic capture happens on every write command; use these subcommands for
-explicit session management, narrative notes, command history, and snapshots.
-
-Usage:
-  planar capture [command]
-
-Available Commands:
-  command     Record a command that was run.
-  end         Close the current (or specified) session.
-  file        Record a file that was touched during the session.
-  note        Append a note entry to the current session.
-  session     Open or reuse a session for the current (vendor, vendor-session-id) tuple.
-  snapshot    Produce a context snapshot for the current or named task.
-
-Flags:
-  -h, --help   help for capture
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar capture [command] --help" for more information about a command.
+error: unknown subcommand [in: capture]
--- go.stderr
+++ zig.stderr
@@ -0,0 +1 @@
+error: UnknownSubcommand
--- exit
+++ exit
-go=0
+zig=1
```

#### 96. `capture --json` — invocation `real-cwd-json`

- Failure class: `zig-failed`
- Go exit: `0` (stdout 1466B, stderr 0B)
- Zig exit: `1` (stdout 40B, stderr 25B)
- cwd: `<CWD_DIR>`

```diff
--- go
+++ zig
@@ -1,30 +1 @@
-Capture commands manage explicit session management and context capture.
-
-Automatic capture happens on every write command; use these subcommands for
-explicit session management, narrative notes, command history, and snapshots.
-
-Usage:
-  planar capture [command]
-
-Available Commands:
-  command     Record a command that was run.
-  end         Close the current (or specified) session.
-  file        Record a file that was touched during the session.
-  note        Append a note entry to the current session.
-  session     Open or reuse a session for the current (vendor, vendor-session-id) tuple.
-  snapshot    Produce a context snapshot for the current or named task.
-
-Flags:
-  -h, --help   help for capture
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar capture [command] --help" for more information about a command.
+error: unknown subcommand [in: capture]
--- go.stderr
+++ zig.stderr
@@ -0,0 +1 @@
+error: UnknownSubcommand
--- exit
+++ exit
-go=0
+zig=1
```

#### 97. `plan --json` — invocation `json`

- Failure class: `zig-failed`
- Go exit: `0` (stdout 1454B, stderr 0B)
- Zig exit: `1` (stdout 37B, stderr 25B)

```diff
--- go
+++ zig
@@ -1,31 +1 @@
-Manage plans — the top-level structured intent for a body of work.
-
-Plans may be hierarchical (--parent) and contain ordered steps (plan step add).
-Status lifecycle: draft → active → paused / done / abandoned.
-
-Usage:
-  planar plan [command]
-
-Available Commands:
-  create           Create a new plan.
-  link             Create an entity link from a plan to another entity.
-  list             List plans.
-  recompute-status Re-fire the plan-status auto-promotion invariant against a plan or all plans.
-  show             Show a plan's details, steps, and child plans.
-  step             Manage plan steps.
-  update           Update mutable fields on a plan.
-
-Flags:
-  -h, --help   help for plan
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar plan [command] --help" for more information about a command.
+error: unknown subcommand [in: plan]
--- go.stderr
+++ zig.stderr
@@ -0,0 +1 @@
+error: UnknownSubcommand
--- exit
+++ exit
-go=0
+zig=1
```

#### 98. `plan --json` — invocation `real-cwd-json`

- Failure class: `zig-failed`
- Go exit: `0` (stdout 1454B, stderr 0B)
- Zig exit: `1` (stdout 37B, stderr 25B)
- cwd: `<CWD_DIR>`

```diff
--- go
+++ zig
@@ -1,31 +1 @@
-Manage plans — the top-level structured intent for a body of work.
-
-Plans may be hierarchical (--parent) and contain ordered steps (plan step add).
-Status lifecycle: draft → active → paused / done / abandoned.
-
-Usage:
-  planar plan [command]
-
-Available Commands:
-  create           Create a new plan.
-  link             Create an entity link from a plan to another entity.
-  list             List plans.
-  recompute-status Re-fire the plan-status auto-promotion invariant against a plan or all plans.
-  show             Show a plan's details, steps, and child plans.
-  step             Manage plan steps.
-  update           Update mutable fields on a plan.
-
-Flags:
-  -h, --help   help for plan
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar plan [command] --help" for more information about a command.
+error: unknown subcommand [in: plan]
--- go.stderr
+++ zig.stderr
@@ -0,0 +1 @@
+error: UnknownSubcommand
--- exit
+++ exit
-go=0
+zig=1
```

#### 99. `plan next 351` — invocation `q233-plan-next`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 1454B, stderr 0B)
- Zig exit: `0` (stdout 86B, stderr 0B)

```diff
--- go
+++ zig
@@ -1,31 +1 @@
-Manage plans — the top-level structured intent for a body of work.
-
-Plans may be hierarchical (--parent) and contain ordered steps (plan step add).
-Status lifecycle: draft → active → paused / done / abandoned.
-
-Usage:
-  planar plan [command]
-
-Available Commands:
-  create           Create a new plan.
-  link             Create an entity link from a plan to another entity.
-  list             List plans.
-  recompute-status Re-fire the plan-status auto-promotion invariant against a plan or all plans.
-  show             Show a plan's details, steps, and child plans.
-  step             Manage plan steps.
-  update           Update mutable fields on a plan.
-
-Flags:
-  -h, --help   help for plan
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar plan [command] --help" for more information about a command.
+task:2369  Phase 2 audit: python3 deviation from 'pure bash 3.2' spec  [todo, pri:25]
```

#### 100. `promote --help` — invocation `help`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 1080B, stderr 0B)
- Zig exit: `0` (stdout 504B, stderr 0B)

```diff
--- go
+++ zig
@@ -1,24 +1,20 @@
+promote
+
 Promote an entity from its current scope to a named association.
 
-Valid entity kinds: plan, task, question, test_scenario (alias: scenario), artifact, decision.
+  Valid entity kinds: plan, task, question, test_scenario (alias:
+  scenario), artifact, decision.
 
-Examples:
-  planar promote task:42 --to org:acme
-  planar promote plan:7  --to project:billing
+  Examples:
+    planar promote task:42 --to org:acme
+    planar promote plan:7 --to project:planar
 
-Usage:
-  planar promote <kind:id> [flags]
+USAGE:
+  promote [flags] <ref>
 
-Flags:
-  -h, --help        help for promote
-      --to string   Target association slug (required)
+FLAGS:
+  --to                  (string) required — Target association slug
+  --json                (bool) default=false
 
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
+POSITIONAL ARGUMENTS:
+  <ref>           (string) — Entity ref (kind:id)
```

#### 101. `test-spec --help` — invocation `help`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 1189B, stderr 0B)
- Zig exit: `0` (stdout 392B, stderr 0B)

```diff
--- go
+++ zig
@@ -1,26 +1,13 @@
+test-spec
+
 Commands for inspecting test-spec coverage of a plan's tasks.
 
-'test-spec status' prints a per-milestone breakdown of which tasks have
-verifying scenarios. This is a read-only complement to the ingest-time
-coverage gate (see `planar spec ingest --strict`).
+  'test-spec status' prints a per-milestone breakdown of which tasks
+  have verifying scenarios. This is a read-only complement to the
+  ingest-time coverage gate (see `planar spec ingest --strict`).
 
-Usage:
-  planar test-spec [command]
+USAGE:
+  test-spec <command>
 
-Available Commands:
-  status      Print per-milestone test-spec coverage for an anchor plan.
-
-Flags:
-  -h, --help   help for test-spec
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar test-spec [command] --help" for more information about a command.
+COMMANDS:
+  status          Print per-milestone test-spec coverage for an anchor plan.
```

#### 102. `test-spec ` — invocation `no-args`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 1189B, stderr 0B)
- Zig exit: `0` (stdout 392B, stderr 0B)

```diff
--- go
+++ zig
@@ -1,26 +1,13 @@
+test-spec
+
 Commands for inspecting test-spec coverage of a plan's tasks.
 
-'test-spec status' prints a per-milestone breakdown of which tasks have
-verifying scenarios. This is a read-only complement to the ingest-time
-coverage gate (see `planar spec ingest --strict`).
+  'test-spec status' prints a per-milestone breakdown of which tasks
+  have verifying scenarios. This is a read-only complement to the
+  ingest-time coverage gate (see `planar spec ingest --strict`).
 
-Usage:
-  planar test-spec [command]
+USAGE:
+  test-spec <command>
 
-Available Commands:
-  status      Print per-milestone test-spec coverage for an anchor plan.
-
-Flags:
-  -h, --help   help for test-spec
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar test-spec [command] --help" for more information about a command.
+COMMANDS:
+  status          Print per-milestone test-spec coverage for an anchor plan.
```

#### 103. `test-spec ` — invocation `real-cwd`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 1189B, stderr 0B)
- Zig exit: `0` (stdout 392B, stderr 0B)
- cwd: `<CWD_DIR>`

```diff
--- go
+++ zig
@@ -1,26 +1,13 @@
+test-spec
+
 Commands for inspecting test-spec coverage of a plan's tasks.
 
-'test-spec status' prints a per-milestone breakdown of which tasks have
-verifying scenarios. This is a read-only complement to the ingest-time
-coverage gate (see `planar spec ingest --strict`).
+  'test-spec status' prints a per-milestone breakdown of which tasks
+  have verifying scenarios. This is a read-only complement to the
+  ingest-time coverage gate (see `planar spec ingest --strict`).
 
-Usage:
-  planar test-spec [command]
+USAGE:
+  test-spec <command>
 
-Available Commands:
-  status      Print per-milestone test-spec coverage for an anchor plan.
-
-Flags:
-  -h, --help   help for test-spec
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar test-spec [command] --help" for more information about a command.
+COMMANDS:
+  status          Print per-milestone test-spec coverage for an anchor plan.
```

#### 104. `scope --json` — invocation `json`

- Failure class: `zig-failed`
- Go exit: `0` (stdout 1375B, stderr 0B)
- Zig exit: `1` (stdout 38B, stderr 25B)

```diff
--- go
+++ zig
@@ -1,28 +1 @@
-Inspect the scope Planar will resolve for the current working directory.
-Plan 153 removed the active scope stack; scope is now derived from cwd and
-overridden by passing --scope <slug> to individual verbs.
-
-Usage:
-  planar scope [command]
-
-Available Commands:
-  clear       Removed in plan 153 M5 — see `planar scope show`.
-  pop         Removed in plan 153 M5 — see `planar scope show`.
-  show        Show the cwd-derived scope (and any --scope override).
-  suggest     Suggest scope associations based on cwd.
-  use         Removed in plan 153 M5 — see `planar scope show`.
-
-Flags:
-  -h, --help   help for scope
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar scope [command] --help" for more information about a command.
+error: unknown subcommand [in: scope]
--- go.stderr
+++ zig.stderr
@@ -0,0 +1 @@
+error: UnknownSubcommand
--- exit
+++ exit
-go=0
+zig=1
```

#### 105. `scope --json` — invocation `real-cwd-json`

- Failure class: `zig-failed`
- Go exit: `0` (stdout 1375B, stderr 0B)
- Zig exit: `1` (stdout 38B, stderr 25B)
- cwd: `<CWD_DIR>`

```diff
--- go
+++ zig
@@ -1,28 +1 @@
-Inspect the scope Planar will resolve for the current working directory.
-Plan 153 removed the active scope stack; scope is now derived from cwd and
-overridden by passing --scope <slug> to individual verbs.
-
-Usage:
-  planar scope [command]
-
-Available Commands:
-  clear       Removed in plan 153 M5 — see `planar scope show`.
-  pop         Removed in plan 153 M5 — see `planar scope show`.
-  show        Show the cwd-derived scope (and any --scope override).
-  suggest     Suggest scope associations based on cwd.
-  use         Removed in plan 153 M5 — see `planar scope show`.
-
-Flags:
-  -h, --help   help for scope
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar scope [command] --help" for more information about a command.
+error: unknown subcommand [in: scope]
--- go.stderr
+++ zig.stderr
@@ -0,0 +1 @@
+error: UnknownSubcommand
--- exit
+++ exit
-go=0
+zig=1
```

#### 106. `skills --json` — invocation `json`

- Failure class: `zig-failed`
- Go exit: `0` (stdout 1372B, stderr 0B)
- Zig exit: `1` (stdout 39B, stderr 25B)

```diff
--- go
+++ zig
@@ -1,30 +1 @@
-Manage the unified skill source tree.
-
-Skill sources live under skills/src/<slug>.md as Markdown with YAML
-frontmatter. The 'render' subcommand produces the per-vendor output trees
-(commands/claude/, skills/codex/, skills/copilot/) from those sources via
-the renderer in internal/skillrender. The vendor profile table is
-embedded into the binary at src/configs/vendors.yaml.
-
-This subcommand performs no database, network, or operational-plane I/O.
-
-Usage:
-  planar skills [command]
-
-Available Commands:
-  render      Render unified skill sources into per-vendor output trees.
-
-Flags:
-  -h, --help   help for skills
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar skills [command] --help" for more information about a command.
+error: unknown subcommand [in: skills]
--- go.stderr
+++ zig.stderr
@@ -0,0 +1 @@
+error: UnknownSubcommand
--- exit
+++ exit
-go=0
+zig=1
```

#### 107. `skills --json` — invocation `real-cwd-json`

- Failure class: `zig-failed`
- Go exit: `0` (stdout 1372B, stderr 0B)
- Zig exit: `1` (stdout 39B, stderr 25B)
- cwd: `<CWD_DIR>`

```diff
--- go
+++ zig
@@ -1,30 +1 @@
-Manage the unified skill source tree.
-
-Skill sources live under skills/src/<slug>.md as Markdown with YAML
-frontmatter. The 'render' subcommand produces the per-vendor output trees
-(commands/claude/, skills/codex/, skills/copilot/) from those sources via
-the renderer in internal/skillrender. The vendor profile table is
-embedded into the binary at src/configs/vendors.yaml.
-
-This subcommand performs no database, network, or operational-plane I/O.
-
-Usage:
-  planar skills [command]
-
-Available Commands:
-  render      Render unified skill sources into per-vendor output trees.
-
-Flags:
-  -h, --help   help for skills
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar skills [command] --help" for more information about a command.
+error: unknown subcommand [in: skills]
--- go.stderr
+++ zig.stderr
@@ -0,0 +1 @@
+error: UnknownSubcommand
--- exit
+++ exit
-go=0
+zig=1
```

#### 108. `demote --help` — invocation `help`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 1046B, stderr 0B)
- Zig exit: `0` (stdout 451B, stderr 0B)

```diff
--- go
+++ zig
@@ -1,24 +1,19 @@
+demote
+
 Reverse a promotion — move an entity back to global personal scope.
 
-Only "global" is accepted as a target. Association-to-association transitions
-go through promote.
+  Only "global" is accepted as a target. Association-to-association
+  transitions go through promote.
 
-Example:
-  planar demote task:42 --to global
+  Example:
+    planar demote task:42 --to global
 
-Usage:
-  planar demote <kind:id> [flags]
+USAGE:
+  demote [flags] <ref>
 
-Flags:
-  -h, --help        help for demote
-      --to string   Demotion target; must be "global" (required)
+FLAGS:
+  --from                (string) — Source association slug
+  --json                (bool) default=false
 
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
+POSITIONAL ARGUMENTS:
+  <ref>           (string) — Entity ref (kind:id)
```

#### 109. `ext --json` — invocation `json`

- Failure class: `zig-failed`
- Go exit: `0` (stdout 1289B, stderr 0B)
- Zig exit: `1` (stdout 36B, stderr 25B)

```diff
--- go
+++ zig
@@ -1,28 +1 @@
-Register and interact with external systems on the operational plane.
-
-Sub-commands: register, list, test, create, propagate.
-
-Usage:
-  planar ext [command]
-
-Available Commands:
-  create      Create an external counterpart for a local entity.
-  list        List all registered external systems.
-  propagate   Propagate a feature (anchor plan + descendants) to a registered external system.
-  register    Register an external system.
-  test        Verify auth and reachability for an external system.
-
-Flags:
-  -h, --help   help for ext
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar ext [command] --help" for more information about a command.
+error: unknown subcommand [in: ext]
--- go.stderr
+++ zig.stderr
@@ -0,0 +1 @@
+error: UnknownSubcommand
--- exit
+++ exit
-go=0
+zig=1
```

#### 110. `ext --json` — invocation `real-cwd-json`

- Failure class: `zig-failed`
- Go exit: `0` (stdout 1289B, stderr 0B)
- Zig exit: `1` (stdout 36B, stderr 25B)
- cwd: `<CWD_DIR>`

```diff
--- go
+++ zig
@@ -1,28 +1 @@
-Register and interact with external systems on the operational plane.
-
-Sub-commands: register, list, test, create, propagate.
-
-Usage:
-  planar ext [command]
-
-Available Commands:
-  create      Create an external counterpart for a local entity.
-  list        List all registered external systems.
-  propagate   Propagate a feature (anchor plan + descendants) to a registered external system.
-  register    Register an external system.
-  test        Verify auth and reachability for an external system.
-
-Flags:
-  -h, --help   help for ext
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar ext [command] --help" for more information about a command.
+error: unknown subcommand [in: ext]
--- go.stderr
+++ zig.stderr
@@ -0,0 +1 @@
+error: UnknownSubcommand
--- exit
+++ exit
-go=0
+zig=1
```

#### 111. `unlink --help` — invocation `help`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 1028B, stderr 0B)
- Zig exit: `0` (stdout 376B, stderr 0B)

```diff
--- go
+++ zig
@@ -1,19 +1,15 @@
-Remove an external_links row by its link id. Associated sync_events rows are
-also removed (cascade).
+unlink
 
-Usage:
-  planar unlink <link-id> [flags]
+Remove an external_links row by its link id.
 
-Flags:
-  -h, --help           help for unlink
-      --scope string   Scope for the cross-scope guard: global, repo, repo:<slug>, or assoc:<slug>. When omitted, scope is resolved from cwd or active stack.
+  Associated sync_events rows are also removed (cascade).
 
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
+USAGE:
+  unlink [flags] <link-id>
+
+FLAGS:
+  --scope               (string) — Scope for the cross-scope guard (currently informational)
+  --json                (bool) default=false
+
+POSITIONAL ARGUMENTS:
+  <link-id>       (string) — External-link id (integer)
```

#### 112. `health --help` — invocation `help`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 1039B, stderr 0B)
- Zig exit: `0` (stdout 355B, stderr 0B)

```diff
--- go
+++ zig
@@ -1,23 +1,16 @@
-Check database reachability, schema version currency, SQLite integrity,
-in-flight task resumability, and pending handoff staleness.
+health
 
-Exit codes:
-  0  all checks pass
-  1  degraded (some tasks not resumable or stale handoffs)
-  2  critical (database unreachable or integrity check failed)
+Check database reachability, schema version currency, SQLite
+  integrity, in-flight task resumability, and pending handoff
+  staleness.
 
-Usage:
-  planar health [flags]
+  Exit codes:
+    0  all checks pass
+    1  degraded (in-flight tasks not resumable, stale handoffs,
+       integrity errors, etc.)
 
-Flags:
-  -h, --help   help for health
+USAGE:
+  health [flags]
 
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
+FLAGS:
+  --json                (bool) default=false
```

#### 113. `spec --help` — invocation `help`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 1064B, stderr 0B)
- Zig exit: `0` (stdout 341B, stderr 0B)

```diff
--- go
+++ zig
@@ -1,25 +1,13 @@
+spec
+
 Commands for the planning pipeline spec surface.
 
-'spec ingest' decomposes workbench planning documents into a structured task
-graph in the database.
+  'spec ingest' decomposes workbench planning documents into a
+  structured task graph in the database.
+  'spec draft' generates initial spec artifacts from a goal statement.
 
-Usage:
-  planar spec [command]
+USAGE:
+  spec <command>
 
-Available Commands:
-  ingest      Decompose workbench spec documents into the task graph.
-
-Flags:
-  -h, --help   help for spec
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar spec [command] --help" for more information about a command.
+COMMANDS:
+  ingest          Decompose workbench spec documents into the task graph.
```

#### 114. `spec ` — invocation `no-args`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 1064B, stderr 0B)
- Zig exit: `0` (stdout 341B, stderr 0B)

```diff
--- go
+++ zig
@@ -1,25 +1,13 @@
+spec
+
 Commands for the planning pipeline spec surface.
 
-'spec ingest' decomposes workbench planning documents into a structured task
-graph in the database.
+  'spec ingest' decomposes workbench planning documents into a
+  structured task graph in the database.
+  'spec draft' generates initial spec artifacts from a goal statement.
 
-Usage:
-  planar spec [command]
+USAGE:
+  spec <command>
 
-Available Commands:
-  ingest      Decompose workbench spec documents into the task graph.
-
-Flags:
-  -h, --help   help for spec
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar spec [command] --help" for more information about a command.
+COMMANDS:
+  ingest          Decompose workbench spec documents into the task graph.
```

#### 115. `spec ` — invocation `real-cwd`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 1064B, stderr 0B)
- Zig exit: `0` (stdout 341B, stderr 0B)
- cwd: `<CWD_DIR>`

```diff
--- go
+++ zig
@@ -1,25 +1,13 @@
+spec
+
 Commands for the planning pipeline spec surface.
 
-'spec ingest' decomposes workbench planning documents into a structured task
-graph in the database.
+  'spec ingest' decomposes workbench planning documents into a
+  structured task graph in the database.
+  'spec draft' generates initial spec artifacts from a goal statement.
 
-Usage:
-  planar spec [command]
+USAGE:
+  spec <command>
 
-Available Commands:
-  ingest      Decompose workbench spec documents into the task graph.
-
-Flags:
-  -h, --help   help for spec
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar spec [command] --help" for more information about a command.
+COMMANDS:
+  ingest          Decompose workbench spec documents into the task graph.
```

#### 116. `test-spec --json` — invocation `json`

- Failure class: `zig-failed`
- Go exit: `0` (stdout 1189B, stderr 0B)
- Zig exit: `1` (stdout 42B, stderr 25B)

```diff
--- go
+++ zig
@@ -1,26 +1 @@
-Commands for inspecting test-spec coverage of a plan's tasks.
-
-'test-spec status' prints a per-milestone breakdown of which tasks have
-verifying scenarios. This is a read-only complement to the ingest-time
-coverage gate (see `planar spec ingest --strict`).
-
-Usage:
-  planar test-spec [command]
-
-Available Commands:
-  status      Print per-milestone test-spec coverage for an anchor plan.
-
-Flags:
-  -h, --help   help for test-spec
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar test-spec [command] --help" for more information about a command.
+error: unknown subcommand [in: test-spec]
--- go.stderr
+++ zig.stderr
@@ -0,0 +1 @@
+error: UnknownSubcommand
--- exit
+++ exit
-go=0
+zig=1
```

#### 117. `test-spec --json` — invocation `real-cwd-json`

- Failure class: `zig-failed`
- Go exit: `0` (stdout 1189B, stderr 0B)
- Zig exit: `1` (stdout 42B, stderr 25B)
- cwd: `<CWD_DIR>`

```diff
--- go
+++ zig
@@ -1,26 +1 @@
-Commands for inspecting test-spec coverage of a plan's tasks.
-
-'test-spec status' prints a per-milestone breakdown of which tasks have
-verifying scenarios. This is a read-only complement to the ingest-time
-coverage gate (see `planar spec ingest --strict`).
-
-Usage:
-  planar test-spec [command]
-
-Available Commands:
-  status      Print per-milestone test-spec coverage for an anchor plan.
-
-Flags:
-  -h, --help   help for test-spec
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar test-spec [command] --help" for more information about a command.
+error: unknown subcommand [in: test-spec]
--- go.stderr
+++ zig.stderr
@@ -0,0 +1 @@
+error: UnknownSubcommand
--- exit
+++ exit
-go=0
+zig=1
```

#### 118. `audit --json` — invocation `json`

- Failure class: `zig-failed`
- Go exit: `0` (stdout 1154B, stderr 0B)
- Zig exit: `1` (stdout 38B, stderr 25B)

```diff
--- go
+++ zig
@@ -1,25 +1 @@
-Cross-plane audit trail commands.
-
-Usage:
-  planar audit [command]
-
-Available Commands:
-  handoff-readiness Check resume-readiness for all in-flight tasks.
-  publish-decision  Post the decision body to linked operational-plane targets.
-  session           Show the full timeline of a session.
-  trail             Show the full audit trail for an external link.
-
-Flags:
-  -h, --help   help for audit
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar audit [command] --help" for more information about a command.
+error: unknown subcommand [in: audit]
--- go.stderr
+++ zig.stderr
@@ -0,0 +1 @@
+error: UnknownSubcommand
--- exit
+++ exit
-go=0
+zig=1
```

#### 119. `audit --json` — invocation `real-cwd-json`

- Failure class: `zig-failed`
- Go exit: `0` (stdout 1154B, stderr 0B)
- Zig exit: `1` (stdout 38B, stderr 25B)
- cwd: `<CWD_DIR>`

```diff
--- go
+++ zig
@@ -1,25 +1 @@
-Cross-plane audit trail commands.
-
-Usage:
-  planar audit [command]
-
-Available Commands:
-  handoff-readiness Check resume-readiness for all in-flight tasks.
-  publish-decision  Post the decision body to linked operational-plane targets.
-  session           Show the full timeline of a session.
-  trail             Show the full audit trail for an external link.
-
-Flags:
-  -h, --help   help for audit
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar audit [command] --help" for more information about a command.
+error: unknown subcommand [in: audit]
--- go.stderr
+++ zig.stderr
@@ -0,0 +1 @@
+error: UnknownSubcommand
--- exit
+++ exit
-go=0
+zig=1
```

#### 120. `spec --json` — invocation `json`

- Failure class: `zig-failed`
- Go exit: `0` (stdout 1064B, stderr 0B)
- Zig exit: `1` (stdout 37B, stderr 25B)

```diff
--- go
+++ zig
@@ -1,25 +1 @@
-Commands for the planning pipeline spec surface.
-
-'spec ingest' decomposes workbench planning documents into a structured task
-graph in the database.
-
-Usage:
-  planar spec [command]
-
-Available Commands:
-  ingest      Decompose workbench spec documents into the task graph.
-
-Flags:
-  -h, --help   help for spec
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar spec [command] --help" for more information about a command.
+error: unknown subcommand [in: spec]
--- go.stderr
+++ zig.stderr
@@ -0,0 +1 @@
+error: UnknownSubcommand
--- exit
+++ exit
-go=0
+zig=1
```

#### 121. `spec --json` — invocation `real-cwd-json`

- Failure class: `zig-failed`
- Go exit: `0` (stdout 1064B, stderr 0B)
- Zig exit: `1` (stdout 37B, stderr 25B)
- cwd: `<CWD_DIR>`

```diff
--- go
+++ zig
@@ -1,25 +1 @@
-Commands for the planning pipeline spec surface.
-
-'spec ingest' decomposes workbench planning documents into a structured task
-graph in the database.
-
-Usage:
-  planar spec [command]
-
-Available Commands:
-  ingest      Decompose workbench spec documents into the task graph.
-
-Flags:
-  -h, --help   help for spec
-
-Global Flags:
-      --color string     Color mode: auto, always, or never. NO_COLOR env var overrides always. (default "auto")
-      --db string        Path to the SQLite database (default: ~/.planar/planar.db)
-      --json             Emit machine-readable JSON instead of human text
-      --no-color         Shorthand for --color=never; wins over --color=always.
-      --no-scope-check   Opt out of strict scope resolution; fall back to top-of-stack when cwd is ambiguous (legacy/escape-hatch — not for routine use)
-  -q, --quiet            Suppress informational output
-      --v                Enable debug-level tracing
-      --vv               Enable trace-level tracing
-
-Use "planar spec [command] --help" for more information about a command.
+error: unknown subcommand [in: spec]
--- go.stderr
+++ zig.stderr
@@ -0,0 +1 @@
+error: UnknownSubcommand
--- exit
+++ exit
-go=0
+zig=1
```

#### 122. `annotate --help` — invocation `help`

- Failure class: `go-failed`
- Go exit: `2` (stdout 0B, stderr 47B)
- Zig exit: `0` (stdout 719B, stderr 0B)

```diff
--- go
+++ zig
@@ -0,0 +1,22 @@
+annotate
+
+  Manage source annotations.
+
+USAGE:
+  annotate <command>
+
+COMMANDS:
+  add             Create a new annotation.
+  show            Show an annotation.
+  list            List annotations.
+  update          Update an annotation.
+  remove          Remove an annotation.
+  tag             Add or remove tags on an annotation.
+  resolve         Mark an annotation as resolved.
+  dismiss         Dismiss an annotation.
+  archive         Archive an annotation.
+  bulk-resolve    Resolve multiple annotations.
+  bulk-dismiss    Dismiss multiple annotations.
+  bulk-archive    Archive multiple annotations.
+  verify          Verify annotation anchors against workspace state.
+  sweep           Sweep stale annotations.
--- go.stderr
+++ zig.stderr
@@ -1 +0,0 @@
-error: unknown command "annotate" for "planar"
--- exit
+++ exit
-go=2
+zig=0
```

#### 123. `annotate ` — invocation `no-args`

- Failure class: `go-failed`
- Go exit: `2` (stdout 0B, stderr 47B)
- Zig exit: `0` (stdout 719B, stderr 0B)

```diff
--- go
+++ zig
@@ -0,0 +1,22 @@
+annotate
+
+  Manage source annotations.
+
+USAGE:
+  annotate <command>
+
+COMMANDS:
+  add             Create a new annotation.
+  show            Show an annotation.
+  list            List annotations.
+  update          Update an annotation.
+  remove          Remove an annotation.
+  tag             Add or remove tags on an annotation.
+  resolve         Mark an annotation as resolved.
+  dismiss         Dismiss an annotation.
+  archive         Archive an annotation.
+  bulk-resolve    Resolve multiple annotations.
+  bulk-dismiss    Dismiss multiple annotations.
+  bulk-archive    Archive multiple annotations.
+  verify          Verify annotation anchors against workspace state.
+  sweep           Sweep stale annotations.
--- go.stderr
+++ zig.stderr
@@ -1 +0,0 @@
-error: unknown command "annotate" for "planar"
--- exit
+++ exit
-go=2
+zig=0
```

#### 124. `annotate ` — invocation `real-cwd`

- Failure class: `go-failed`
- Go exit: `2` (stdout 0B, stderr 47B)
- Zig exit: `0` (stdout 719B, stderr 0B)
- cwd: `<CWD_DIR>`

```diff
--- go
+++ zig
@@ -0,0 +1,22 @@
+annotate
+
+  Manage source annotations.
+
+USAGE:
+  annotate <command>
+
+COMMANDS:
+  add             Create a new annotation.
+  show            Show an annotation.
+  list            List annotations.
+  update          Update an annotation.
+  remove          Remove an annotation.
+  tag             Add or remove tags on an annotation.
+  resolve         Mark an annotation as resolved.
+  dismiss         Dismiss an annotation.
+  archive         Archive an annotation.
+  bulk-resolve    Resolve multiple annotations.
+  bulk-dismiss    Dismiss multiple annotations.
+  bulk-archive    Archive multiple annotations.
+  verify          Verify annotation anchors against workspace state.
+  sweep           Sweep stale annotations.
--- go.stderr
+++ zig.stderr
@@ -1 +0,0 @@
-error: unknown command "annotate" for "planar"
--- exit
+++ exit
-go=2
+zig=0
```

#### 125. `health --json` — invocation `json`

- Failure class: `zig-failed`
- Go exit: `0` (stdout 249B, stderr 0B)
- Zig exit: `1` (stdout 264B, stderr 0B)

```diff
--- go
+++ zig
@@ -1,12 +1 @@
-{
-  "db_path": "<AUDIT_DB>",
-  "db_ok": true,
-  "schema_current": true,
-  "integrity_ok": true,
-  "inflight_tasks": 732,
-  "resumable_tasks": 0,
-  "not_resumable_tasks": 732,
-  "pending_handoffs": 0,
-  "stale_handoffs": 0,
-  "overall": "degraded"
-}
+{"db_path":"<AUDIT_DB>","db_ok":true,"schema_version":14,"schema_target":14,"schema_current":true,"migration_count":14,"integrity_ok":true,"inflight_tasks":2,"resumable_tasks":0,"not_resumable_tasks":2,"pending_handoffs":7,"stale_handoffs":3,"overall":"degraded"}
--- exit
+++ exit
-go=0
+zig=1
```

#### 126. `health ` — invocation `no-args`

- Failure class: `both-failed`
- Go exit: `1` (stdout 246B, stderr 23B)
- Zig exit: `1` (stdout 208B, stderr 0B)

```diff
--- go
+++ zig
@@ -1,9 +1,6 @@
-planar health
-
-  db:               <AUDIT_DB>  [ok]
-  schema:           14  [current]
-  integrity:        ok
-  in-flight tasks:  732  (0 resumable, 732 NOT RESUMABLE)
-  pending handoffs: 0  (0 stale)
-
-overall: DEGRADED  (732 tasks not resumable)
+db:               ok (<AUDIT_DB>)
+schema:           v14 of v14 (current)
+integrity:        ok
+in-flight tasks:  2 (0 resumable, 2 NOT resumable)
+pending handoffs: 7 (3 stale > 24h)
+overall:          degraded
--- go.stderr
+++ zig.stderr
@@ -1 +0,0 @@
-error: degraded health
```

#### 127. `health --json` — invocation `real-cwd-json`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 237B, stderr 0B)
- Zig exit: `0` (stdout 256B, stderr 0B)
- cwd: `<CWD_DIR>`

```diff
--- go
+++ zig
@@ -1,12 +1 @@
-{
-  "db_path": "<CWD_DB>",
-  "db_ok": true,
-  "schema_current": true,
-  "integrity_ok": true,
-  "inflight_tasks": 0,
-  "resumable_tasks": 0,
-  "not_resumable_tasks": 0,
-  "pending_handoffs": 0,
-  "stale_handoffs": 0,
-  "overall": "ok"
-}
+{"db_path":"<CWD_DB>","db_ok":true,"schema_version":14,"schema_target":14,"schema_current":true,"migration_count":14,"integrity_ok":true,"inflight_tasks":0,"resumable_tasks":0,"not_resumable_tasks":0,"pending_handoffs":0,"stale_handoffs":0,"overall":"ok"}
```

#### 128. `health ` — invocation `real-cwd`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 207B, stderr 0B)
- Zig exit: `0` (stdout 200B, stderr 0B)
- cwd: `<CWD_DIR>`

```diff
--- go
+++ zig
@@ -1,9 +1,6 @@
-planar health
-
-  db:               <CWD_DB>  [ok]
-  schema:           14  [current]
-  integrity:        ok
-  in-flight tasks:  0  (0 resumable, 0 NOT RESUMABLE)
-  pending handoffs: 0  (0 stale)
-
-overall: OK
+db:               ok (<CWD_DB>)
+schema:           v14 of v14 (current)
+integrity:        ok
+in-flight tasks:  0 (0 resumable, 0 NOT resumable)
+pending handoffs: 0 (0 stale > 24h)
+overall:          ok
```

#### 129. `tree --json` — invocation `real-cwd-json`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 176B, stderr 0B)
- Zig exit: `0` (stdout 151B, stderr 0B)
- cwd: `<CWD_DIR>`

```diff
--- go
+++ zig
@@ -1,8 +1 @@
-{
-  "kind": "scope",
-  "title": "assoc:parity-audit-fixture",
-  "scope_kind": "association",
-  "scope_id": 1,
-  "scope_label": "assoc:parity-audit-fixture",
-  "children": []
-}
+{"kind":"scope","title":"assoc:parity-audit-fixture","scope_kind":"association","scope_id":1,"scope_label":"assoc:parity-audit-fixture","children":[]}
```

#### 130. `handoff --json` — invocation `json`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 121B, stderr 0B)
- Zig exit: `0` (stdout 96B, stderr 0B)

```diff
--- go
+++ zig
@@ -1,8 +1 @@
-{
-  "ok": true,
-  "snapshot_id": 6,
-  "handoff_id": 6,
-  "status": "validated",
-  "resumable": false,
-  "failures": []
-}
+{"ok":true,"snapshot_id":7,"handoff_id":7,"status":"validated","resumable":false,"failures":[]}
```

#### 131. `pl-import ` — invocation `no-args`

- Failure class: `both-failed`
- Go exit: `2` (stdout 0B, stderr 36B)
- Zig exit: `1` (stdout 48B, stderr 33B)

```diff
--- go
+++ zig
@@ -0,0 +1 @@
+error: required positional missing: <repo-root>
--- go.stderr
+++ zig.stderr
@@ -1 +1 @@
-error: accepts 1 arg(s), received 0
+error: MissingRequiredPositional
--- exit
+++ exit
-go=2
+zig=1
```

#### 132. `pl-import --json` — invocation `json`

- Failure class: `both-failed`
- Go exit: `2` (stdout 0B, stderr 36B)
- Zig exit: `1` (stdout 48B, stderr 33B)

```diff
--- go
+++ zig
@@ -0,0 +1 @@
+error: required positional missing: <repo-root>
--- go.stderr
+++ zig.stderr
@@ -1 +1 @@
-error: accepts 1 arg(s), received 0
+error: MissingRequiredPositional
--- exit
+++ exit
-go=2
+zig=1
```

#### 133. `pl-import ` — invocation `real-cwd`

- Failure class: `both-failed`
- Go exit: `2` (stdout 0B, stderr 36B)
- Zig exit: `1` (stdout 48B, stderr 33B)
- cwd: `<CWD_DIR>`

```diff
--- go
+++ zig
@@ -0,0 +1 @@
+error: required positional missing: <repo-root>
--- go.stderr
+++ zig.stderr
@@ -1 +1 @@
-error: accepts 1 arg(s), received 0
+error: MissingRequiredPositional
--- exit
+++ exit
-go=2
+zig=1
```

#### 134. `pl-import --json` — invocation `real-cwd-json`

- Failure class: `both-failed`
- Go exit: `2` (stdout 0B, stderr 36B)
- Zig exit: `1` (stdout 48B, stderr 33B)
- cwd: `<CWD_DIR>`

```diff
--- go
+++ zig
@@ -0,0 +1 @@
+error: required positional missing: <repo-root>
--- go.stderr
+++ zig.stderr
@@ -1 +1 @@
-error: accepts 1 arg(s), received 0
+error: MissingRequiredPositional
--- exit
+++ exit
-go=2
+zig=1
```

#### 135. `pl-synthesize ` — invocation `no-args`

- Failure class: `both-failed`
- Go exit: `2` (stdout 0B, stderr 36B)
- Zig exit: `1` (stdout 48B, stderr 33B)

```diff
--- go
+++ zig
@@ -0,0 +1 @@
+error: required positional missing: <repo-root>
--- go.stderr
+++ zig.stderr
@@ -1 +1 @@
-error: accepts 1 arg(s), received 0
+error: MissingRequiredPositional
--- exit
+++ exit
-go=2
+zig=1
```

#### 136. `pl-synthesize --json` — invocation `json`

- Failure class: `both-failed`
- Go exit: `2` (stdout 0B, stderr 36B)
- Zig exit: `1` (stdout 48B, stderr 33B)

```diff
--- go
+++ zig
@@ -0,0 +1 @@
+error: required positional missing: <repo-root>
--- go.stderr
+++ zig.stderr
@@ -1 +1 @@
-error: accepts 1 arg(s), received 0
+error: MissingRequiredPositional
--- exit
+++ exit
-go=2
+zig=1
```

#### 137. `pl-synthesize ` — invocation `real-cwd`

- Failure class: `both-failed`
- Go exit: `2` (stdout 0B, stderr 36B)
- Zig exit: `1` (stdout 48B, stderr 33B)
- cwd: `<CWD_DIR>`

```diff
--- go
+++ zig
@@ -0,0 +1 @@
+error: required positional missing: <repo-root>
--- go.stderr
+++ zig.stderr
@@ -1 +1 @@
-error: accepts 1 arg(s), received 0
+error: MissingRequiredPositional
--- exit
+++ exit
-go=2
+zig=1
```

#### 138. `pl-synthesize --json` — invocation `real-cwd-json`

- Failure class: `both-failed`
- Go exit: `2` (stdout 0B, stderr 36B)
- Zig exit: `1` (stdout 48B, stderr 33B)
- cwd: `<CWD_DIR>`

```diff
--- go
+++ zig
@@ -0,0 +1 @@
+error: required positional missing: <repo-root>
--- go.stderr
+++ zig.stderr
@@ -1 +1 @@
-error: accepts 1 arg(s), received 0
+error: MissingRequiredPositional
--- exit
+++ exit
-go=2
+zig=1
```

#### 139. `handoff ` — invocation `no-args`

- Failure class: `neither-failed`
- Go exit: `0` (stdout 109B, stderr 0B)
- Zig exit: `0` (stdout 109B, stderr 0B)

```diff
--- go
+++ zig
@@ -1,3 +1,3 @@
 handoff captured for current task
-  snapshot: 4  vendor: cli  next_action: 
-  handoff:  4  status: validated
+  snapshot: 5  vendor: cli  next_action: 
+  handoff:  5  status: validated
```

#### 140. `unlink ` — invocation `no-args`

- Failure class: `both-failed`
- Go exit: `2` (stdout 0B, stderr 36B)
- Zig exit: `1` (stdout 46B, stderr 33B)

```diff
--- go
+++ zig
@@ -0,0 +1 @@
+error: required positional missing: <link-id>
--- go.stderr
+++ zig.stderr
@@ -1 +1 @@
-error: accepts 1 arg(s), received 0
+error: MissingRequiredPositional
--- exit
+++ exit
-go=2
+zig=1
```

#### 141. `unlink --json` — invocation `json`

- Failure class: `both-failed`
- Go exit: `2` (stdout 0B, stderr 36B)
- Zig exit: `1` (stdout 46B, stderr 33B)

```diff
--- go
+++ zig
@@ -0,0 +1 @@
+error: required positional missing: <link-id>
--- go.stderr
+++ zig.stderr
@@ -1 +1 @@
-error: accepts 1 arg(s), received 0
+error: MissingRequiredPositional
--- exit
+++ exit
-go=2
+zig=1
```

#### 142. `unlink ` — invocation `real-cwd`

- Failure class: `both-failed`
- Go exit: `2` (stdout 0B, stderr 36B)
- Zig exit: `1` (stdout 46B, stderr 33B)
- cwd: `<CWD_DIR>`

```diff
--- go
+++ zig
@@ -0,0 +1 @@
+error: required positional missing: <link-id>
--- go.stderr
+++ zig.stderr
@@ -1 +1 @@
-error: accepts 1 arg(s), received 0
+error: MissingRequiredPositional
--- exit
+++ exit
-go=2
+zig=1
```

#### 143. `unlink --json` — invocation `real-cwd-json`

- Failure class: `both-failed`
- Go exit: `2` (stdout 0B, stderr 36B)
- Zig exit: `1` (stdout 46B, stderr 33B)
- cwd: `<CWD_DIR>`

```diff
--- go
+++ zig
@@ -0,0 +1 @@
+error: required positional missing: <link-id>
--- go.stderr
+++ zig.stderr
@@ -1 +1 @@
-error: accepts 1 arg(s), received 0
+error: MissingRequiredPositional
--- exit
+++ exit
-go=2
+zig=1
```

#### 144. `annotate --json` — invocation `json`

- Failure class: `both-failed`
- Go exit: `2` (stdout 0B, stderr 47B)
- Zig exit: `1` (stdout 41B, stderr 25B)

```diff
--- go
+++ zig
@@ -0,0 +1 @@
+error: unknown subcommand [in: annotate]
--- go.stderr
+++ zig.stderr
@@ -1 +1 @@
-error: unknown command "annotate" for "planar"
+error: UnknownSubcommand
--- exit
+++ exit
-go=2
+zig=1
```

#### 145. `annotate --json` — invocation `real-cwd-json`

- Failure class: `both-failed`
- Go exit: `2` (stdout 0B, stderr 47B)
- Zig exit: `1` (stdout 41B, stderr 25B)
- cwd: `<CWD_DIR>`

```diff
--- go
+++ zig
@@ -0,0 +1 @@
+error: unknown subcommand [in: annotate]
--- go.stderr
+++ zig.stderr
@@ -1 +1 @@
-error: unknown command "annotate" for "planar"
+error: UnknownSubcommand
--- exit
+++ exit
-go=2
+zig=1
```

#### 146. `search ` — invocation `no-args`

- Failure class: `both-failed`
- Go exit: `2` (stdout 0B, stderr 36B)
- Zig exit: `1` (stdout 44B, stderr 33B)

```diff
--- go
+++ zig
@@ -0,0 +1 @@
+error: required positional missing: <query>
--- go.stderr
+++ zig.stderr
@@ -1 +1 @@
-error: accepts 1 arg(s), received 0
+error: MissingRequiredPositional
--- exit
+++ exit
-go=2
+zig=1
```

#### 147. `search --json` — invocation `json`

- Failure class: `both-failed`
- Go exit: `2` (stdout 0B, stderr 36B)
- Zig exit: `1` (stdout 44B, stderr 33B)

```diff
--- go
+++ zig
@@ -0,0 +1 @@
+error: required positional missing: <query>
--- go.stderr
+++ zig.stderr
@@ -1 +1 @@
-error: accepts 1 arg(s), received 0
+error: MissingRequiredPositional
--- exit
+++ exit
-go=2
+zig=1
```

#### 148. `search ` — invocation `real-cwd`

- Failure class: `both-failed`
- Go exit: `2` (stdout 0B, stderr 36B)
- Zig exit: `1` (stdout 44B, stderr 33B)
- cwd: `<CWD_DIR>`

```diff
--- go
+++ zig
@@ -0,0 +1 @@
+error: required positional missing: <query>
--- go.stderr
+++ zig.stderr
@@ -1 +1 @@
-error: accepts 1 arg(s), received 0
+error: MissingRequiredPositional
--- exit
+++ exit
-go=2
+zig=1
```

#### 149. `search --json` — invocation `real-cwd-json`

- Failure class: `both-failed`
- Go exit: `2` (stdout 0B, stderr 36B)
- Zig exit: `1` (stdout 44B, stderr 33B)
- cwd: `<CWD_DIR>`

```diff
--- go
+++ zig
@@ -0,0 +1 @@
+error: required positional missing: <query>
--- go.stderr
+++ zig.stderr
@@ -1 +1 @@
-error: accepts 1 arg(s), received 0
+error: MissingRequiredPositional
--- exit
+++ exit
-go=2
+zig=1
```

#### 150. `demote ` — invocation `no-args`

- Failure class: `both-failed`
- Go exit: `2` (stdout 0B, stderr 36B)
- Zig exit: `1` (stdout 42B, stderr 33B)

```diff
--- go
+++ zig
@@ -0,0 +1 @@
+error: required positional missing: <ref>
--- go.stderr
+++ zig.stderr
@@ -1 +1 @@
-error: accepts 1 arg(s), received 0
+error: MissingRequiredPositional
--- exit
+++ exit
-go=2
+zig=1
```

#### 151. `demote --json` — invocation `json`

- Failure class: `both-failed`
- Go exit: `2` (stdout 0B, stderr 36B)
- Zig exit: `1` (stdout 42B, stderr 33B)

```diff
--- go
+++ zig
@@ -0,0 +1 @@
+error: required positional missing: <ref>
--- go.stderr
+++ zig.stderr
@@ -1 +1 @@
-error: accepts 1 arg(s), received 0
+error: MissingRequiredPositional
--- exit
+++ exit
-go=2
+zig=1
```

#### 152. `demote ` — invocation `real-cwd`

- Failure class: `both-failed`
- Go exit: `2` (stdout 0B, stderr 36B)
- Zig exit: `1` (stdout 42B, stderr 33B)
- cwd: `<CWD_DIR>`

```diff
--- go
+++ zig
@@ -0,0 +1 @@
+error: required positional missing: <ref>
--- go.stderr
+++ zig.stderr
@@ -1 +1 @@
-error: accepts 1 arg(s), received 0
+error: MissingRequiredPositional
--- exit
+++ exit
-go=2
+zig=1
```

#### 153. `demote --json` — invocation `real-cwd-json`

- Failure class: `both-failed`
- Go exit: `2` (stdout 0B, stderr 36B)
- Zig exit: `1` (stdout 42B, stderr 33B)
- cwd: `<CWD_DIR>`

```diff
--- go
+++ zig
@@ -0,0 +1 @@
+error: required positional missing: <ref>
--- go.stderr
+++ zig.stderr
@@ -1 +1 @@
-error: accepts 1 arg(s), received 0
+error: MissingRequiredPositional
--- exit
+++ exit
-go=2
+zig=1
```

#### 154. `handoff ` — invocation `real-cwd`

- Failure class: `both-failed`
- Go exit: `1` (stdout 0B, stderr 73B)
- Zig exit: `2` (stdout 0B, stderr 62B)
- cwd: `<CWD_DIR>`

```diff
--- go.stderr
+++ zig.stderr
@@ -1,2 +1 @@
-error: no active session found for vendor "cli"
-error: no active session
+error: no active session (run `planar capture session` first)
--- exit
+++ exit
-go=1
+zig=2
```

#### 155. `handoff --json` — invocation `real-cwd-json`

- Failure class: `both-failed`
- Go exit: `1` (stdout 0B, stderr 73B)
- Zig exit: `2` (stdout 0B, stderr 62B)
- cwd: `<CWD_DIR>`

```diff
--- go.stderr
+++ zig.stderr
@@ -1,2 +1 @@
-error: no active session found for vendor "cli"
-error: no active session
+error: no active session (run `planar capture session` first)
--- exit
+++ exit
-go=1
+zig=2
```

#### 156. `agent ` — invocation `q233-agent-top`

- Failure class: `both-failed`
- Go exit: `2` (stdout 0B, stderr 44B)
- Zig exit: `1` (stdout 39B, stderr 25B)

```diff
--- go
+++ zig
@@ -0,0 +1 @@
+error: unknown subcommand [in: planar]
--- go.stderr
+++ zig.stderr
@@ -1 +1 @@
-error: unknown command "agent" for "planar"
+error: UnknownSubcommand
--- exit
+++ exit
-go=2
+zig=1
```

#### 157. `agent ps` — invocation `q233-agent-ps`

- Failure class: `both-failed`
- Go exit: `2` (stdout 0B, stderr 44B)
- Zig exit: `1` (stdout 39B, stderr 25B)

```diff
--- go
+++ zig
@@ -0,0 +1 @@
+error: unknown subcommand [in: planar]
--- go.stderr
+++ zig.stderr
@@ -1 +1 @@
-error: unknown command "agent" for "planar"
+error: UnknownSubcommand
--- exit
+++ exit
-go=2
+zig=1
```

#### 158. `link ` — invocation `no-args`

- Failure class: `both-failed`
- Go exit: `2` (stdout 0B, stderr 36B)
- Zig exit: `1` (stdout 35B, stderr 23B)

```diff
--- go
+++ zig
@@ -0,0 +1 @@
+error: required flag missing: --to
--- go.stderr
+++ zig.stderr
@@ -1 +1 @@
-error: accepts 1 arg(s), received 0
+error: MissingRequired
--- exit
+++ exit
-go=2
+zig=1
```

#### 159. `link --json` — invocation `json`

- Failure class: `both-failed`
- Go exit: `2` (stdout 0B, stderr 36B)
- Zig exit: `1` (stdout 35B, stderr 23B)

```diff
--- go
+++ zig
@@ -0,0 +1 @@
+error: required flag missing: --to
--- go.stderr
+++ zig.stderr
@@ -1 +1 @@
-error: accepts 1 arg(s), received 0
+error: MissingRequired
--- exit
+++ exit
-go=2
+zig=1
```

#### 160. `link ` — invocation `real-cwd`

- Failure class: `both-failed`
- Go exit: `2` (stdout 0B, stderr 36B)
- Zig exit: `1` (stdout 35B, stderr 23B)
- cwd: `<CWD_DIR>`

```diff
--- go
+++ zig
@@ -0,0 +1 @@
+error: required flag missing: --to
--- go.stderr
+++ zig.stderr
@@ -1 +1 @@
-error: accepts 1 arg(s), received 0
+error: MissingRequired
--- exit
+++ exit
-go=2
+zig=1
```

#### 161. `link --json` — invocation `real-cwd-json`

- Failure class: `both-failed`
- Go exit: `2` (stdout 0B, stderr 36B)
- Zig exit: `1` (stdout 35B, stderr 23B)
- cwd: `<CWD_DIR>`

```diff
--- go
+++ zig
@@ -0,0 +1 @@
+error: required flag missing: --to
--- go.stderr
+++ zig.stderr
@@ -1 +1 @@
-error: accepts 1 arg(s), received 0
+error: MissingRequired
--- exit
+++ exit
-go=2
+zig=1
```

#### 162. `promote ` — invocation `no-args`

- Failure class: `both-failed`
- Go exit: `2` (stdout 0B, stderr 36B)
- Zig exit: `1` (stdout 35B, stderr 23B)

```diff
--- go
+++ zig
@@ -0,0 +1 @@
+error: required flag missing: --to
--- go.stderr
+++ zig.stderr
@@ -1 +1 @@
-error: accepts 1 arg(s), received 0
+error: MissingRequired
--- exit
+++ exit
-go=2
+zig=1
```

#### 163. `promote --json` — invocation `json`

- Failure class: `both-failed`
- Go exit: `2` (stdout 0B, stderr 36B)
- Zig exit: `1` (stdout 35B, stderr 23B)

```diff
--- go
+++ zig
@@ -0,0 +1 @@
+error: required flag missing: --to
--- go.stderr
+++ zig.stderr
@@ -1 +1 @@
-error: accepts 1 arg(s), received 0
+error: MissingRequired
--- exit
+++ exit
-go=2
+zig=1
```

#### 164. `promote ` — invocation `real-cwd`

- Failure class: `both-failed`
- Go exit: `2` (stdout 0B, stderr 36B)
- Zig exit: `1` (stdout 35B, stderr 23B)
- cwd: `<CWD_DIR>`

```diff
--- go
+++ zig
@@ -0,0 +1 @@
+error: required flag missing: --to
--- go.stderr
+++ zig.stderr
@@ -1 +1 @@
-error: accepts 1 arg(s), received 0
+error: MissingRequired
--- exit
+++ exit
-go=2
+zig=1
```

#### 165. `promote --json` — invocation `real-cwd-json`

- Failure class: `both-failed`
- Go exit: `2` (stdout 0B, stderr 36B)
- Zig exit: `1` (stdout 35B, stderr 23B)
- cwd: `<CWD_DIR>`

```diff
--- go
+++ zig
@@ -0,0 +1 @@
+error: required flag missing: --to
--- go.stderr
+++ zig.stderr
@@ -1 +1 @@
-error: accepts 1 arg(s), received 0
+error: MissingRequired
--- exit
+++ exit
-go=2
+zig=1
```

#### 166. `test-spec status 351` — invocation `q233-status-positional`

- Failure class: `both-failed`
- Go exit: `1` (stdout 0B, stderr 122B)
- Zig exit: `1` (stdout 0B, stderr 28B)

```diff
--- go.stderr
+++ zig.stderr
@@ -1 +1 @@
-error: plan "351" not found: plan 351 not found or is not a top-level plan: plan 351 not found or is not a top-level plan
+error: plan '351' not found
```

#### 167. `question add --plan 351 --title parity-probe --body x` — invocation `q233-add-plan-flag`

- Failure class: `both-failed`
- Go exit: `2` (stdout 0B, stderr 29B)
- Zig exit: `1` (stdout 34B, stderr 19B)

```diff
--- go
+++ zig
@@ -0,0 +1 @@
+error: unknown flag (got --title)
--- go.stderr
+++ zig.stderr
@@ -1 +1 @@
-error: unknown flag: --title
+error: UnknownFlag
--- exit
+++ exit
-go=2
+zig=1
```

#### 168. `task add --plan 351 --title parity-probe --next-action x --editor=false` — invocation `q233-add-editor-false`

- Failure class: `both-failed`
- Go exit: `2` (stdout 0B, stderr 29B)
- Zig exit: `1` (stdout 34B, stderr 19B)

```diff
--- go
+++ zig
@@ -0,0 +1 @@
+error: unknown flag (got --title)
--- go.stderr
+++ zig.stderr
@@ -1 +1 @@
-error: unknown flag: --title
+error: UnknownFlag
--- exit
+++ exit
-go=2
+zig=1
```

#### 169. `task add --plan 351 --title parity-probe --next-action x --no-editor` — invocation `q233-add-no-editor`

- Failure class: `both-failed`
- Go exit: `2` (stdout 0B, stderr 29B)
- Zig exit: `1` (stdout 34B, stderr 19B)

```diff
--- go
+++ zig
@@ -0,0 +1 @@
+error: unknown flag (got --title)
--- go.stderr
+++ zig.stderr
@@ -1 +1 @@
-error: unknown flag: --title
+error: UnknownFlag
--- exit
+++ exit
-go=2
+zig=1
```

#### 170. `test-spec status --plan 351` — invocation `q233-status-plan-flag`

- Failure class: `both-failed`
- Go exit: `2` (stdout 0B, stderr 28B)
- Zig exit: `1` (stdout 33B, stderr 19B)

```diff
--- go
+++ zig
@@ -0,0 +1 @@
+error: unknown flag (got --plan)
--- go.stderr
+++ zig.stderr
@@ -1 +1 @@
-error: unknown flag: --plan
+error: UnknownFlag
--- exit
+++ exit
-go=2
+zig=1
```

#### 171. `resume ` — invocation `real-cwd`

- Failure class: `both-failed`
- Go exit: `1` (stdout 0B, stderr 55B)
- Zig exit: `2` (stdout 0B, stderr 55B)
- cwd: `<CWD_DIR>`

```diff
--- exit
+++ exit
-go=1
+zig=2
```

#### 172. `resume --json` — invocation `real-cwd-json`

- Failure class: `both-failed`
- Go exit: `1` (stdout 0B, stderr 55B)
- Zig exit: `2` (stdout 0B, stderr 55B)
- cwd: `<CWD_DIR>`

```diff
--- exit
+++ exit
-go=1
+zig=2
```

## No-diff invocations

| Verb | Invocation | Args |
|------|------------|------|
| `tree` | `real-cwd` | `` |
