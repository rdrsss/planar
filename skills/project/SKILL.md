---
name: project
description: "Planar plugin: start a Planar project. Resolves the settings, chooses and confirms the project name, checks for existing projects on the same repository and for a name already taken, then creates the project directory with project.yaml and context.md, and says what is next. Use when the operator asks for a new project, or to start work on something in a repository, as /planar:project."
---

# Create a project

Provenance: implements PRD-R17, PRD-R21, PRD-R22 and PRD-R25; TS-3.1,
TS-3.3, TS-3.4, TS-3.5, TS-4.1, TS-4.2 and TS-4.3; layout entry LAY-1.3.1.
The plugin's references are the definitions and are not repeated here: read
the files named below in full and follow them exactly.

Use only file reads and writes, shell commands and subagent dispatch. This
skill needs no subagent.

## Rules

- **Write nothing before the operator has answered.** Every question below
  ends your reply. Until the operator has answered each question that
  applies, create no file and no directory, anywhere.
- **Never write into the repository.** The project lives under the projects
  directory only. Run no command that changes the repository.
- **Never edit the settings file.** Not to fix a bad value, not to add a
  key (TS-4.3).
- **Never pick a name or reuse a directory unasked** (PRD-R25).

## 1. Resolve the settings

Follow `${CLAUDE_PLUGIN_ROOT}/references/settings.md`.

1. Resolve `<home>`: the directory `PLANAR_HOME` names if it is set,
   otherwise `~/.planar/`. When it is not the default, say which directory
   you are using (TS-4.2).
2. Read `<home>/settings.yaml` if it exists. With no file, every default
   applies. Every key is optional (TS-4.1).
3. Take `projects_dir` (default `<home>/projects`) and `naming.method`
   (default `context`). The project's own `projects.<name>.path`, if
   settings give one for the name chosen in step 3, replaces
   `<projects_dir>/<name>` as the project directory.
4. Report an unknown key or a bad value, naming the key and the value, and
   use the default for it (TS-4.3). For example `naming.method: banana` is
   not valid: say that the value "banana" is not valid, and go on with
   `context`. Put the report in your final reply, and in any earlier
   question you ask.

## 2. Gather the work

Take from the operator's words the statement of the work, a name if they gave
one, and an issue key and title if the work comes from a tracker. Find the
repository: run `git rev-parse --show-toplevel` and
`git remote get-url origin` from the directory the operator is working in.
Both are reads. The remote may not exist; the repository itself may be
absent, and then say so and ask which repository the work is in.

## 3. Choose the name

Follow "Naming a project" in
`${CLAUDE_PLUGIN_ROOT}/references/project.md`; TS-3.4 and PRD-R21 apply.

1. A name the operator gave. It always wins, whatever `naming.method` is.
   Record `naming: operator`.
2. Otherwise, if the work is a tracker issue and the method in force is
   `tracker`: `<KEY>-<slug of the title>`, for example `ABC-123` and
   "Add export" give `ABC-123-add-export`. Record `naming: tracker`.
3. Otherwise a slug of the operator's statement, or the repository name when
   the statement gives nothing better, by the slug rule in that section.
   Record `naming: context`.

Tell the operator the name before you write anything. When the name was
derived (cases 2 and 3), ask them to confirm it and end your reply; a
different name from the operator is case 1. When the operator gave the name,
state it and go on.

## 4. Collisions

Follow "Creating a project: collisions" in
`${CLAUDE_PLUGIN_ROOT}/references/project.md`; TS-3.5 and PRD-R25 apply. Do
these in order and write nothing until the operator has answered.

1. List the directories under `projects_dir`. Read each one's `project.yaml`
   (a read only) and list those whose `repository` matches this one: the
   same `remote`, or, when there is no remote, the same `path`. If there are
   any, show them by name and status and ask whether to continue one of them
   or start a new project. End your reply. Change nothing in them. If the
   operator chooses to continue one, create nothing; point them to
   `/planar:draft` for that project and stop.
2. If the chosen name is already a directory under the projects directory (or
   at the project's own `path`), ask what to do and offer the name with a
   number suffix as an example, such as `colours-2`. End your reply. Do not
   choose the new name yourself and do not reuse the directory unless the
   operator says to. A name the operator gives afterwards is checked again
   from this step.

Both questions may be put in one reply when both apply.

## 5. Create the project

Only now write anything. Create the project directory (`mkdir -p`) and these
two files in it, in the formats of the sections "`project.yaml`" and
"`context.md`" of `${CLAUDE_PLUGIN_ROOT}/references/project.md` (TS-3.1,
TS-3.3). Take each value from here:

- `project.yaml`: `name` and `naming` from step 3; `created` from
  `date +%Y-%m-%d`; `repository.path` and `repository.remote` from step 2
  (no remote key when there is none); `spec_set` as the reference gives it
  for a new project, with `spec_lenses` empty: `project` never fills it,
  and never copies lenses from settings.
- `context.md`: `<name>` from step 3; the operator's statement of the work in
  their own words, quoted, in the statement section; the other sections
  empty. When the operator gave an issue, put its key and title in the
  statement.

Write with quoted heredocs (`<<'PLANAR_CONTEXT'`) so that backticks and `$`
in the operator's words are kept as they are. Create no other file: no
`worktree/`, no spec, no `ids.md`, and nothing in the repository or in
`<home>` outside the project directory. Never write replication copies; the
settings' `replication` is used by later steps.

## 6. Finish

Reply with: the project name and how it was chosen, the project directory
relative to `<home>` when it is inside it (otherwise as given in settings),
the directory in use when `<home>` is not the default, every setting problem
reported in step 1 (name the key and the value), and what is next: the
operator describes the work in detail and runs `/planar:draft` for this
project to draft the specs. Do not start drafting.
