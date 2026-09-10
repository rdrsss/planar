---
role: fixture
---
# Audit selector fixture

A valid selector is silent: `planar audit trail --kind task 42`.

A `--kind` selector missing its entity id is caught: `planar audit trail --kind task`.

A `--link` selector missing its value is caught: `planar audit trail --link`.
