# `planar`

The operator CLI for planning and local administration. `main.cppm`
assembles the root CLI11 `CLI::App` in its pinned declaration order;
`main.cpp` constructs the invocation and dispatches it. Each
`handlers/<name>/` directory owns a root command family, its child apps,
configuration, and command-specific implementation. `shared/` contains
helpers used by multiple families.

| Handler directory | Root CLI app and responsibility |
| --- | --- |
| `init/` | `init`: initialize the local Planar database and project. |
| `scope/`, `assoc/` | `scope`, `assoc`: select scope and manage association membership. |
| `plan/`, `task/` | `plan`, `task`: planning hierarchy, steps, task lifecycle and dependencies. |
| `question/`, `scenario/`, `decision/` | Questions, test scenarios and decisions. |
| `artifact/`, `annotate/` | Planning artifacts and retained annotations. |
| `promote/`, `demote/` | Move personal entities into or out of an association scope. |
| `workbench/`, `workspace/` | Bidirectional drafting filesystem and workspace registration/scan. |
| `link/`, `unlink/`, `links/` | Entity and repository relationships. |
| `resume/`, `handoff/`, `capture/` | Resume readiness, durable handoff and session capture. |
| `audit/`, `health/`, `dashboard/` | Audit operations, health checks and summary views. |
| `models/` | Model and routing configuration views. |
| `spec/`, `test_spec/` | Specification ingestion and test-spec operations. |
| `config/`, `templates/` | Configuration plane and propagation templates. |
| `tree/`, `search/`, `groups/` | Hierarchical, search and grouped read views. |
| `local/`, `skills/` | Operator-local assets and installed skill views. |
| `import/`, `synthesize/` | Import existing planning content and synthesize new artifacts. |
| `report/`, `feedback/` | Issue reporting and feedback triage. |
| `bench/`, `run/` | Measurement bench and run records. |
| `closure/` | Feature closeout computation and evidence. |
| `workflow/` | Workflow definitions and operations. |
| `explore/` | Exploration command. |
| `version/`, `completion/`, `schema/` | Version, shell completion and the flat command catalog. |
| `shared/` | Shared handler helpers; no root CLI app. |

The state table is in [`src/engine/README.md`](../../engine/README.md#state-inventory).
Detailed command syntax is in
[`docs/cli-reference.md`](../../../docs/cli-reference.md).
