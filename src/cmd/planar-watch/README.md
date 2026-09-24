# `planar-watch`

Read-only viewer for active and historical agent activity. `main.cppm`
assembles its `CLI::App` tree; the database policy opens SQLite read-only.
Each `handlers/<name>/` directory owns one root command family.

| Handler directory | CLI app and view |
| --- | --- |
| `feed/` | Default activity feed. |
| `ps/`, `claims/`, `actions/` | Active claim summary, claim rows and action rows. |
| `plans/` | Plans with in-flight activity summaries. |
| `log/`, `tree/` | Activity history and parent-action forest. |
| `run/` | Workflow run list and detail. |
| `sync_events/` | Sync event history. |
| `version/`, `completion/`, `schema/` | Version, shell completion and command catalog. |
| `shared/` | Shared view helpers; no root CLI app. |

Follow-capable views stream new rows without changing the database. The
writer-side commands are in [`planar-agent/`](../planar-agent/README.md).
