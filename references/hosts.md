# Host capability contract

This reference packages the bounded evidence and intended host contract for
PRD-R39 and PRD-R42, TS-24.3, TS-24.4 and TS-24.7. Codex integration is not
yet implemented; these requirements guide the remaining integration work.
The shared Markdown specs, packets and records remain authoritative. No
engine, database, MCP service or shipped runner is introduced.

## Evidence and surface

The project-local `reviews/codex-capabilities.md` records the procedures,
outputs and limitations of the 2026-11-15 local app-server probe with
codex-cli 0.169.0. A scratch plugin installed from a local marketplace using
a root manifest; an explicitly supplied installed skill resolved its bundled
reference from the cache. Targeted process-local overrides disabled the other
enumerated skills, and the test used scratch write roots. A fresh worker and
separate reviewer used `fork_turns: none`, with an independent byte check
between them; a second fresh session read the resulting files. This verifies
those primitives, not a complete Planar loop, cross-host handoff, global
isolation or an adversarial sandbox. Desktop and cloud surfaces were not
tested. The app-server transport exposed model and effort, but the agents did
not receive that metadata. An attempted agent-side native lookup failed at
subprocess initialization; it does not prove the metadata API unsupported.

Two bare `codex exec --ignore-user-config --ephemeral --json` attempts did
not discover the skill despite exit 0; the cause was not established. Do not
use them as a successful isolation recipe. The successful app-server path
used normal runtime configuration with targeted per-skill overrides and
explicit skill input. `skip_host_skill_discovery` also hid the probe skill.
Ephemeral `thread/read` with turns was rejected; retain live evidence and
durable artifacts rather than relying on recovery of that conversation.
The probe cache was removed, but the user config digest did not return to its
baseline; semantic equivalence was not established.

The separate project-local `reviews/codex-metadata-amendment-evidence.md`
records native collaboration calls with explicit model and effort and fresh
contexts, independent checks and review. Those calls completed, but responses
provided no backend model/effort observation; configured values were recorded
separately from `unavailable`. Historical Astra probes demonstrate primitives
only, not permission to use Astra in execution. The amendment permits missing
observations without requiring a nested lookup. The locally advertised catalog
and bounded tier dispatches are recorded in
`reviews/codex-model-allocation-evidence.md`; they are not universal
availability or measured model equivalence. The evidence covers the revised
prerequisite on the stated surfaces; full integration scenarios and mismatch
and negative cases remain pending.

## Dispatch and provenance requirements

Before a Codex execution step, validate the profile and available host
capabilities. Stop before the affected dispatch if a required pin, effort or
capability is missing or unsupported; never silently substitute or upgrade.
The planned serial `codex-minimal` profile uses `gpt-6.1-terra` for the
selected driver and worker, `gpt-6.1-sol` for task and spec reviewers, and
`gpt-6-luna` for the spec checker, with `medium` effort on each dispatched
role. Pass full model and effort explicitly for every fresh-context dispatch.
These pins are frozen when authored or deliberately updated. Establish the
selected driver model from host configuration or explicit operator
confirmation before execution; reading a worker pin does not switch or
identify the session. If the selected driver differs, or is Astra, switch or
resume in the required model before execution. Astra is drafting-only and is
never an execution driver, role or fallback. Advanced profiles, ladders,
autonomy and workflow parity remain deferred.

Worker and reviewer receive bounded packets in fresh separate contexts, share
the worktree with separate output ownership, and do not receive each other's
conversation. Checks are independent; only the driver writes task records.
For each Codex packet and result record `host: codex`, `configured_model`,
`configured_effort`, `observed_model`, `observed_effort` and
`observation_source`. Configured dispatch values are the actual requested
pins; observations come only from host runtime evidence. Record each absent
observation as `unavailable`, never infer identity from model output or promote
a configured value into an observation. Packets retain only observations
available at creation; add later evidence to the result or record. Preserve the
six-field log and qualify configured and observed sources as TS-24.4 requires.
Driver actor values use observations or `unavailable`; an explicitly
established driver selection is configured provenance, not an observation.
Missing observations alone do not block work. An exposed mismatch is retained,
sets the task to `question` and escalates before further task dispatch or
approval; a mismatched spec review does not satisfy its requested configuration.

## Resources and handoff

Resolve bundled references relative to the installed skill, as verified on
the local app-server surface. Keep project artifacts in the selected project
and use explicit scratch homes and writable roots for tests. A fresh session's
access to files was observed; the Planar resume state machine and cross-host
handoff were not. The intended handoff requires confirmation that the prior
run stopped, a compatible profile and preserved artifacts and changes before
continuing at the next completed step boundary. Do not claim a host-specific
check passed without running it there.
