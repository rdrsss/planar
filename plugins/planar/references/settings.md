# Settings

The single definition of the Planar settings file: every key, its default, and
how `<home>`, the policy in force and the profile in force are resolved.
Implements PRD-R22 and PRD-R26 and TS-4 (TS-4.1 to TS-4.7).

## The file

Planar settings live in one YAML file, `<home>/settings.yaml`. Read it with
the ordinary Read tool. The operator edits it; you never do unless asked.

- Every key is optional. With no file, or a missing key, use the default.
- Never rewrite or "fix" the settings file unasked.
- Planar puts no files in a project's repository unless the operator turns on
  replication for that project (see `replication` below).

## Resolving `<home>`

Resolve `<home>` before reading anything else:

1. If the environment variable `PLANAR_HOME` is set and names a directory, that
   directory is `<home>`.
2. Otherwise `<home>` is `~/.planar/`.

`PLANAR_HOME` replaces `<home>` as a whole: the settings file, the profiles
directory and the default `projects_dir` all move with it. When `<home>` is not
`~/.planar/`, tell the operator which directory you are using.

## Unknown keys and bad values

Report an unknown key or a bad value (wrong type, or a value outside the
allowed set) to the operator, name the key, and use that key's default. Carry
on with the rest of the file. Do not edit the file. `worktrees_dir` is an
exception: invalid values are refused before worktree creation, without fallback
(TS-4.7).

## Defaults

This is the whole default configuration; it applies wherever the file or a key
is missing:

```yaml
version: 1
projects_dir: <home>/projects
worktrees_dir: <project directory>/worktrees  # explanatory default, not literal YAML
naming:
  method: context
orchestration:
  mode: skill
profile: minimal
policy: default
autonomy:
  enabled: false
  escalations: 3
policies:
  default:
    commit: per-task
    worktree: project
projects: {}
```

## Top-level keys

| Key | Type | Allowed values | Default | Meaning |
| --- | --- | --- | --- | --- |
| `version` | integer | `1` | `1` | Settings format version. |
| `projects_dir` | path | any directory | `<home>/projects` | Directory under which each Planar project gets its own directory, unless the project sets its own `path`. |
| `worktrees_dir` | path | absolute or `~/` path | `<project directory>/worktrees` | Parent for named managed worktrees; see resolution below. |
| `naming.method` | string | `context`, `tracker` | `context` | Preferred way to name a new project: `context` slugs the operator's description of the work (the repository name if that gives nothing better); `tracker` uses the tracker key plus a short task slug, and applies only when the work comes from a tracker issue. A name the operator gives always wins. |
| `orchestration.mode` | string | `skill`, `workflow` | `skill` | Whether the orchestrating loop runs as a skill or as a workflow. |
| `profile` | string | name of a profile | `minimal` | The profile in force for projects that name none. |
| `policy` | string | name in `policies` | `default` | The policy in force for projects that name none. |
| `autonomy.enabled` | boolean | `true`, `false` | `false` | Whether unattended (autonomous) running is allowed. |
| `autonomy.escalations` | integer | whole number | `3` | How many decisions an autonomous run may take for the operator. |
| `policies` | map | policy name to rules | built-in `default` | Named rule sets; see Policies. |
| `projects` | map | project name to overrides | `{}` | Per-project overrides; see Projects. |

## Policies

A policy is a named set of rules for how work is carried out. A policy says
what is allowed; a profile says how work is staged. Skills and agents name the
rule they depend on and never fix its value: read the rule from the policy in
force.

`policies` maps a policy name to its rules. There are two rules:

| Rule | Allowed values | Default (in `default`) | Meaning |
| --- | --- | --- | --- |
| `commit` | `per-task`, `none` | `per-task` | `per-task`: commit each approved task, without pushing. `none`: do not commit; leave changes in the working tree. Whichever value is in force, Planar never pushes or merges; the operator does. |
| `worktree` | `project`, `none` | `project` | `project`: use the selected named git worktree, by default `<project directory>/worktrees/wt-<project>/`, on its own branch. `none`: no separate worktree; work in the repository's own checkout. The project directory (`<home>/projects/<project>/`) holds specs and records, not the code. |

- `default` is built in and the operator can redefine it. A redefined `default` that leaves a rule out keeps the built-in value for that rule.
- Any other policy lists only the rules it changes; every rule it omits takes
  the value from `default`.
- There is no revision-limit rule in a policy; revision limits belong to the
  profile. Treat a `max_attempts` rule as an unknown key.

Example, a policy that works in the repository's own checkout and inherits `commit: per-task`:

```yaml
policies:
  in-place:
    worktree: none
```

### Policy in force

For a given project, pick the first of these that is set:

1. the project's own `policy`,
2. the top-level `policy`,
3. `default`.

If the named policy does not exist in `policies`, report it as a bad value and
use `default`.

## Profile in force

Resolved the same way as the policy: the project's own `profile`, else the
top-level `profile`, else `minimal`. Look for a named profile in
`<home>/profiles/` first, then in the plugin's own `profiles/` directory. If
neither has it, report a bad value and use `minimal`.

## Projects

`projects` maps a project name to overrides for that project. Every field is
optional.

| Key | Type | Allowed values | Default | Meaning |
| --- | --- | --- | --- | --- |
| `path` | path | any directory | `<projects_dir>/<project>` | Where this project's directory lives. `~` is expanded. |
| `worktrees_dir` | path | absolute or `~/` path | top-level `worktrees_dir`, else project `worktrees/` | Project override for the managed worktree parent. |
| `profile` | string | name of a profile | top-level `profile` | Profile in force for this project. |
| `policy` | string | name in `policies` | top-level `policy` | Policy in force for this project. |
| `spec_lenses` | list of strings | lens names, for example `[api]` | none (empty) | Extra review lenses applied to this project's specs. |
| `replication.enabled` | boolean | `true`, `false` | `false` | Whether this project's Planar files are also copied into its repository. |
| `replication.target` | path | relative path inside the repository | `docs/planar/<project>` | Where the copy goes. Used only when replication is enabled. |

Example:

```yaml
projects:
  widgets:
    path: ~/work/planar-widgets
    worktrees_dir: ~/work/trees/widgets
    profile: basic
    policy: in-place
    spec_lenses: [api]
    replication:
      enabled: true
      target: docs/planar/widgets
```

## Worktree parent resolution

For approval, run/resume and replication on both hosts, resolve
`projects.<name>.worktrees_dir`, then top-level `worktrees_dir`, then the
selected project directory's `worktrees/` (TS-4.7). Configured values must be
nonempty absolute paths or begin with `~/`; expand only the current user's
home and canonicalize. Refuse relative paths, other-user tilde, environment
substitution, invalid types and invalid or escaping destinations before
mutation; never silently fall back or rewrite settings. Preserve spaces in
paths. `worktree: none` remains in-place and creates no managed tree. Resume
uses the verified recorded path and branch, including legacy `worktree/`,
and reports a changed parent rather than creating or moving a tree (TS-19.10).
