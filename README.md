# Planar

*A context plane for long-running agents.*

Planar helps agents carry long-running work across sessions: specs, plans,
tasks, decisions and handoffs that outlast any one conversation.

This repository is a fresh start. It holds Planar as a Claude Code plugin,
made of skills, agents and Markdown specs. There is no engine binary and no
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
- the plugin is distributed through Claude Code's plugin system.

Where an engine would clearly have helped, that gap is recorded rather than
patched over, so that any future tooling has to show it is needed first.

## Acknowledgement

The original Planar, with its full history, source, design documents and
specifications, is preserved read-only at
[rdrsss/planar-archive](https://github.com/rdrsss/planar-archive). Much of
what is here was learned there. Its
[acknowledgements](https://github.com/rdrsss/planar-archive#acknowledgements)
thank the people who shaped it.
