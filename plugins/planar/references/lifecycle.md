# Resource lifecycle

This is the canonical lifecycle rule for every run and driver. Implements
PRD-R19, PRD-R28, PRD-R35, PRD-R24, TS-19.9 and TS-7.5; layout entries
LAY-1.5.13 and LAY-3.24. Use the host contract for verified capabilities and
`task-record.md` for the six-field log and invocation audit.

## Ownership and dispatch

Before creating a child agent, process, container, temporary authentication,
config or cache copy, scratch fixture or generated output, record its owner,
purpose, intended lifetime and exact planned locator in the task record or a
run cleanup report. When the host assigns the handle on creation, record that
fact before creation and append the exact returned handle as soon as observed.
Record observations with actual clock output. Never put secrets in a report.
Do not act on an unknown owner or guess a handle. Limit active workers to
available slots, give parallel workers disjoint file ownership, and reuse a
suitable idle agent for related non-independent work where appropriate. A
review still requires a fresh independent context: never share the worker's
conversation with its reviewer.

After each child result, cancellation or failure, persist its required output,
verdict, provenance and audit links before retiring it. Completed, interrupted,
closed and archived are distinct observations; record only what the host
actually establishes. Close or archive an individual child only through a
verified native capability and a proven mapping to its exact handle. A
collaboration path is not itself such a handle. If individual closure is
unsupported, record the host limitation once and report the child's completed
status where observed; do not repeatedly attempt closure or archive the user's
parent conversation as a proxy. Never interrupt another task's children.

## Teardown and resume

At each task or round boundary and before returning control, reconcile owned
resources; do so on success, failure, timeout, cancellation and blocked work,
without waiting for final approval. On resume, recheck identity and ownership
of leftovers before acting: a stale handle alone grants no permission. If a
resource is already absent, record the observation. Stop owned processes and
children before removing their resources, remove temporary credential copies
promptly, and verify exact owned process and container handles are absent: a
stop request alone is not proof. If stop or verification fails, retain dependent
resources and record the failure and next recovery action. Never claim clean
teardown while known owned processes or temporary credentials remain.

Remove only verified owned temporary resources whose evidence and dependents
are safe. Never blanket-prune containers, delete unrelated sessions, rewrite
user config, recursively remove unverified paths, or delete user or other-task
changes or project directories. Worktree removal, branch deletion, merge,
push and publication remain subject to their existing ownership and
operator-authorization boundaries. Attribute and report a dirty tree rather
than forcibly cleaning it.

Keep durable task records, numbered evidence, review verdicts and needed
failure artifacts; link existing evidence rather than duplicating it. Retain
scratch containing the sole remaining evidence with a reason until a durable
copy is verified. Preserve counterevidence and valid audit links. Generated
Python caches/environments and run outputs belong outside source or in ignored
paths; fixtures and dependency locks remain versioned. This rule implies no
general transcript retention or deletion policy.

## Record the outcome

Append a concise outcome for each owned resource: **released**, with evidence;
**retained**, with reason; **unsupported**, with the host limitation; or
**failed/pending**, with the next recovery action. Include actual clock
observations and enough exact identity and ownership evidence to support the
outcome without secrets. For a run-wide report write an immutable
`reviews/cleanup-<run-id>-<sequence>.md`, link it from affected task logs using
the existing six-field grammar, and create a new numbered supplement for a
later attempt rather than editing a completed report. Separate delivery and
test verdicts from teardown status: a cleanup failure does not retroactively
change the implementation or test verdict.
