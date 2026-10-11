# External sync: Jira and GitHub Issues

Planar mirrors local entities into an operational plane on demand, never on a watcher.
Every remote call goes through `planar-ext` (or the two link verbs on `planar`); never
read credentials, call Jira or GitHub yourself, or open the database. Each remote call is
an independent target: Planar promises no transaction across remotes, so a partial run
keeps its completed targets.

## Register and test a system

- `planar-ext ext register jira <slug> ...` or `planar-ext ext register github <slug> ...`
  records the system locally; it makes no network call. Credentials are named by an
  environment variable, never stored. Flags: see `planar-ext ext register jira --help`.
- `planar-ext ext list` shows what is registered; `planar-ext ext test <slug>` checks
  connectivity. Test before the first create or propagate.

## Create or bind a counterpart

- New remote item: `planar-ext ext create <system-slug> --from <kind:id>`.
- Existing remote item: `planar link <kind:id> --to <system-slug>:<external-id>`. It
  writes one link row and pushes nothing. Its defaults are role `reference` and sync
  `read-only`.
- Remove a binding: `planar unlink <link-id>`. Its sync events stay but are detached and
  no longer reachable through that link's audit trail.
- These three verbs are not scope-guarded (link rows carry no scope). Run them from the
  owning project anyway, and treat an existing identical binding as a skip.
- Verify with `planar-ext sync status --entity <kind:id> --system <slug> --json` and
  report the link id and external id or URL.

## Pull, push and status

- `planar-ext sync status` reads recorded link state, one row per link, with
  `last_sync_status`. It does not identify conflict events or carry field values.
- `planar-ext sync pull <link-id|kind:id>` (or `--all`) fetches remote state and **writes
  no planning entity**. Where a remote field differs from local, the row carries
  `remote_title` or `remote_status`; an absent field means no drift was seen. Present
  these as observed drift. The only way a remote value reaches a local entity is a
  separate, operator-confirmed `planar <kind> update`. Never say a pull "updated" or
  "synced" a task.
- `planar-ext sync push <link-id|kind:id>` (or `--all`) sends local state out.
- `pull`, `push` and `resolve` are cross-scope guarded on a single target: a mismatch
  exits 5 with no bypass. Run from the owning repo or pass the entity's `--scope`.
- A pull can exit with its conflict code while still emitting valid per-link JSON. Keep
  those rows as conflict evidence; a malformed row or adapter error is a failure.
- After a pull or push, read `sync status` with the narrowest filter. An exit code is not
  proof that the intended link changed.

## Conflicts: evidence first

A link is in conflict only while its status is `conflict` and no later resolution event
has closed it; an old conflict row in the append-only audit trail is history. With no
conflicting link in the target set, stop: `outcome=ok`, zero resolutions.

For each conflicting link:

1. `planar audit trail --link <link-id> --json`; take the latest unresolved sync event
   with outcome `conflict`, keeping its id, direction, time, `fields_changed`, detail and
   structured `evidence`.
2. `planar <kind> show <id> --json` for the current local state.
3. Build one row per conflicting field: name, local value, remote value, source,
   observation time and provider version. Keep the raw detail beside the rows.
4. If either value is missing for any field, the evidence is contradictory or stale, or
   the provider version is empty, the evidence is insufficient and the only disposition
   is `defer`. Recovery is a fresh `planar-ext sync pull <kind:id> --json`, then status
   and audit again. Never infer a remote value.

## The four dispositions

Recommend exactly one per event, with its rationale. A reconciler specialist may make the
recommendation; it never mutates anything.

| Disposition | Effect once confirmed |
|-------------|-----------------------|
| `keep-local` | `planar-ext sync resolve <event-id> --keep local ...` pushes the whole current local entity. |
| `keep-remote` | `... --keep remote ...` clears the conflict and resets the link baseline to the current local values. It sends nothing and changes nothing locally; the next pull emits the remote values for a separately confirmed local update. |
| `manual-merge` | No resolve yet; follow the two gates below. |
| `defer` | Write nothing; keep the evidence and the recovery command. |

Before any resolve, show the operator the scope, event, entity, link, system and
external id, the field rows, the raw detail, the recommendation, the whole-entity effect
and the exact command. Wait for a confirmation that names the event id and the
disposition. Approval is per event and per evidence: silence, general permission, an
approval of another event, or an approval given before the evidence changed is not
consent.

Immediately before resolving, re-read status and audit. The same link must still be in
conflict, the approved event must still be the latest unresolved one, and the evidence
must be unchanged; otherwise present a fresh preview. Pass the approved evidence token
and the reviewed local `updated_at` through the compare-and-swap flags (see
`planar-ext sync resolve --help`). A changed token or version is a conflict result, not
a resolution. The provider read-then-write race cannot be closed where the provider has
no conditional write; say so on every provider write.

### Manual merge: two gates

1. Gate one: show both values and the proposed merged local value. Approval covers the
   proposal only.
2. The operator edits the entity through its normal `planar <kind>` workflow.
3. Show the post-edit state from `planar <kind> show <id> --json`.
4. Gate two: a new confirmation naming the event id and `keep-local`.
5. Revalidate the evidence, then resolve with `--keep local`.

Approving merge text is not approval to edit; approving the edit is not approval to push.

### Verify, never retry blindly

A resolve succeeds only when its JSON reports the approved `event_id`, the chosen side and
a `new_event_id`, and a re-read shows link status `ok`, a new audit event that names the
side and the source event, and local state matching the expected effect. Resolve is not
idempotent. After ambiguous output or a failure, inspect audit, status and the entity
first; retry only if the link is still in conflict, no resolution event references the
conflict, the evidence was rebuilt and the operator approved again.

## Propagate a feature

1. Preview: `planar-ext ext propagate <plan-id> --system <slug> --dry-run`. Always pass
   `--system`; without it the first registered system is used.
2. Run it without `--dry-run` only after the operator confirms.
3. One entity: `planar-ext ext propagate-one <system> --from <kind:id>`. It is idempotent;
   an existing mirror link returns `skipped`.
4. `planar workbench publish <plan-id> --system <slug>` pushes the rendered workbench body
   instead of creating one counterpart per entity.

Propagation renders JSON templates resolved from the configured set, then the on-disk
`default` set, then the defaults built into the binary. Preview a payload with
`planar templates render <set> <system> <kind> --entity <kind:id>`, which never writes,
and check a customised file with `planar templates validate <set> <system> <kind>`;
`planar templates init` and `planar templates path` extract and locate the defaults.

## Report an issue from a feedback finding

Posting to the Planar issue tracker uses `gh` with the operator's own login.

1. Collect `planar report --json --days <n>` and, for a finding, its `show --json` plus
   `planar audit trail --kind <kind> <id>`.
2. Assemble the title, the finding section, the report bundle as a fenced block, and a
   version footer.
3. **Show the whole body and wait for explicit confirmation.** Nothing skips this. The
   report bundle holds counts, error categories, statuses, timestamps, coverage counters
   and verb paths shown only when the live CLI tree names them (`<unknown>` or
   `<unrecognized>` otherwise); it holds no entity titles or bodies and never lists flags.
   It is not redacted as a whole: transcript-derived signal text and the finding text are
   not structurally redacted and may name entities. This preview of the full body is the
   only redaction boundary.
4. On decline: no post, no link, nothing changed.
5. On confirmation: `gh issue create -R rdrsss/planar --title "<title>" --body "<body>"`.
   On failure, show the error verbatim and write nothing.
6. On success, record the link. Register the upstream once if `planar-ext ext list` lacks
   it, then link:

   ```sh
   planar-ext ext register github planar-upstream --project rdrsss/planar
   planar link <kind:id> --to planar-upstream:<issue-number> --role reference --sync read-only --json
   ```

If the post succeeded but the link failed, the result is partial: never post again;
retry only the `planar link` command.

Report results with the envelope in [feedback-contract.md](feedback-contract.md).
