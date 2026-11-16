# Planar

*A context plane for long-running agents.*

Planar helps agents carry long-running work across sessions: specs, plans,
tasks, decisions and handoffs that outlast any one conversation.

This repository is a fresh start. It holds Planar as a plugin for Claude Code
and Codex, made of skills, agents and Markdown specs. There is no engine binary and no
database.

## What Planar was

Planar began as a local command-line tool backed by a single SQLite database.
Agents in any harness, such as Claude Code, Codex, Copilot or Gemini, used it
to record and share long-running work:

- specifications, plans and tasks;
- decisions, open questions and test scenarios;
- sessions and handoff records;
- claims with time-limited leases, so that two agents never worked on the same
  task at once;
- sync with Markdown workbench files and with issue trackers such as Jira and
  GitHub Issues.

The tool went through several implementations, the last a C++26 rewrite with
a Lua workspace format. Over time the engine came to cost more than it gave
back: its constraints shaped the agent workflows instead of serving them, and
small scripts could reproduce much of what it produced.

## What Planar is now

This repository keeps the ideas that worked and drops the engine:

- Markdown specs are the source of truth;
- skills and agents carry the workflow;
- the same plugin package carries host instructions for Claude Code and Codex.

Where an engine would clearly have helped, that gap is recorded rather than
patched over, so that any future tooling has to show it is needed first.

## Repository layout

The installable plugin lives in [`plugins/planar/`](plugins/planar/). Its
manifest, skills, agents, profiles and references are scoped to that directory.
The repository marketplace points to it, and its namespace remains `planar`.
Other plugins can live alongside it under `plugins/`, each with its own
manifest name.

## Installation and status

To install from the repository marketplace in Claude Code, add the marketplace
and install its plugin:

```sh
claude plugin marketplace add rdrsss/planar
claude plugin install planar@planar
```

For a local checkout, `make install-claude` adds this checkout as the
marketplace and installs its plugin; `make uninstall-claude` removes the plugin
while retaining its data. `make install-codex` and `make uninstall-codex` do
the same for Codex. These targets change the selected host's user configuration;
they leave the marketplace registered on uninstall. Run `make help` for the
no-model evaluation commands. A marketplace with the same name may already be
registered to another source; resolve that in the host CLI before installation.

Restart Claude Code to load it. For local development, run
`claude --plugin-dir ./plugins/planar` from the repository root. The plugin
contains the workflow instructions and components, but the Claude end-to-end
scenarios are still pending verification.

For Codex, `plugins/planar/plugin.json` is the portable root manifest alongside
the Claude manifest; both package the same skills, agents, profiles and
references. The documented installation experiment used codex-cli 0.169.0:
a scratch plugin was listed and added from a local marketplace with
`codex plugin list --available --json --marketplace <marketplace>` and
`codex plugin add <plugin>@<marketplace> --json`, using process-local
marketplace configuration. An explicitly supplied installed skill then read a
bundled reference from the cache. This is a procedure for the scratch probe,
not a verified installation recipe for this Planar package. Installed Planar
discovery and role dispatch still need live checks. See the
[host contract](plugins/planar/references/hosts.md) for the evidence and
limits.

The intended Codex scope is the serial minimal skill-mode loop: one pinned
worker, independent checks and a separate pinned reviewer. The full Planar
loop, recovery in both host directions and Claude end-to-end scenarios remain
unverified; required unavailable checks stay pending. Richer Codex profiles,
autonomy and workflow mode are deferred.

See [architecture](docs/architecture.md) and [methodology](docs/methodology.md)
for the components and workflow.

## Acknowledgement

The original Planar, with its full history, source, design documents and
specifications, is preserved read-only at
[rdrsss/planar-archive](https://github.com/rdrsss/planar-archive). Much of
what is here was learned there. Its
[acknowledgements](https://github.com/rdrsss/planar-archive#acknowledgements)
thank the people who shaped it.
