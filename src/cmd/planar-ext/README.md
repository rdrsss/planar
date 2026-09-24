# `planar-ext`

Operational-plane CLI for external systems, propagation and sync. `main.cppm`
assembles its `CLI::App` tree. Its SQLite authorizer allows writes only to
`external_links`, `external_systems` and `sync_events`; planning tables are
read-only. Each `handlers/<name>/` directory owns one root command family.

| Handler directory | CLI app and role |
| --- | --- |
| `ext/` | Register, list and test external systems; create counterparts and propagate a feature. |
| `sync/` | Pull or push remote state, inspect status and resolve conflicts. Pull emits proposed planning changes for verification. |
| `version/`, `schema/` | Version metadata and flat command catalog. |
| `shared/` | Shared handler helpers; no root CLI app. |

The link and event states are listed in
[`src/engine/README.md`](../../engine/README.md#state-inventory); the sync
flow is in [`docs/lifecycles.md`](../../../docs/lifecycles.md#54-external-sync-planar-ext).
