# AGENTS.md

Guidance for coding agents working in this repository.

## What this repository is

Planar is a Claude Code plugin for long-running, spec-driven agent work. It is
made of skills, agents and Markdown specs. It has no engine binary, no
database and no compiled workspace format.

The earlier Planar, a command-line tool backed by SQLite, is archived
read-only at [rdrsss/planar-archive](https://github.com/rdrsss/planar-archive).
Use it as a reference for ideas and past decisions. Do not copy its engine,
binaries, migrations or Lua compile step into this repository.

## Repository layout

- `plugins/planar/` is the installable Planar plugin root. Keep its manifests,
  skills, agents, references, profiles, workflows and examples inside it.
- `.claude-plugin/marketplace.json` at the repository root is the marketplace
  catalog; its Planar entry points to `./plugins/planar`.
- Repository guidance, the README, licence and `docs/` stay at the repository
  root. Give additional plugins their own directories under `plugins/` and
  manifest namespaces.

## Working rules

- Keep it engine-free. If a task seems to need an engine, a database or a
  compiled format, record the gap and the evidence for it instead of building
  one.
- Markdown specs are the source of truth. Skills and agents should cite the
  spec sections they implement.
- The plugin keeps each project's specs and task records under
  `~/.planar/projects/<project>/`. This repository's own specs are at
  `~/.planar/projects/planar/`. Work only inside the project directory you
  were given, and never delete another project's directory.
- Keep this repository free of machine-specific paths, credentials and
  personal details.

## Development commands

Run `make` or `make help` from the repository root or worktree holding the
Makefile. Generic `make install` and `make uninstall` require `HOST=codex` or
`HOST=claude`; explicit host targets are listed in help. Do not mutate user
installations or configuration just to test wrappers. An existing explicit or
standing user authorization for the same scope applies; do not request it again.
The user instruction prevails.

The `evals/` workspace is outside the installed plugin. Use `make evals-setup`
for locked dependencies, `make evals-test` or `make evals` for model-free tests,
and `make evals-plan` for preflight (override `CASE` when needed). Live
`make evals-run` requires explicit authorization and a matching existing review
gate; preserve the gate and recovery checks. See [evaluation guidance](evals/README.md)
for output roots, gate preparation and recovery. Keep secrets and ignored
outputs out of commits and preserve the engine-free design.

## Commits

- Sign off every commit (`git commit -s`).
- Stage explicit paths. Do not use `git add -A` or `git add .`.
- Do not add AI co-author trailers to commit messages.
