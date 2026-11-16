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

## Commits

- Sign off every commit (`git commit -s`).
- Stage explicit paths. Do not use `git add -A` or `git add .`.
- Do not add AI co-author trailers to commit messages.
