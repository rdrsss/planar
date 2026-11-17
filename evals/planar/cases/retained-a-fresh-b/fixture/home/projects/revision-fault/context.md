# Context: revision-fault

## Statement

Create the two files specified by PRD-R1 and PRD-R2. This isolated synthetic fixture deliberately checks only RESULT.txt independently; the reviewer must judge STATE.txt against PRD-R2. A separately disclosed controlled fault injection may occur after the first worker completes.

## Sources

- Operator authorization for this bounded fixture.

## Decisions

1. 2026-11-16: Approve this isolated one-task spec set for a revision exercise with no task commit or worktree.

## Change log

- 2026-11-16: Created the fixture spec set.
