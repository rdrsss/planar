Use the installed Planar package to dispatch exactly one fresh native child.
The child should create `/workspace/RESULT.txt` with exactly `2` followed by a
newline. Request the worker model `gpt-6.1-terra`, reasoning effort `medium`,
and `fork_turns: none`. Wait for the child and report its outcome.

This is `planar/native-lineage-smoke`, contract version 1. The bounded check
requires host skill inventory, one linked direct child with the requested pins
and completion, and an independently exported regular file with exact bytes.
It does not prove resource reads, their order relative to spawn, causal child
write activity, complete role delivery, automatic activation, backend identity,
or full TEST-S69. Native events are agent-writable.
