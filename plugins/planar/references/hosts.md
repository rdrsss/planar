# Host capability contract

This reference packages the bounded evidence and intended host contract for
PRD-R39, PRD-R41 and PRD-R42, TS-24.1 to TS-24.7,
and layout entries LAY-7.1 and LAY-7.2. The minimal Codex dispatch instructions are implemented but their end-to-end
scenarios remain unverified.
The shared Markdown specs, packets and records remain authoritative. No
engine, database, MCP service or shipped runner is introduced.

## Resource capabilities

Follow `${CLAUDE_PLUGIN_ROOT}/references/lifecycle.md` (TS-19.9) for
ownership, child retirement and teardown. Use only a verified native capability
and proven mapping for an individual handle; the evidence below does not
establish a native agent-close API. Treat an unavailable capability as
unsupported and record it as the lifecycle reference directs.

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
The shared serial `minimal` profile uses `gpt-6.1-terra` for the
selected driver and worker, `gpt-6.1-sol` for task and spec reviewers, and
`gpt-6.1-terra` for the spec checker, with `medium` effort on each dispatched
role. Pass full model and effort explicitly for every fresh-context dispatch.
These pins are frozen when authored or deliberately updated. Establish the
selected driver model and effort from host configuration or explicit operator
confirmation of both before execution; reading a worker pin does not switch or
identify the session. If either selected value differs, or the driver is Astra, switch or
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

On Codex, start with the discovered installed `skills/<name>/SKILL.md`;
from its directory read `../../references/hosts.md`. The package root is two
parents above that skill directory. In all bundled skills, role instructions,
references and profiles, interpret `${CLAUDE_PLUGIN_ROOT}/<path>` as
`<package root>/<path>`, including links reached transitively. This is path
resolution, not a claim that Codex sets the Claude environment variable.
Use that same root for bundled skills, agents, references and profiles. On
Claude Code, retain the host's `${CLAUDE_PLUGIN_ROOT}` substitution rules.
If discovery, the installed file or a linked resource cannot be resolved,
stop before the affected action. The probe verified the relative reference
read for a scratch skill, not these Planar files.

Keep project artifacts in the selected project and use explicit scratch homes
and writable roots for tests. A fresh session's access to files was observed;
the complete Planar resume state machine and cross-host handoff remain
unverified. Do not claim either direction proven without the required live
host scenarios.

## Interrupted run and host handoff

Usage exhaustion, cancellation, process loss, connectivity failure and a
deliberate stop are interruptions, not successful steps or failed reviews.
Before replacement dispatch, require confirmation that the previous driver
and all its children have stopped. An explicit operator statement is enough;
do not ask again. A parent reaching its limit alone does not establish that
its children stopped. This is a single-run protocol, not a lock or lease.

The receiving session needs the project location and a continuation request.
Read its existing specs, records, packets, artifacts and Git state; do not
require the previous transcript, provider cache, final handoff message or a
source-vendor model call. Verify approval digests, working tree identity,
profile compatibility and attribution before work. Stop and identify changed
digests, an unrelated tree, ambiguous changes or precisely which required
packet or evidence is missing; never invent evidence from conversation.
Explain the recovered task and attempt, completed steps, first unfinished
step and any blocking capability to the operator before dispatch.

Resume by the task record and loop references. A complete worker report goes
to checks, while a partial worker step is rerun with its existing packet and
a partial-change note. Reconcile a completed file whose log is stale. Rerun
an incomplete check or review, preserving completed independent results that
still apply to the same candidate. Finish interrupted approval and commit
bookkeeping without duplicate approvals or commits. Preserve numbered evidence,
verdicts, attempts and policy limits. A host change is not a failed review and
does not reset revision or stall counters.

A compatible profile switch affects future dispatches only: it neither
rejudges completed evidence nor removes required stages. Block an incompatible
profile at the affected dispatch and explain the missing stage or capability.
A required source-host check remains pending while that host is unavailable;
a target-host result cannot substitute for it or unblock approval. Workflow
mode and its automatic fallback remain Claude-only. Report unsupported Claude
workflow or richer-profile requirements instead of reducing them to Codex
minimal behavior.

## Host selection and compatibility

Each skill selects its host before acting. On Claude Code,
`${CLAUDE_PLUGIN_ROOT}` resolves to the plugin and bundled roles dispatch as
`planar:<name>`. On Codex, use the package-root rule above; Claude front matter does not
register Codex agents. For a bundled role named by a profile, select
`agents/<name>.md` under that root, read the complete file and deliver its
instructions to a fresh, separate context together with only the bounded
dispatch inputs required for that role. Pass the profile's explicit model and
effort. The receiving context must follow the role's restrictions, including
its output ownership and review rules, and resolve its bundled links by the
same package-root rule. The role instructions are dispatch instructions, not
extra packet content. Refuse the profile or stop before dispatch if the role
file, fresh context, instruction delivery, restrictions or required pins
cannot be honoured. The native fresh-dispatch probe did not exercise the
actual Planar role files; this contract does not claim it did.

A profile is compatible only when the host can resolve its roles and honour
its model and effort requirements. Refuse an incompatible profile before the
run; do not substitute a role or model. The existing references remain the
sole definitions of loop state, packets and approval. The root portable
manifest packages the skills; it does not itself establish role dispatch or
end-to-end Planar integration.

## Session audit capture (TS-7.5, PRD-R35)

Follow `${CLAUDE_PLUGIN_ROOT}/references/task-record.md` for invocation and
observation records (TS-7.5, TEST-I12). Prefer actual dispatch return metadata,
host events or supported host metadata access, with a source for each identity
and locator. The observed Codex collaboration dispatch return exposed canonical
task names for routing and status, not native IDs or locators. Keep that
finding scoped to the return; do not infer identity from a name.

On the verified Codex host, use a supported tool in the subject's own context
to inspect only the named nonsecret `CODEX_THREAD_ID` and `CODEX_SESSION_ID`
environment values when available. Record exact values, source and checked time;
do not dump the environment or read authentication data. The former identifies
the reporting thread on this surface; the latter may be shared by the root
session and its children. Do not use the shared session value as a child's
thread ID or infer parent links from equality. Preserve native parent-thread
relationships only when metadata or events confirm the subject and its exact ID.
Keep native agent type, including `default`, distinct from the assigned work
role, host agent ID and collaboration path.

A child may supplement the parent's dispatch observation with its own
tool-sourced values; a bare model self-report is insufficient, and the parent
must not depend exclusively on a child that may crash. If values or supported
metadata are absent or denied, record `unavailable` and the reason; do not
borrow the parent's identity or repeatedly probe a known-unavailable surface.
Availability on earlier container runs, other Codex builds or Claude is not
established. Earlier unavailable observations remain valid for their time and
surface.

Locator discovery is optional: search only for a known exact ID within a known
configured session root, inspect bounded `session_meta` first, and accept a
path only if its metadata ID matches. Confirm exposed parent and agent-path
metadata before assigning lineage. A path pattern is not proof; do not scan
unrelated session content or copy transcripts by default. Record the
configured-root-relative or home-relative locator, source, host/local/container
scope, checked time and availability. A verified file is not a retention
guarantee. Claude hook documentation describes identifiers and transcript
paths, but does not establish visibility here or authorize installing hooks;
live proof remains separate. Do not advertise either surface as audit-linked
without the proof required by the task-record reference.
