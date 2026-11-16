Use Planar to dispatch its bundled worker to perform one small task in a fresh
native child. The child should create `/workspace/RESULT.txt` containing exactly
the single digit 2 followed by a newline. Complete this bounded dispatch and
report the child outcome.

The invoking Codex exec transport requests driver model `gpt-6.1-terra` and
reasoning effort `medium`. These are the configured request, not an observation
of the backend model or effort. Use the installed Planar profile and host
contract for the worker dispatch.
