# Annotation consumer contract

`planar annotate capabilities --json` is the source-bound discovery step for
local consumers. A consumer records its `source_uuid` and must obtain a new
handshake if the selected source changes. A capability response means the
source supports the annotation read representation, entity anchors, integer
revisions, structured annotation commands, and receipt lookup. It does not
enable a writer or change the source.

Consumers read annotations with `planar annotate list --json` and one row with
`planar annotate show <id> --json`. List results are a single JSON array in
ascending annotation ID order; both forms have one trailing newline. Read
commands only inspect the source. They do not resolve, archive, process, or
otherwise mutate an annotation.

Each annotation has a stable numeric `id`, stored `scope_kind`/`scope_id`,
`body`, `tags`, lifecycle `status`, timestamps, and a positive integer
`revision`. `revision` is the value a structured command uses as
`expected_revision`; it is independent of any source snapshot revision.

`anchor.kind` is `file` or `entity`. File anchors have a non-null `path` and
may contain line and verification fields. Entity anchors have `path: null` and
`target` is an object with `kind` (`plan` or `task`) and its numeric `id`.
File anchors have `target: null`. The legacy `plan_id` and `task_id` fields
remain available for existing file associations; new consumers should use the
unambiguous `target` object for entity annotations.

The list filters are exact, composable predicates. Use `--anchor-kind`,
`--target-kind`, and `--target-id` to select entity/file forms and targets;
`--status`, `--scope`, `--tag`, `--vendor`, and `--anchor-path` refine the
collection. `--plan` and `--task` preserve the older association filter and
are not a replacement for target filtering. A no-match list is `[]`, not an
error.

This contract exposes durable annotations for a future consumer. It does not
define a consumer skill, queue, automatic processing workflow, or any
background lifecycle action.

## Writer compatibility and receipts

Explorer remains a reader unless its operator explicitly starts it with its
annotation-writing option and configures a compatible `planar` executable. The
writer begins with `planar annotate capabilities --json`; a missing capability,
source UUID change, or incompatible command set leaves Explorer in its normal
read-only mode. Capability discovery itself does not create, migrate, or change
the selected database.

The supported Explorer command set is `create`, `edit`, `replace-tags`,
`resolve`, `dismiss`, and `archive`. It sends the source UUID, an operation
UUID, target/scope, and the current expected revision when the operation needs
one. `remove` remains a deliberate CLI-only disposition and is never an
Explorer command. A repeated operation UUID with the same payload returns the
original durable receipt; changing the payload for that UUID is a conflict.

Every command receipt and receipt lookup exposes `operation_uuid`,
`source_uuid`, `annotation_id`, `revision`, `affected_count`, `outcome`, and
whether it was replayed. `affected_count` is always present: it is the exact
number of annotations changed by a bulk operation and remains identical when
that receipt is replayed; non-bulk operations report `0`. Aggregate bulk
receipts have no per-row revision (`revision: 0` in the JSON representation),
because each affected annotation carries its own revision and audit record.
Consumers recovering after a timeout or restart must look up the original
operation UUID against the original source before deciding whether to retry.
