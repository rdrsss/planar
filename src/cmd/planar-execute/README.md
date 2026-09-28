# `planar-execute`

Workflow entry point and client. `main.cpp` classifies verbs with the manual
parser in `cli.cppm` and dispatches into `handlers/<name>/command.cppm`.
These are command handlers, not CLI11 `CLI::App` objects. This binary has no
SQLite handle. The local `run` path remains available until the Centurion
cutover described by decision 1007 / plan 1033.

| Handler directory | Command |
| --- | --- |
| `run/` | Execute one phase of a local Lua workflow. |
| `profile/` | Resolve and display the selected execution profile. |
| `submit/`, `status/`, `cancel/`, `follow/` | Submit a Centurion run and inspect, cancel or follow it. |
| `host/` | Host status, drain and stop commands. |
| `schema/` | Flat command catalog for CLI usage linting. |
| `shared/` | Shared client and environment helpers used by these handlers; no root command. |

The local runner's result enum is described in
[`src/engine/README.md`](../../engine/README.md#state-inventory).
