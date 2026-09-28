# Shared command internals

These modules supply invocation plumbing to `planar`, `planar-agent`,
`planar-watch` and `planar-ext`. They do not define a CLI application or own
domain rules. `planar-execute` does not link this library and has no SQLite
handle.

| Module | Responsibility |
| --- | --- |
| `context.cppm` | Holds argv, environment lookup, cwd, output streams and an injected database object. Construction does not open SQLite. |
| `database.cppm` | Lazy connection holder parameterized by each binary's open policy; `ensure_db()` opens on first use and `refresh_db()` reopens. |
| `environment.cppm` / `.cpp` | Process or test environment lookup, `PLANAR_DB`/`HOME` path resolution, and binary-selected cwd policy. |
| `config_path.cppm` / `.cpp` | Resolve the operator configuration path. |

The caller constructs the database policy and passes its holder into
`context<Database>`. This keeps the binary's access policy at the entry
point: `planar` can open for planning writes, `planar-watch` opens read-only,
and `planar-ext` applies its table write allowlist. SQLite implementation
and migration application remain in `src/lib/db/`.
