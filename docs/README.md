# Documentation

Planar is a context plane for long-running agents: a local, durable store of the
state that agent work accumulates, and the protocol agents use to read and extend
it. The project [README](../README.md) states what it is. These pages explain how
it works and how to use it.

## Reading order

| If you are | Read, in order |
|---|---|
| New to Planar | [Overview](overview.md), then [Concepts](concepts.md), then [Getting started](getting-started.md) |
| Running agents on it | [Operations](operations.md), [Workflows](workflows.md), [Skill reference](skill-reference.md) |
| Looking something up | [CLI reference](cli-reference.md), [Glossary](glossary.md) |
| Changing Planar | [Architecture](architecture.md), [Testing](testing.md), [CONTRIBUTING](../CONTRIBUTING.md) |

## Understanding Planar

- [Overview](overview.md). The problem Planar addresses, the context plane, the
  three synchronization planes, handoff and resumption, and how Planar relates to
  an agent harness. Diagrams throughout.
- [Concepts](concepts.md). Precise definitions of scope, association, plan, task,
  claim, session, handoff, and the other terms the commands and tables use.
- [Glossary](glossary.md). Short definitions of recurring terms.

## Using Planar

- [Installation](../INSTALL.md). Prerequisites, building from source, and the
  full installer.
- [Getting started](getting-started.md). Initialize the database, create a plan
  and tasks, capture a session, hand off, and synchronize.
- [Light touch](light-touch.md). Capturing research and exploration with the
  base capture verbs, without the agent-driven workflow.
- [Workflows](workflows.md). End-to-end recipes for common operations.

## How work flows

- [Operations](operations.md). The three flows that carry most work: the
  specification pipeline, the orchestration lifecycle, and the claim ritual,
  plus the host build and test queue.
- [Lifecycles](lifecycles.md). Every state machine Planar enforces, as diagrams
  with the command that fires each transition.
- [Skill reference](skill-reference.md). The bundled skills and agent roles,
  grouped by purpose.

## Reference

- [CLI reference](cli-reference.md). Every command, its flags, and its exit codes.
- [Scope resolution](features/scope-resolution.md), [workspace agents](features/workspace-agents.md),
  and the [annotation consumer contract](features/annotation-consumer-contract.md).
  Specifications of individual features.

## Internals

- [Architecture](architecture.md). Storage model, the schema contract, the
  workbench, operational adapters, configuration, and repository layout.
- [Testing](testing.md). Test layers, what each gate proves, and the continuous
  integration tiers.
- [Toolchain parity](toolchain-parity.md). The pinned compiler and build tools.

## Project history and research

- [Design specification v0.1](planar-spec-v0.1.md) and the
  [architecture decision records](adrs.md). The reasoning behind the design.
- [Changelog](changelog.md).
- [Research instrumentation](research/README.md). Documentation of the
  measurement instrument that Planar includes.
