---
name: replicate
description: "Planar plugin: copy a project's Planar files into its repository as regular files, leaving out project.yaml and the worktree, writing a .planar-replica marker and replacing machine paths with placeholders. Used by the run and review steps when replication is on for the project, at approval of the spec set and when a task is approved; not called by the operator directly. Never copies back."
---

# Replicate a project into its repository

Before acting, select the host. On Claude Code, read
`${CLAUDE_PLUGIN_ROOT}/references/hosts.md`. On Codex, use this discovered
installed skill file's directory and read `../../references/hosts.md` relative
to it. Follow that contract for all subsequent resource resolution and
dispatch. If the file or a required capability is unavailable, stop and report
it before the affected action.

Implements PRD-R23 (replication) and PRD-Q23 (what is copied and committed),
with TS-11.4 (the replicate step), TS-19.2 (the working tree it runs after)
and TS-10; layout entry LAY-1.3.6. It is followed by the orchestrating actor
(the `run` skill), or by `review` at approval. The settings and the project
directory are defined in the references named below, which you read and
follow; they are not restated here.

Use only file reads and writes and shell commands.

## 1. When it runs

Run this step only if replication is on for the project: `replication.enabled`
is `true` in the project's entry in settings
(`${CLAUDE_PLUGIN_ROOT}/references/settings.md`). It is off by default, and
when it is off you do nothing and write nothing (PRD-R23).

It runs:

- when the operator approves the spec set, after the working tree is prepared
  (TS-19.2); `review` calls it at "On approval";
- when each task is approved, after the task is ticked
  (`${CLAUDE_PLUGIN_ROOT}/references/loop.md`, step 13).

Resolve `<project directory>` as
`${CLAUDE_PLUGIN_ROOT}/references/project.md` says. The working tree root is
`<tree>`, from `git rev-parse --show-toplevel` run in the working tree.
The target is `replication.target`, default `docs/planar/<project>`.

## 2. Check the target

Do this before anything is written, and on every run of the step.

1. The target must be a relative path inside the working tree. If it is
   absolute, contains `..`, or resolves outside `<tree>` (for example through
   a link), stop and tell the operator. Copy nothing.
2. `<tree>/<target>` does not exist, or is an empty directory: it is a first
   copy; go on.
3. `<tree>/<target>/.planar-replica` exists and its first line is
   `project: <project>`: it is this project's replica; go on.
4. Anything else, that is, the target exists and holds files without that
   marker, or the marker names another project: do not write there. Ask the
   operator, naming the target and what is in it, whether to use another
   target or to clear it themselves, and stop. Leave every file in it
   unchanged. The operator changes `replication.target` in settings; you do
   not edit settings.

## 3. Copy

Copy the project directory to `<tree>/<target>/` as regular files, never as
links, leaving out `project.yaml` and `worktree/` (PRD-Q23). Run from the
project directory, for example (`-h` makes `tar` follow symbolic links and
store what they point to, so every copied entry is a regular file):

```sh
mkdir -p "<tree>/<target>"
tar -h --exclude='./project.yaml' --exclude='./worktree' -cf - . | tar -C "<tree>/<target>" -xf -
```

Afterwards check that the copy holds no link, for example with
`find "<tree>/<target>" -type l`; if it prints anything, replace each link
with a copy of what it points to.

Then, on a first copy, write the marker `<tree>/<target>/.planar-replica`
with one line, `project: <project>`.

The copy goes one way. Never copy from the repository back to the project
directory, and never edit the project directory.

## 4. Replace machine paths

In every regular text file of the copy, replace the absolute path of the
working tree with `<worktree>` and then the absolute path of the user's home
directory with `~`, in that order, because the working tree may lie under the
home directory. Do it with a shell command, for example `sed` writing to a
temporary file that then replaces the original, with the paths escaped for
`sed`. Do not edit the files by hand. The marker is left as written. The
purpose is that check output in the repository carries no machine path
(AGENTS.md, "Working rules").

## 5. Commit

Apply the policy's `commit` rule
(`${CLAUDE_PLUGIN_ROOT}/references/settings.md`).

- `per-task`: stage the target by explicit path (`git add -- <target>`),
  never `git add -A`, and never push.
  - At approval of the spec set, make a commit of its own that holds only
    the target. Follow the repository's own commit rules as
    `${CLAUDE_PLUGIN_ROOT}/references/loop.md`, step 13, says.
  - When a task is approved, do not commit here: the task's commit holds the
    copy together with the task's `touches` paths (step 13 of the loop). If
    the copy changed nothing, there is nothing to stage.
- `none`: do not commit; leave the copied files in the working tree.

Report to the caller the target, whether it was a first copy, and the paths
that changed.
