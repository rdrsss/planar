# W7 — import / synthesize / ingest pipeline (STUB)

**Status:** STUB / decisions-parked · child of `00-anchor.md` · PARTIAL
**Current home:** `engine/import.zig`, `engine/synthesize.zig`,
`handlers/spec/ingest.zig` (~2000 LOC) + the `importer`/`synthesizer`/`ingestor`
agents
**Determinism:** ~40% — deterministic discover/parse/diff/apply floor wrapped
around a load-bearing optional LLM pass.

**What it owns:** the planning-content pipeline — discover source docs → parse →
classify/decompose → (optional LLM interpret/synthesize) → diff vs current state
→ preview → apply. Three related entrypoints (import = translate existing,
synthesize = generate from repo, ingest = decompose workbench docs) share the
discover→diff→apply skeleton.

**Why a stub.** This is the *least* clean extraction in the initiative — the
deterministic floor (classifier, fingerprint cache, title-based reconciliation,
atomic per-plan apply with savepoint rollback) is strong, but the LLM
interpret/synthesize pass is load-bearing for two of the three entrypoints, and
the apply step is a schema-aware atomic write that must stay in Zig.

- **D-W7.1 — where the apply boundary sits.** Entity insertion (slug derivation,
  parent-before-child ordering, FK enforcement) is KEEP-ZIG. The question is
  whether the harness can own discover→classify→diff and hand a *diff document*
  to a single atomic `apply` verb, or whether the diff computation is too coupled
  to the schema to leave the binary. *Gate:* inspect whether `spec ingest`
  already separates "compute diff" from "apply diff" internally — if the diff is
  a serializable intermediate, the seam is clean; if not, it is entangled.
- **D-W7.2 — the LLM pass.** `--interpret` / synthesize is a callout the harness
  invokes (write Request → run LLM → merge Result by deterministic rules). This
  matches the existing isolated-callout pattern, so it is extractable *if*
  D-W7.1 resolves. *Gate:* with D-W7.1.

**Resolution gate (stub → spec):** lowest priority — defer until `03`/`04`/`05`
have shipped and the "deterministic floor + isolated LLM callout" pattern is
proven on simpler ground. The closure-measurement work (`closure-measurement-
build-spec.md`) also dogfoods `pl-spec-ingest`; coordinate so this extraction
does not disturb that experiment's ingest path mid-flight.

**Pre-declared touches (for ingest):** `src/engine/import.zig`,
`src/engine/synthesize.zig`, `src/cmd/planar/handlers/spec/ingest.zig`
(read-only audit at stub stage).
