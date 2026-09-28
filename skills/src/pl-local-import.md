---
description: Compatibility wrapper for importing skill or agent files into the local sandbox through the canonical pl-local lifecycle.
origin: docs/cli-reference.md#domain-local
shared_notes:
    - This compatibility entry point preserves existing import invocations; use pl-local for link, list, unlink, migrate, and repair.
slug: pl-local-import
vendor:
    claude:
        argument_hint: <path-to-skill-file-or-directory>
        invocation_examples: |
            /pl-local-import ~/my-skills/fixup-protos.md
            /pl-local-import ~/my-skills/
            /pl-local-import ~/my-agents/ --kind agent
---

# Pl-Local-Import Compatibility Wrapper ({{ VendorTitle }})

Preserve the existing `pl-local-import <path> [options]` invocation while
routing it to the canonical `pl-local import` operation. Use `pl-local` for
the broader link, list, unlink, migrate, and repair lifecycle.

## When To Invoke

When the operator says any of:

- "import my skills from `<path>`"
- "ingest the skill at `<path>` into the sandbox"
- "bring in my external skill collection"
- "add this skill folder to planar"

The argument remains a single flat `.md` file, a single dir-shape skill source
(`foo/` containing `SKILL.md`), or a flat collection containing both shapes.

## Context

Interpret every existing invocation as `pl-local import` with the same path,
kind, `--force`, `--dry-run`, and `--no-link` options. Sources remain
machine-local under `~/.planar/local/{skills,agents}/`.

## Intent

State that the request imports the supplied path through the canonical local
lifecycle, including whether it is an agent import, preview, overwrite, or
import-without-linking.

## Actions

Run the matching canonical operation with all supplied options unchanged:

```text
planar local import <path> [--kind skill|agent] [--force] [--dry-run] [--no-link] --json
```

Follow the complete `pl-local` import contract: use CLI JSON internally, verify
linked installs with `planar local list --json` unless `--no-link` or
`--dry-run` intentionally leaves no new live row, and count attempted,
applied, skipped, and failed import/link actions.

## Result

Return the same import outcome as `pl-local import`: `outcome=ok|partial|error`,
the path and kind, action counts, sandbox destinations, and verified vendor
install rows. Also name `pl-local` as the lifecycle entry point for follow-up
operations.

## Warnings

Preserve `pl-local` warnings for collisions, invalid inputs, ignored entries,
copy fallback, shadowing, and partial linking. `--force` requires clear intent
because it can replace an existing sandbox entry. Never modify or delete the
external source.

## Next actions

Give at most three executable recommendations. Prefer
`planar local list --json`, `planar local link <name> --json` after
`--no-link`, or the broader `pl-local` workflow for another lifecycle action.

## Recovery

On partial failure, list each failed input and provide the exact idempotent
`planar local import ... --json` retry plus `planar local list --json` for
inspection. Do not claim rollback of inputs that already imported or linked.

## Boundaries

- This wrapper performs only import; it does not invent wrapper-only flags or
  CLI verbs.
- Use `pl-local` for link, list, unlink, migrate, and repair.
- There is no `planar local promote`; canonical promotion remains manual.
- Do not commit machine-local sources or hand-edit vendor installs/manifests.

## Vendor Notes

