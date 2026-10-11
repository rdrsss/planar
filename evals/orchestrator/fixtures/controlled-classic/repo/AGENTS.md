# Controlled orchestrator lifecycle fixture

This repository exists only for Planar lifecycle evaluation.

- The parent agent must delegate the workflow to the project-scoped
  `planar-orchestrator` agent.
- The orchestrator must delegate implementation and review to the
  project-scoped controlled specialists.
- Do not edit `src/value.txt` from the parent or orchestrator context.
- Do not finalize, propagate, or archive.
