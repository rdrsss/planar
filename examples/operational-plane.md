# Example: Propagate A Plan To Jira Or GitHub Issues

Use this after local planning and execution have produced a coherent feature
tree that should be visible in the operational system of record.

## 1. Register Or Confirm The External System

```text
planar-ext ext list
planar-ext ext test <system-slug>
```

If the system is not registered, use the appropriate registration command from
`docs/cli-reference.md`.

## 2. Preview Propagation

```text
planar-ext ext propagate <plan-id> --system <system-slug> --dry-run
```

Check that the preview creates the expected counterpart hierarchy and does not
try to publish draft-only or out-of-scope work.

## 3. Propagate

```text
planar-ext ext propagate <plan-id> --system <system-slug>
```

This creates or reuses external counterparts and records links locally.

## 4. Sync Later Changes

```text
planar-ext sync status --system <system-slug>
planar-ext sync push plan:<plan-id> --system <system-slug>
planar-ext sync pull plan:<plan-id> --system <system-slug>
```

Use `push` for local changes that should update the external system. Use `pull`
when the external system is authoritative for a field and you want to bring it
back into Planar.

