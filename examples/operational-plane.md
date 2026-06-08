# Example: Propagate A Plan To Jira Or GitHub Issues

Use this after local planning and execution have produced a coherent feature
tree that should be visible in the operational system of record.

## 1. Register Or Confirm The External System

```text
planar ext list
planar ext test <system-slug>
```

If the system is not registered, use the appropriate registration command from
`docs/cli-reference.md`.

## 2. Preview Propagation

```text
/pl-ext-propagate <plan-id> --system <system-slug> --dry-run
```

Check that the preview creates the expected counterpart hierarchy and does not
try to publish draft-only or out-of-scope work.

## 3. Propagate

```text
/pl-ext-propagate <plan-id> --system <system-slug>
```

This creates or reuses external counterparts and records links locally.

## 4. Sync Later Changes

```text
/pl-sync status
/pl-sync push plan:<plan-id> --system <system-slug>
/pl-sync pull plan:<plan-id> --system <system-slug>
```

Use `push` for local changes that should update the external system. Use `pull`
when the external system is authoritative for a field and you want to bring it
back into Planar.

