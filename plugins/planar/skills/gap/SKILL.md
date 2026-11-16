---
name: gap
description: "Planar plugin: append one entry to a project's gap log (gaps.md) recording a step that would have been safer, faster or simpler with an engine, a database or a compiled format, with the evidence. Use when the operator asks to record a gap, or when a Planar step names one, as /planar:gap."
---

# Record a gap

Before acting, select the host. On Claude Code, read
`${CLAUDE_PLUGIN_ROOT}/references/hosts.md`. On Codex, use this discovered
installed skill file's directory and read `../../references/hosts.md` relative
to it. Follow that contract for all subsequent resource resolution and
dispatch. If the file or a required capability is unavailable, stop and report
it before the affected action.

Implements PRD-R7 (gap log) and TS-12 (gap log). A gap is a step that would
have been safer, faster or simpler with an engine, a database or a compiled
format. You record the gap and the evidence for it. You do not build the
tooling, and you do not propose building it here.

Use only file reads and writes and shell commands.

## 1. Find the project directory

1. Resolve `<home>` and `projects_dir` as
   `${CLAUDE_PLUGIN_ROOT}/references/settings.md` says: `PLANAR_HOME` if set,
   otherwise `~/.planar/`. Read `<home>/settings.yaml` if it exists. Do not
   edit it.
2. The project directory is defined in
   `${CLAUDE_PLUGIN_ROOT}/references/project.md`: `<projects_dir>/<project>/`,
   or the project's own `path` from settings.
3. Use the project the operator or the calling step names. If it is not
   clear which project is meant, list the directories under `projects_dir`
   and ask. If a project is named, do not ask anything.

Stop and tell the operator if the project directory does not exist. Do not
create it.

## 2. Pick the number

`gaps.md` is in the project directory. Find the highest `G-<n>` among its
`### G-` headings and use the next number, zero-padded to three digits
(`G-001`, `G-014`). If the file does not exist or has no entries, use `G-001`.
Take the number from `gaps.md` alone. A `G-` number is not added to
`ids.md`. Prefix rules are in
`${CLAUDE_PLUGIN_ROOT}/references/conventions.md`.

## 3. Write the entry

If `gaps.md` does not exist, create it with `# Gap log` as its first line.
Then append, after a blank line, exactly this form:

```markdown
### G-004 Record and files disagreed after an interrupted review
- date: 2026-11-20
- step: TS-7.2
- what happened: …
- cost: …
- what would have helped: …
```

- The heading is `### G-<n>` and a short title of the gap.
- `date` is today, from `date +%Y-%m-%d`.
- `step` is the step the gap arose in: a spec section ID (`TS-7.2`), a check
  or scenario ID, or a task ID, as the operator or caller gave it. If none
  was given, write `not stated`.
- `what happened` is what occurred, in the operator's words as far as
  possible, with the evidence: file, command or ID.
- `cost` is what it cost, such as time, redone work or a wrong result, as the
  operator or caller stated it. If none was stated, write `not stated`.
- `what would have helped` is the capability that was missing, stated as a
  need and not a design, as the operator or caller stated it. If none was
  stated, write `not stated`.
- Give every field a value. A field the operator or caller did not state is
  written `not stated`. Never infer or invent a value for any field, `cost`
  and `what would have helped` included; do not ask for it.

Append with a shell redirect (`>>`) from a quoted heredoc
(`<<'PLANAR_GAP_ENTRY'`), so that backticks and `$` in the entry are written
as they are and not run by the shell. Use a delimiter such as
`PLANAR_GAP_ENTRY`, and pick another if that exact line occurs in the entry
text, because it would end the heredoc early. The blank line after the first
line keeps the entry apart from the one before it:

```sh
cat >> "<project directory>/gaps.md" <<'PLANAR_GAP_ENTRY'

### G-<n> <title>
- date: <YYYY-MM-DD>
- step: <step>
- what happened: <text>
- cost: <text>
- what would have helped: <text>
PLANAR_GAP_ENTRY
```

Never use an unquoted heredoc (`<<PLANAR_GAP_ENTRY`) or `echo` with double quotes for the
entry. Entries are appended and never edited:
do not change, reorder or delete an existing entry, even a wrong one. Record
a correction as a new entry.

## 4. Finish

Write nothing else: not `ids.md`, `project.yaml`, a spec, a task record or
any other file, and build no tooling in response. Tell the operator the
entry's ID and the path of `gaps.md`.
