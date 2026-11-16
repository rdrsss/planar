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
