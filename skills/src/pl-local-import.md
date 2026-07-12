---
slug: pl-local-import
description: "Import skill or agent files from an external directory into the local sandbox and link them into every vendor surface."
source: docs/cli-reference.md#domain-local
vendor:
  claude:
    argument_hint: "<path-to-skill-file-or-directory>"
    invocation_examples: |
      /pl-local-import ~/my-skills/fixup-protos.md
      /pl-local-import ~/my-skills/
      /pl-local-import ~/my-agents/ --kind agent
shared_notes:
  - "Local sandbox files are linked through the CLI link layer; source files must already be valid skill or agent inputs."
---

# Pl-Local-Import ({{.VendorTitle}})

Import operator-authored skill or agent files from an external location (a personal skills folder, a separate git repo, a Dropbox directory) into the local sandbox at `~/.planar/local/{skills,agents}/`, then symlink them into every vendor surface so they are immediately invocable.

## When To Invoke

When the operator says any of:

- "import my skills from `<path>`"
- "ingest the skill at `<path>` into the sandbox"
- "bring in my external skill collection"
- "add this skill folder to planar"

The argument is a single flat `.md` file, a single dir-shape skill source (`foo/` containing `SKILL.md`), or a directory containing a mix of both. Subdirectories that are not skill dirs are ignored.

## What It Does

Runs `planar local import` with the operator-supplied path. The verb:

1. Validates each file's YAML frontmatter (the same rules as the canonical sandbox walker — `vendors:` must be a subset of `{claude, codex, copilot}`, `kind:` if present must match the target kind).
2. Materializes each valid input into the sandbox in the correct shape: skills as `~/.planar/local/skills/<name>/SKILL.md` (with any dir-shape auxiliary files copied along), agents as `~/.planar/local/agents/<name>.md`.
3. Invokes the existing link layer to install into every vendor's surface (Claude gets a file symlink → `<src>/SKILL.md`; Codex and Copilot get directory symlinks → the source dir).

Name collisions with existing sandbox files are skipped unless `--force` is passed; the operator's prior work is never silently lost.

## What It Does Not Do

- Does not recurse into subdirectories — operator collections are expected to be flat.
- Does not convert between vendor formats (e.g. Codex SKILL.md directories back into flat `.md` files). Source files must already be the flat-frontmatter skill format.
- Does not modify or delete the operator's source files.

## Underlying CLI Verb

Composes from [`local`](../../docs/cli-reference.md#domain-local):

```
planar local import <path> [--kind skill|agent] [--force] [--dry-run] [--no-link]
```

`--kind skill` is the default. Use `--kind agent` when the input is an agent role file rather than a skill. `--dry-run` previews without writing. `--no-link` imports without linking (useful when the operator wants to inspect the sandbox first).

## Output Shape

```
fixup-protos  imported      ←  /home/me/my-skills/fixup-protos.md
audit-deps    skipped       reason: name-collision

imported 1 file(s); skipped 1

Linking imported files into vendor surfaces:
fixup-protos (skill)
  claude   created [symlink]  →  /home/me/.claude/commands/local-fixup-protos.md
  codex    created [symlink]  →  /home/me/.codex/skills/local-fixup-protos
  copilot  created [symlink]  →  /home/me/.copilot/skills/local-fixup-protos
```

## Context

Report the source path, resolved sandbox root, skill or agent kind, selected
vendors, force/no-link flags, and dry-run or apply mode.

## Intent

State in one sentence which local extensions will be previewed or imported and
whether vendor links will be created.

## Actions

Report `attempted`, `applied` (the succeeded count), `skipped`, and `failed` per source and per vendor
link. Name every failed source or vendor target with its canonical local name,
source path, destination, and failure evidence. Collisions without `--force`
and ignored non-skill subdirectories are skips, not failures.

## Result

Always report `outcome=ok|partial|error`. A dry run returns proposed sandbox
and vendor paths with zero applied. After apply, confirm each imported sandbox
path and each requested symlink target, and return those identities. An
idempotent collision-only run is `outcome=ok`, zero applied, with the collision
reason.

## Warnings

Name invalid frontmatter, kind mismatches, unavailable link verification, and
partial imports or vendor-link failures. Preserve the dry-run preview and do
not use `--force` without explicit operator intent. Successful imports and
links are not rolled back when another target fails.

## Next actions

Give zero to three executable recommendations. A preview leads with the exact
approved import command; `--no-link` results may recommend the corresponding
`planar local link` command only if that public verb is available in the
current CLI schema.

## Recovery

For each failed source, give `planar local import <source> --kind <kind>
[--force] [--no-link]` with the original safe flags. For a vendor-link failure,
name the failed destination and retry through the exact CLI command surfaced
by the import result; otherwise give the sandbox path to inspect and state
that link retry is unavailable through this compatibility workflow. Never
delete successful imports as fabricated rollback.

## Vendor Notes

{{.VendorNotes}}
{{- if .InvocationBlock}}

## Invocation

```
{{.InvocationBlock -}}
```
{{- end}}
