---
slug: pl-local
description: "Manage the complete lifecycle of operator-local skills and agents: import, link, list, unlink, migrate, and repair."
source: docs/cli-reference.md#domain-local
vendor:
  claude:
    argument_hint: "<import|link|list|unlink|migrate|repair> [target] [options]"
    invocation_examples: |
      /pl-local import ~/my-skills/
      /pl-local link fixup-protos --vendor codex
      /pl-local list
      /pl-local unlink fixup-protos
      /pl-local migrate --dry-run
      /pl-local repair
shared_notes:
  - "Local sources remain machine-local under ~/.planar/local/{skills,agents}/; this workflow never promotes or writes canonical repo skills."
---

# Planar Local Lifecycle ({{.VendorTitle}})

Manage operator-authored skills and agents from import through vendor linking,
inspection, unlinking, legacy migration, and repair. Sources live only under
`~/.planar/local/{skills,agents}/`; vendor installs use the visible `local-`
prefix unless source frontmatter explicitly sets `shadow: true`.

## Context

Resolve one operation: `import`, `link`, `list`, `unlink`, `migrate`, or
`repair`. Record the requested path or local name, kind, vendor filter, and
preview mode. These are machine-local filesystem operations, not planning
entity writes; do not add `--scope`, weaken scope checks, or touch SQLite.

`import` requires a source path. `link` optionally takes one sandbox name.
`unlink` requires one name. `list`, `migrate`, and `repair` take no positional
target. If the operation is omitted or ambiguous, ask which lifecycle action
the operator wants before mutating anything.

## Intent

State the interpreted operation and target in one sentence. Say explicitly
when `--dry-run` makes the request a preview. For unlink, distinguish retaining
the sandbox source (default) from deleting it (`--purge`) before execution.

## Actions

Use only these schema-backed commands:

```text
planar local import <path> [--kind skill|agent] [--force] [--dry-run] [--no-link] --json
planar local link [<name>] [--dry-run] [--vendor <vendor>]... --json
planar local list [--vendor <vendor>] --json
planar local unlink <name> [--purge] --json
planar local migrate [--dry-run] --json
planar local link --reconcile --json
```

Map `repair` to `planar local link --reconcile --json`; the CLI has no
standalone `local repair` verb. `--reconcile` takes no name or vendor filter.
It removes stale manifest rows whose source disappeared and repairs recorded
vendor targets that are missing or inconsistent.

Run the chosen mutation once, retaining its JSON action rows. Then verify
persisted state with `planar local list --json` (or the same command with the
requested `--vendor` filter):

- after import with automatic linking or after link, every expected install
  should be `live`;
- after import with `--no-link`, confirm the returned sandbox destination and
  explain that no install row is expected yet;
- after unlink, confirm no install row remains for the name; with `--purge`,
  also retain the CLI's source-removal result;
- after migrate, retain each migrated/skipped row, then list current installs;
  migration changes source shape but does not invent missing vendor installs;
- after repair, list again and report remaining `broken` or `missing` rows.

Treat `--dry-run` as preview-only: report proposed actions with `applied=0`
and do not misrepresent the unchanged list as a failure. Track `attempted`,
`applied`, `skipped`, and `failed` across individual import, migration, link,
unlink, or reconciliation action rows. Independent targets may partially
succeed; do not claim transaction-wide rollback.

## Result

Return `outcome=ok|partial|error`, the operation, target, kind/vendor filters,
preview state, action counts, and stable post-state paths or install rows.
For linked installs include name, kind, vendor, status, and installed path.
Call success only after the post-state read agrees with the requested result
when a list row can prove it. An idempotent `unchanged` or already-absent result
is a successful no-op, not a warning.

## Warnings

Report name collisions, invalid frontmatter, ignored collection entries,
copy-fallback installs, explicit shadows, migration collisions, and each
remaining non-live install. `--force` may replace an existing sandbox entry;
`unlink --purge` deletes the sandbox source; require clear operator intent for
either destructive flag.

Never delete the external import source. Never describe an unlink without
`--purge` as deleting the sandbox source. Never claim that repairing a missing
source restores its content: reconciliation removes its stale manifest and
vendor installs because no source remains to restore from.

## Next actions

Give at most three executable recommendations, chosen from:

```text
planar local list --json
planar local link <name> --json
planar local link --reconcile --json
planar local migrate --dry-run --json
```

For a successfully imported-and-linked entry, usually no next action is
needed. For `--no-link`, recommend the exact `local link <name>` command.

## Recovery

Give an exact idempotent inspect or retry command for every failed target:

- inspect all persisted installs: `planar local list --json`;
- retry a failed import without overwriting: rerun the same
  `planar local import ... --json` command; use `--force` only after explicit
  approval of the named collision;
- retry one link: `planar local link <name> --json`;
- inspect/repair vendor drift: `planar local link --reconcile --json`, then
  `planar local list --json`;
- preview legacy conversion: `planar local migrate --dry-run --json`, then run
  it without `--dry-run` only after collisions are resolved.

Completed targets remain applied after a partial multi-target operation. Do
not promise rollback where the CLI provides none.

## Boundaries

- Use `planar local` as the only lifecycle access layer; do not hand-author
  vendor links or `.link-manifest.json` rows.
- Keep sources under `~/.planar/local/{skills,agents}/`; never commit them.
- Do not create a `planar local promote` flow. Canonical promotion is a manual
  contribution that copies reviewed source into `skills/src/` and follows the
  normal render and review gates.
- Preserve the `local-` install prefix unless the existing source explicitly
  opts into `shadow: true`; never add shadowing on the operator's behalf.
- Do not recurse beyond the CLI importer's supported flat collection shape or
  translate vendor-specific formats yourself.

## Vendor Notes

{{.VendorNotes}}
{{- if .InvocationBlock}}

## Invocation

```text
{{.InvocationBlock -}}
```
{{- end}}
