# Local skills, agents and workspaces

Two machine-local lifecycles: operator-authored skills and agents (`planar local`), and
polyrepo workspaces (`planar workspace`). Both write files on this machine. Neither takes
a planning scope, and neither should ever be done by hand-editing the files they manage.

## Where things go

| Content | Location |
|---------|----------|
| Operator-local skill and agent sources | `~/.planar/local/skills/` and `~/.planar/local/agents/`, managed by `planar local` |
| Specs, roadmaps, ADRs, research | Planar artifacts, through `planar artifact` and the workbench |
| Plans, tasks, claims, decisions, questions | The Planar database, through the CLIs only |
| Workspace routing and guidance | Generated under `~/.planar/workspaces/<org_id>/` |
| Operator routing changes | `~/.planar/workspaces/<org_id>/routing-table-overrides.json` |

Never commit local sources to a project repository, and never author planning documents
as loose Markdown in a repository when they belong in Planar.

## Local skills and agents

Pick one operation before touching anything: import, link, list, unlink, migrate or
repair. If the request is ambiguous, ask which.

| Operation | Command | Notes |
|-----------|---------|-------|
| import | `planar local import <path>` | A flat `.md` file, a directory holding `SKILL.md`, or a flat collection of both. Links to vendors unless `--no-link`. |
| link | `planar local link [<name>]` | Installs one source, or all, for each vendor. |
| list | `planar local list` | The proof of state: each install with name, kind, vendor, status and path. |
| unlink | `planar local unlink <name>` | Removes the installs and keeps the source, unless `--purge`. |
| migrate | `planar local migrate` | Converts legacy-shaped sources; preview with `--dry-run`. |
| repair | `planar local link --reconcile` | There is no `local repair` verb. Takes no name or vendor filter. |

Flags: see `planar local <verb> --help`.

### Procedure

1. Run the mutation once with `--json` and keep its action rows.
2. Verify with `planar local list --json`:
   - after import with linking, or after link: every expected install is `live`;
   - after import with `--no-link`: report the sandbox destination; no install row is
     expected yet, and the next step is `planar local link <name>`;
   - after unlink: no install row remains for the name;
   - after migrate: report migrated and skipped rows; migration does not create missing
     vendor installs;
   - after repair: report any install still `broken` or `missing`.
3. `--dry-run` is a preview: report proposed actions with zero applied.

### Rules

- Installs carry a visible `local-` prefix. Only a source whose front matter sets
  `shadow: true` installs under its bare name; never add that on the operator's behalf.
- `import --force` replaces an existing sandbox entry and `unlink --purge` deletes the
  sandbox source. Require clear intent for either, naming the collision or the source.
- Never delete or modify the external path an import came from.
- Repair cannot restore a source that disappeared. It removes the stale manifest row and
  the vendor installs because nothing is left to install from.
- Do not hand-author vendor links or edit the link manifest; `planar local` is the only
  access layer.
- There is no `planar local promote`. Making a local skill part of Planar is a manual
  contribution to the Planar repository through its normal review.
- An already-installed or already-absent target is a successful no-op.

## Workspaces

A workspace groups sibling repositories under one organisation and generates shared
routing and guidance for agents working across them.

| Operation | Commands |
|-----------|----------|
| init | `planar workspace init` from the workspace root. |
| doctor | `planar workspace doctor` checks every registered workspace; it takes no target. |
| routing show | `planar workspace routing show [<workspace>]` (read-only). |
| routing build | `planar workspace routing build [<workspace>]`, then `routing show` to verify. |
| regenerate | Confirm routing exists with `routing show`, then `planar workspace regenerate [<workspace>]`. |
| scan (the default refresh) | `routing build`, `routing show`, then `regenerate`. |
| repair | `doctor`, `routing build`, `routing show`, `regenerate`, then `doctor` again. |

Flags, including `--enrich` on `init` and `routing build`: see
`planar workspace <verb> --help`.

### Procedure

1. Run `planar scope show --json` before any scoped write. The cwd must resolve inside the
   intended workspace, or pass the workspace as the positional where the verb takes one.
   With several organisation workspaces registered, do not guess; ask.
2. If the request could mean initialising a new workspace or refreshing an existing one,
   stop and present the two choices.
3. Run each stage with `--json`; stop stages that depend on a failed one, but still run
   the read-only checks that show the persisted state.
4. Report the post-state from the CLI: for init, the organisation and memberships; for
   doctor and repair, issues found and repaired per organisation; for routing, path,
   project count and dependency edges; for regenerate, the canonical guidance path,
   project count and bytes written.
5. A preview is a read: run `scope show` and `routing show`, then list the commands that
   would run, with zero applied. The write verbs have no dry-run flag.

### Rules

- The workspace-root `AGENTS.md` and `CLAUDE.md` in a sibling-repository workspace are
  generated links (or copies) of canonical state. Never hand-edit them; regenerate.
  A meta-repository's own root instruction files belong to that repository, and doctor
  leaves them alone.
- Never edit the generated `routing-table.json` or the generated canonical `AGENTS.md`.
  Put routing changes in `routing-table-overrides.json` and rebuild.
- `init` that returns a pipeline warning is partial: registration may already be
  committed. Name it; do not claim a rollback.
- There is no workspace destroy verb in this lifecycle; do not remove a workspace or an
  association to "reset" it.
- Recovery per stage: unresolved target, `planar workspace routing show <workspace>`;
  missing state, `planar workspace doctor`; stale routing,
  `planar workspace routing build <workspace>`; stale guidance,
  `planar workspace regenerate <workspace>`.

Report results with the envelope in [feedback-contract.md](feedback-contract.md).
