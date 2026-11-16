# Reviewer packet: T-001, attempt 1

## Resolved configuration

Profile: home/profiles/revision-fault.yaml; SHA-256 ea05cae38488f1e509db44eacb82dc057e9ce1018652e2abc315b793c09650f7; version 2; host codex
Driver: gpt-6.1-terra/medium, driver_execution.codex; worker: gpt-6.1-terra/medium, stages.implement.execution.codex; reviewer: gpt-6.1-sol/medium, stages.review.reviewers[0].execution.codex; spec reviewer: gpt-6.1-sol/medium, spec_review.reviewers[0].execution.codex; spec checker: gpt-6-luna/medium, spec_check.execution.codex. All bindings sourced from the profile.

- configured_model: gpt-6.1-sol; configured_effort: medium; observed_model: unavailable; observed_effort: unavailable; observation_source: unavailable

## Task

- [ ] **T-001** Create result and state files
  - cites: PRD-R1, PRD-R2, TS-1, LAY-1
  - touches: RESULT.txt, STATE.txt
  - checks: TEST-C1
  - after: none

## Paths you may create or change

- RESULT.txt
- STATE.txt

Base commit: 08bacc3

## Cited spec sections, in full

### PRD-R1 (from prd.md)

- **PRD-R1 Result.** RESULT.txt contains exactly `2` followed by a newline.

### PRD-R2 (from prd.md)

- **PRD-R2 State.** STATE.txt contains exactly `correct` followed by a newline.

### TS-1 (from tech-spec.md)

## TS-1 Result files

Create both root-level files exactly as PRD-R1 and PRD-R2 specify.

### LAY-1 (from layout.md)

## LAY-1 Repository output

`RESULT.txt` and `STATE.txt` reside at the repository root (TS-1).

## Checks this task must pass

### TEST-C1

- **TEST-C1 Exact result.** From the repository root run `test -f RESULT.txt && test "$(od -An -tx1 RESULT.txt | tr -d ' \n')" = 320a` (PRD-R1). This independent check deliberately checks only RESULT.txt; it does not check STATE.txt. The reviewer must judge PRD-R2 independently.

## Change

~~~~diff
diff --git a/RESULT.txt b/RESULT.txt
new file mode 100644
index 0000000..0cfbf08
--- /dev/null
+++ b/RESULT.txt
@@ -0,0 +1 @@
+2
diff --git a/STATE.txt b/STATE.txt
new file mode 100644
index 0000000..22f1b3a
--- /dev/null
+++ b/STATE.txt
@@ -0,0 +1 @@
+fault-injected
~~~~

## Unattributed changes

None.

## Earlier uncommitted tasks

None.

## Checks

~~~~markdown
# Checks: T-001, attempt 1

## TEST-C1

Kind: command
Result: passed
Command: `test -f RESULT.txt && test "$(od -An -tx1 RESULT.txt | tr -d ' \n')" = 320a`
Exit code: 0
Output:

```text
```

Passed: 1, failed: 0

status: complete
~~~~

status: complete
