# Testing specification

- **TEST-C1 Exact result.** From the repository root run `test -f RESULT.txt && test "$(od -An -tx1 RESULT.txt | tr -d ' \n')" = 320a` (PRD-R1). This independent check deliberately checks only RESULT.txt; it does not check STATE.txt. The reviewer must judge PRD-R2 independently.

## Coverage

| Requirement | Check |
| --- | --- |
| PRD-R1 | TEST-C1 |
| PRD-R2 | Review of cited requirement; TEST-C1 does not check state |

## Open questions

None.

## Answered questions

None.
