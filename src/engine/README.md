# Engine

`src/engine/` contains Planar's domain operations and state rules. Command
handlers in [`src/cmd/`](../cmd/README.md) parse requests and call these
modules; the engine does not assemble CLI applications. Storage and migrations
live in `src/lib/db/` and `migrations/`.
Multi-command lifecycle scenarios and their coverage inventory live in
[`src/cmd/integration_tests/`](../cmd/integration_tests/README.md).

## State inventory

This table lists the stored lifecycle states owned or interpreted by engine
modules. Spellings are database values. An arrow summarizes the normal path;
it does not replace the transition guards and exceptional engine roll-ups in
[`docs/lifecycles.md`](../../docs/lifecycles.md). Migration `CHECK` constraints
and the transition code are authoritative.

| State holder | Values | Normal path and owner |
| --- | --- | --- |
| Plan | `draft`, `active`, `paused`, `done`, `abandoned` | `draft → active → done`; `active ↔ paused`; `active → abandoned`. Task roll-up can reopen `done → active`. [`planning/`](planning/) |
| Plan step | `pending`, `in-progress`, `done`, `skipped` | `pending → done` or `skipped`; the engine can move `pending → in-progress → done`. [`planning/`](planning/) |
| Task | `todo`, `doing`, `blocked`, `done`, `cancelled` | `todo → doing → done`; blocker edges can park and unblock; terminal tasks can be reopened. Claim terminal verbs change task and claim atomically. [`planning/`](planning/), [`runtime/`](runtime/) |
| Question | `open`, `answered`, `wontfix` | `open → answered` or `wontfix`. [`planning/`](planning/) |
| Decision | `proposed`, `accepted`, `superseded`, `withdrawn` | `proposed → accepted`; `proposed` or `accepted → superseded` or `withdrawn`. [`planning/`](planning/) |
| Test scenario | `draft`, `ready`, `verified`, `failing`, `retired` | Verification walks `draft → ready → verified`; `ready → failing`, `verified → failing`, and `failing → verified` are matrix-legal; non-retired states can retire. `last_outcome`: `pass`, `fail`, `error`, `skipped`. [`planning/`](planning/) |
| Artifact | `draft`, `active`, `superseded`, `retired` | `draft ↔ active`; `active → superseded` or `retired`. [`planning/`](planning/) |
| Annotation | `active`, `resolved`, `dismissed`, `archived` | `active → resolved` or `dismissed` or `archived`; outcomes can be archived. [`planning/`](planning/) |
| Handoff | `pending`, `validated`, `consumed`, `abandoned` | `pending → validated → consumed`; pending or validated can be abandoned. [`runtime/`](runtime/) |
| Agent work claim | `active`, `released`, `completed`, `aborted`, `stale` | A live active claim ends through one atomic terminal verb; expiry is marked `stale` by reconciliation. [`runtime/`](runtime/) |
| Agent action | Open or closed (`ended_at` is null or set) | A claim or nested action opens a row; action end or a claim terminal verb closes it. Outcomes: `ok`, `error`, `aborted`, `timeout`. [`runtime/`](runtime/) |
| Feedback triage disposition | `untriaged`, `needs-reproduction`, `accepted`, `retained-question`, `dismissed`, `reported-external`, `duplicate` | Triage disposition; reproduction status is `not-run`, `reproduced`, `not-reproduced`, or `inconclusive`. [`planning/`](planning/) |
| External link `last_sync_status` | `never`, `ok`, `conflict`, `error` | Sync sets success, conflict or error; resolution returns a conflict to `ok`. [`external/`](external/), [`extsync/`](extsync/) |
| Sync event `outcome` | `ok`, `conflict`, `error`, `noop`, `strategy-abandoned`, `counterpart-missing`, `resolved-fs`, `resolved-db`, `partial`, `success`, `failure` | External and workbench operations record outcomes; these are event values, not a single entity lifecycle. [`extsync/`](extsync/), [`workbench/`](workbench/) |
| Session | Active or ended (`ended_at` is null or set) | Capture starts a session; `capture end` sets the timestamp. [`runtime/`](runtime/) |

Other state-like values are *computed observations*, not stored lifecycles:
annotation verification reports `fresh`, `drifted`, or `stale`;
workbench classification reports `no_op`, `db_to_fs`, `fs_to_db`,
`conflict`, `new_on_fs`, or `malformed`; health, closeout and resume
readiness are derived from current rows. `next_work` groups tasks as
`available`, `claimed`, `stale`, or `blocked`. See
[`docs/lifecycles.md`](../../docs/lifecycles.md) for transitions, guards,
side effects and the executable source index.

## Module directories

| Directory | Responsibility |
| --- | --- |
| [`config/`](config/) | Configuration operations and defaults. |
| [`entitylink/`](entitylink/) | Relationships between planning entities. |
| [`external/`](external/) | External systems, links and adapters. |
| [`extsync/`](extsync/) | External pull, push, conflict and propagation logic. |
| [`health/`](health/) | Derived health and diagnosis. |
| [`identity/`](identity/) | Repository and association identity. |
| [`importer/`](importer/) | Translate existing repository planning material. |
| [`ingest/`](ingest/) | Decompose workbench specifications into entities. |
| [`introspect/`](introspect/) | Usage introspection and findings. |
| [`introspection_adapters/`](introspection_adapters/) | Inputs to introspection. |
| [`local/`](local/) | Operator local skill and agent lifecycle. |
| [`planning/`](planning/) | Plans, tasks, decisions, scenarios and other planning entities. |
| [`promotion/`](promotion/) | Scope promotion and demotion. |
| [`runtime/`](runtime/) | Claims, actions, sessions and handoffs. |
| [`search/`](search/) | Planning search. |
| [`synthesize/`](synthesize/) | Generate planning artifacts from repo evidence. |
| [`templates/`](templates/) | External template selection and rendering. |
| [`tree/`](tree/) | Entity hierarchy views. |
| [`workbench/`](workbench/) | Filesystem sync, conflicts, archive and restore. |
| [`workspace/`](workspace/) | Workspace registration and scans. |
