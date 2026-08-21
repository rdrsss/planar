# Introspection transcript fixtures

These JSONL files are synthetic, sanitized examples of the current local
session envelopes written by Claude Code, Codex, and GitHub Copilot CLI. They
are test inputs only; none of the values were copied from an operator session.

Each vendor fixture includes:

- a paired tool call and result;
- ordinary user and assistant conversation records;
- an unknown future record shape;
- a structurally invalid instance of a recognized envelope; and
- synthetic privacy sentinels for transcript prose, argument values, entity
  text, scope-like values, and raw paths.

The fixtures deliberately do not prescribe normalization results. Extractor
behavior, pairing, ignored-versus-malformed classification, and evidence
redaction are covered by the implementation tasks that consume these inputs.
