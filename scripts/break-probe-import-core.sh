#!/usr/bin/env bash
#
# Mutant evidence for `src/cmd/planar/import_leaf.t.cpp`.  This is deliberately
# separate from the generic prover: each invocation below names one invariant
# and one test, so an apparently-green import suite cannot hide an unprobed
# transaction or idempotency predicate.

set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"

probe() {
  scripts/break-probe.sh --file src/cmd/planar/handlers/import.cpp "$@" \
    --build-dir build/debug --target planar_cmd_planar_tests
}

# A failed cache reconciliation must escape through the transaction guard;
# swallowing the error makes the handler commit phase/artifact writes and the
# rollback fixture must go red.
probe --label import-rollback \
  --test 'interpreted import rolls every prior write back' \
  --mutate "perl -0pi -e 's/if \(!reconciled\) return std::unexpected\(reconciled\.error\(\)\);/if (!reconciled) { static_cast<void>(reconciled.error()); }/' src/cmd/planar/handlers/import.cpp"

# The second apply relies on the anchor-slug conflict path.  Turning every
# conflict into a failure must kill the idempotency fixture.
probe --label import-idempotency \
  --test 'interpreted import applies its cache atomically and is idempotent' \
  --mutate "perl -0pi -e 's/if \(created\.error\(\) != pl::plan_error::slug_conflict\)/if (true)/' src/cmd/planar/handlers/import.cpp"

# Imported markdown must be materialized, rather than merely counted in the
# cache.  Replacing the reconciliation call with a successful no-op must kill
# the artifact/link assertions in the same end-to-end fixture.
probe --label import-artifact-materialization \
  --test 'interpreted import applies its cache atomically and is idempotent' \
  --mutate "perl -0pi -e 's/auto artifacts = reconcile_artifacts\(conn, root, anchor, scope, apply_removals\);/std::expected<void, domain_error> artifacts{};/' src/cmd/planar/handlers/import.cpp"

# Proposed removals are opt-in.  Forcing the first removal pass on ordinary
# apply must kill the fixture's pre-removal state assertions.
probe --label import-removals-opt-in \
  --test 'interpreted import applies proposed removals only when explicitly enabled' \
  --mutate "perl -0pi -e 's/if \(apply_removals\) \{/if (true) {/' src/cmd/planar/handlers/import.cpp"

# A retained task is a real exclusion, not merely a count: cancelling it too
# must kill the survivor assertion after the removal-enabled apply.
probe --label import-removal-survivor \
  --test 'interpreted import applies proposed removals only when explicitly enabled' \
  --mutate "perl -0pi -e 's/if \(kept_tasks\.contains\(id\)\) continue;/if (false) continue;/' src/cmd/planar/handlers/import.cpp"
