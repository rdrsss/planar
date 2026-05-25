---
doc_kind: changelog
template_version: 1
synthesis_voice: aggregator
required_sources: [changelog_entry]
optional_sources: []
output_shape: A release-tagged changelog grouping changelog_entry artifacts under H2 headings per release, with bulleted entries per release describing user-visible changes.
---

# Synthesis prompt — changelog

You are producing the project's user-facing changelog. The body is an
aggregation of `kind=changelog_entry` artifacts grouped by release
tag.

## Inputs

- `{{title}}` — the doc title (typically "Changelog").
- `{{slug}}` — file slug.
- `{{kind}}` — always `changelog`.
- `{{template_version}}` — always `1`.
- `{{entries}}` — every `kind=changelog_entry` artifact, pre-grouped
  by release tag and pre-sorted newest release first. Each release
  block is rendered as a YAML-ish header (`release: v0.4.0`, `date:
  2026-05-20`) followed by a list of entries, each with `title`,
  `category` (one of `added | changed | fixed | removed | deprecated
  | security`), and `body`. Release blocks are separated by
  `\n\n---\n\n`.

## Voice and shape

- Past tense, third-person ("Added X.", "Fixed Y when Z.").
- One bullet per entry. Lead with the verb matching `category`.
- Group within a release by category, in the order: Added, Changed,
  Fixed, Removed, Deprecated, Security. Omit empty categories.

## Required sections

1. **`# {{title}}`** — title heading.
2. One `## <release tag> — <date>` H2 per release, newest first.
3. Under each release, H3 per category (`### Added`, `### Fixed`,
   etc.) with bulleted entries.

## Output

Emit only the markdown body. The caller computes the provenance front
matter. No preamble.
