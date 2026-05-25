---
name: pl-doc-regenerate
description: Re-synthesize an existing doc from its current sources; refuses to overwrite hand edits without --force/--merge.
source: docs/cli-reference.md#domain-doc
---

# Planar Doc Regenerate (Codex)

Re-synthesises an existing outward-facing doc when its declared source
entities have drifted, refreshing the body and the provenance
front-matter `source_versions` map. The four-signal classifier in
`.manifest-docs` is what decides whether a regenerate is needed at all,
and whether the doc has been hand-edited since the last synthesis.

## What It Does

Reads each target doc's provenance front matter to recover the
template and the declared source entity refs, resolves those sources'
current bodies from the database, fills the doc-kind synthesis
template, calls the LLM at temperature 0 to produce a refreshed body,
then hands that body to `planar doc regenerate` which validates
hand-edit safety, writes the doc with refreshed provenance, and
re-runs the manifest build atomically.

## CLI Commands

Wraps [`doc`](../../docs/cli-reference.md#domain-doc):

```
planar doc regenerate (--slug <slug> | --path <path> | --all)
                      [--force | --merge] [--body-file <path>]
                      [--out <path>]
planar doc manifest diff --json
planar artifact show <id> --json
planar decision show <id> --json
planar plan show <id> --json
```

## When To Invoke

Run this skill after a source entity (artifact, decision, plan) has
been updated and a downstream outward-facing doc needs to catch up —
for example after a product-spec artifact gained a new section, or an
ADR's rationale was rewritten, or a research artifact was extended.
The skill is deliberate; it consumes LLM budget and overwrites a file
under `docs/`, so it should not be a per-session reflex. Prefer to
batch with `--all` after a roadmap-level update so one skill run
refreshes the entire affected surface.

## How the Skill Composes

1. Resolve targets. With `--slug <slug>` look up the matching doc
   under `docs/` by stem (the file whose `<slug>.md` filename matches).
   With `--path <path>` use the path directly. With `--all`, call
   `planar doc manifest diff --json` and collect every entry whose
   `signal` is `regenerate-candidate`. Exit early with "no docs need
   regeneration" if `--all` produced an empty list.

2. For each target path, read the doc's provenance front matter by
   reading the file directly and parsing the YAML between the first
   `---\n` line and the next `\n---\n` terminator. The provenance
   block names the `doc_kind`, the `template_version`, and the
   declared source entity refs in `source_artifacts`,
   `source_decisions`, and `source_plans`. If any required field is
   missing the doc cannot be regenerated — surface the error and skip.

3. Detect hand-edit. Compute the normalised xxh64 of the on-disk doc
   body and compare it to the `doc_hash` recorded for that path in
   `.manifest-docs`. If they differ AND the manifest's `sources` block
   for that entry still matches the current source-version hashes,
   the four-signal classifier flags this as `hand-edit`. Refuse unless
   `--force` (discard the hand edit) or `--merge` (write the
   regenerated body to a sibling file and leave the original alone).
   The Go verb performs this check independently — the skill simply
   passes `--force` or `--merge` through.

4. Load the doc-prompt template at
   `~/.planar/templates/doc-prompts/<doc_kind>.md`. The template's
   front matter declares `template_version`, `synthesis_voice`,
   `required_sources`, `optional_sources`, and `output_shape`; the
   body below the fence is the prompt the LLM receives.

5. Resolve each declared source's current body via
   `planar artifact show <id> --json`,
   `planar decision show <id> --json`, or
   `planar plan show <id> --json`. The Go verb re-resolves and
   re-hashes these as well — what the skill computes here is the LLM
   prompt's input, not the on-disk hash record.

6. Invoke the LLM at `temperature=0` with the template prompt plus
   the concatenated source bodies. The expected output is the
   refreshed doc body — markdown without a front-matter block, since
   the Go CLI prepends a refreshed provenance block.

7. Write the synthesised body to a tmpfile (e.g.
   `$TMPDIR/pl-doc-regenerate-<slug>.md`), then invoke
   `planar doc regenerate --path <path> --body-file <tmpfile>`
   (plus `--force` or `--merge` if the operator requested either).
   The Go verb composes the refreshed provenance front matter, atomic-
   writes the doc back to its original path (or to
   `<path>.regenerated.md` under `--merge`), and re-builds the
   manifest atomically at the end so successful per-target writes are
   reflected and failed ones leave the prior manifest entry intact.

## Drift handling

The four-signal classifier in `.manifest-docs` partitions every path
into one of four states:

- `regenerate-candidate`: the doc's declared sources have moved since
  the last synthesis. Regenerate is the intended response — sources
  are authoritative, the body should catch up. `--all` selects this
  signal exclusively.
- `hand-edit`: the doc body moved but the sources did not. The
  regenerator refuses by default, surfacing the file path and a hint:

  ```
  hand-edit detected on docs/features/foo.md; run with --force to overwrite or --merge to keep both
  ```

  `--force` instructs the verb to overwrite the hand edit. The
  original body is discarded. Use when the hand edit was a mistake or
  has already been captured upstream.

  `--merge` writes the freshly-synthesised body to
  `<path>.regenerated.md` as a sibling and leaves the original
  untouched. The operator reconciles the two via `vimdiff` or
  similar, then deletes the `.regenerated.md` sibling when done.
- `new-authoring`: the path exists on disk but the manifest does not
  know about it. Run `planar doc manifest update` to register it; the
  regenerator does not author from scratch.
- `deletion`: the manifest knows about a path that no longer exists.
  Either restore the file or rebuild the manifest; the regenerator
  does not handle this either.

## Output

A single line on stdout describing the write:

```
regenerated docs/features/auth-flow.md (kind feature, 2 source(s), manifest root a3f1c2d4e5b6f708)
```

Under `--merge` the message names both files so the operator knows
which to reconcile:

```
merged: wrote docs/features/auth-flow.md.regenerated.md; original docs/features/auth-flow.md left for manual reconciliation
```

In `--json` mode the verb emits:

```json
{
  "ok": true,
  "regenerated": [
    {
      "path": "docs/features/auth-flow.md",
      "kind": "feature",
      "hand_edit_detected": false,
      "action": "regenerated"
    }
  ],
  "skipped": [],
  "manifest_root": "1a2b3c4d5e6f7080"
}
```

## Authoring Conventions

This skill body adheres to the six rules established by task 602:

1. Quoted titles ("Title") not bare. The frontmatter `description`
   value is a double-quoted string.
2. Literal headings (`## What It Does`, `## CLI Commands`,
   `## When To Invoke`, `## How the Skill Composes`,
   `## Drift handling`, `## Output`, `## Vendor Notes`,
   `## Invocation`).
3. No nested bullets. Step lists use only top-level numbered items
   with prose; sub-letters are flat under the parent number where
   used.
4. No `## Out of this plan` H2. Deferred items belong in the tech
   spec's `## Out of scope` section.
5. Always double-quote `title:`-style values when this skill emits
   front matter elsewhere. The provenance front matter that
   `planar doc regenerate` writes follows this convention via the
   provenance.Format YAML emitter; the skill itself only writes a
   synthesised body to a tmpfile, never directly to a doc.
6. Workbench discipline: this skill MUST NOT read or write the
   workbench filesystem. It reads entity bodies through `planar
   artifact|decision|plan show --json`, writes a tmpfile, and invokes
   `planar doc regenerate` to commit the result.

## Vendor Notes

- Installed into `~/.codex/skills/pl-doc-regenerate` from `~/.planar/codex-skills/pl-doc-regenerate`.
- Source entities, template state, and manifest state come from the CLI; the skill must not read or write planar context outside it.
