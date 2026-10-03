# Research Instrumentation

Planar's research program, experiments, datasets, analyses, claims, and papers are
maintained outside this repository.

This directory holds only documentation owned by the Planar instrument:

- [`run-record-schema.md`](run-record-schema.md) — rationale and schema boundary
  for durable run, event, and touch records;
- [`closure-measurement-build-spec.md`](closure-measurement-build-spec.md) —
  historical engineering specification for the measurement and closure subsystem;
- [`hypergraph-tech-spec.md`](hypergraph-tech-spec.md) — historical engineering
  specification for grouping and partitioning.

Instrument implementation, migrations, tests, metric fixtures, and the historical
matrix driver remain in this repository because they describe or verify Planar's
behavior. Research protocols must pin a Planar commit and consume documented CLI
or export surfaces; they must not treat this directory as the evidence authority.

Do not add new protocols, results, raw datasets, analysis narratives, literature
reviews, or manuscripts here. Keep project-specific material in the research
program's own repository and link back to immutable Planar commits when instrument
provenance is needed.

