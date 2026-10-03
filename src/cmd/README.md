# Command binaries

Each binary has its own entry point and command tree. The `handlers/` directory
under a binary groups one root command family per directory; related child
commands sit in that family. `main.cppm` assembles the CLI11 `CLI::App` tree
where the binary uses one. `planar-execute` has a manual parser in
`handlers/shared/cli.cppm` and dispatches from `main.cpp`. Handlers translate CLI requests into calls to
[`src/engine/`](../engine/README.md).

| Directory | Binary and capability |
| --- | --- |
| [`planar/`](planar/README.md) | Operator CLI. Creates and updates planning entities and owns manual task transitions. |
| [`planar-agent/`](planar-agent/README.md) | Agent coordination CLI. Claims work, writes actions and dispatch evidence, and changes task status as part of atomic claim operations. |
| [`planar-watch/`](planar-watch/README.md) | Read-only activity viewer; its SQLite connection is opened read-only. |
| [`planar-execute/`](planar-execute/README.md) | Workflow configuration, local run, and Centurion client commands (refused by this build); no SQLite handle. |
| [`planar-ext/`](planar-ext/README.md) | External system and sync CLI. SQLite authorizer permits writes only to `external_links`, `external_systems` and `sync_events`. |
| [`internal/`](internal/README.md) | Shared invocation context, environment/config path resolution and injected lazy database holder for the database-using binaries. |
| [`integration_tests/`](integration_tests/README.md) | Multi-command lifecycle scenarios and their coverage inventory. `planar`-only cases share an injected in-memory database; cross-binary cases use a scratch database file. |

`CMakeLists.txt` registers the command targets. Binary targets do not link
other binary targets. The shared parser, help renderer, schema catalog and
completion machinery live in `src/lib/cliapp/`; database connections and
migrations live in `src/lib/db/`. See
[`docs/cli-reference.md`](../../docs/cli-reference.md) for the user-facing
command contract.
