---
description: Import skill or agent files from an external directory into the local sandbox and link them into every vendor surface.
argument-hint: <path-to-skill-file-or-directory>
source: docs/cli-reference.md#domain-local
---

# Pl-Local-Import (Claude)

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

## Vendor Notes

- Installed to `~/.claude/commands/pl-local-import.md`.
- Invoked as `/pl-local-import <subcommand> [args]`.
- Local sandbox files are linked through the CLI link layer; source files must already be valid skill or agent inputs.

## Invocation

```
/pl-local-import ~/my-skills/fixup-protos.md
/pl-local-import ~/my-skills/
/pl-local-import ~/my-agents/ --kind agent
```
