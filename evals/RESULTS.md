# Orchestrator eval results ledger

Append-only record of graded `live`/`lifecycle` orchestrator eval runs. One row per `--grade-artifacts <dir>` regrade (see `regrade_artifacts` in `harness.py`). A blocked run (host exit 75) is recorded with grade `blocked` and never counts as a pass. Never hand-edit an existing row; append a new one instead -- the row for a given artifact hash is the regrade's own verdict at that point in time, and a later regrade of the same retained artifacts adds another row rather than replacing it.

| date | case | mode | vendor | surface | model | grade | artifact hash |
|---|---|---|---|---|---|---|---|
