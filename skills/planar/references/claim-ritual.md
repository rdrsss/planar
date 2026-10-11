# Claim ritual

A claim is the live ownership record for a unit of work. Task status alone cannot say who
is working on what: a task can be `todo` while another vendor already holds it. Every
code-writing dispatch therefore runs claim, heartbeat, and exactly one terminal verb, all
through `planar-agent`. There is no `planar agent` namespace.

## Before claiming: pick claim-aware work

1. Ask for next work with `planar plan next <plan-id>` or `planar-agent peek <plan-id>`.
   Both skip tasks under a live claim. Never choose from `planar task list` filtered to
   `todo`. `plan next --json` always carries every bucket (available, claimed, stale,
   blocked); its text form hides claimed and stale unless asked; see
   `planar plan next --help`.
2. Check readiness with `planar task packet <task-id> --json`. A claim still succeeds on
   an unready packet, so `ready: false` is your stop signal, not the claim's. A spec
   artifact edited mid-run turns every packet under that anchor `ready: false`; the
   refresh is an operator-confirmed `planar spec ingest <anchor> --apply` (see
   [spec-pipeline.md](spec-pipeline.md)).

## Taking the claim

- **Next task on a plan:** `planar-agent pull <plan-id>` atomically picks the next
  eligible task, claims it, flips it to `doing` and opens a top-level action. When nothing
  is available it returns `no_work` and the agent stops cleanly.
- **A hand-picked task:** `planar-agent claim --entity task:<id>`. A direct task claim
  also flips `todo` to `doing`, so the ordinary terminal verbs work afterwards.
- **Coordination:** an orchestrator claims `plan:<id>` and never uses `pull` for itself,
  because `pull` consumes a feature task. Its coders pass `--parent-action` so the action
  tree links them; without it each coder's action is a disconnected root.
- Size the lease to the work with `--ttl` on `pull` or `claim`; the default is 600
  seconds, too short for a long dispatch. Flags: see `planar-agent claim --help` and
  `planar-agent pull --help`.

## Heartbeating

- Heartbeat at least once per half the TTL: `planar-agent heartbeat --claim <token>`.
- Omitting `--ttl` renews the lease length the claim already holds; an 8-hour claim stays
  8 hours. Pass `--ttl` only to change the length deliberately; it is then absolute, in
  either direction.
- Heartbeat immediately before and after any long tool call.
- Read `expires_at` from the heartbeat output to confirm the lease moved. "Sent" is not
  proof.

### Status strings

Add `--status "<text>"` at every phase boundary so `planar-watch ps` shows what you are
doing: `claim acquired: task <id>`, `reading brief`, `editing <area>`,
`validating: <gate-id>`, `committing` or `reporting`. Prefix `awaiting:` when blocked on
something external (`awaiting:reviewer`, `awaiting:operator-confirmation`). The prefix is
a convention, not an enum. The string is capped at 256 bytes; a longer one is refused at
exit 2 and the lease is not refreshed. Do not heartbeat a status that mirrors an
entity create (a new question, decision or artifact): those are recorded automatically
under the active claim, and a manual echo duplicates the action row.

## Finishing: exactly one terminal verb

| Verb | Use when | Effect |
|------|----------|--------|
| `planar-agent complete` | The work succeeded. | Task to `done`, claim `completed`. |
| `planar-agent fail` | The work was attempted and failed. | Task back to `todo`, claim `aborted`. |
| `planar-agent release` | Giving up gracefully without attempting. | Task back to `todo`, claim `released`. |
| `planar-agent block` | An external blocker stops the work. | Task `blocked`, blocker edge added, claim `released`. |

Each terminal verb flips the claim and the task status in one transaction and recomputes
the owning plan's status. Never split it into `planar task done` followed by
`planar-agent release`: a process death between the two strands the claim.

`complete`, `fail` and `block` refuse a plan or plan-step claim (`ClaimNotOnTask`).
`release` is the one verb that also ends a plan or plan-step claim, so an orchestrator
releases its coordination claim explicitly rather than waiting out the lease.

## Orchestrated versus direct claims

- **Orchestrated.** The orchestrator claims or pulls and hands the token to a coder in the
  brief. The coder heartbeats with status strings and returns its report; it never runs
  `complete`, `fail`, `release`, `block`, `planar task done` or any other status write.
  The orchestrator runs the one terminal verb after review and, in worktree isolation,
  only after the coder's commit is merged into the integration branch. Confirm the merge
  with `git merge-base --is-ancestor <coder-sha> HEAD`; a merge of a misspelt branch name
  can report "Already up to date" and exit 0.
- **Direct.** The agent that claimed the work runs the terminal verb itself, after its
  validation evidence is green.
- **Engine-supervised.** Not live in this build: every claim here is caller-supervised.
  If a brief says a claim is engine-supervised, heartbeat only with `--status` and no
  `--ttl`, and run no terminal verb. A bare heartbeat and any terminal verb are refused
  with `SupervisorMismatch`; only the engine extends or ends that lease.

## Lapsed-claim recovery

When `complete` fails with `ClaimNotActive` because the lease expired, but the task is
still `doing` and the work is done:

1. Do not reach for `planar task update --status done`.
2. Re-claim without a status change. A plain `claim` fails with `IllegalTransition`,
   because it retries `todo` to `doing` on a task that is already `doing`:

   ```sh
   planar-agent claim --entity task:<id> --no-transition --json
   ```

3. Run the one terminal verb with the new token.

Before re-claiming, make sure no other session has taken the task meanwhile:
`planar-watch ps --stale` lists active and stale claims together. Act only on your own
task.

## Builds and tests go through the host queue

In a Planar-managed repository, run builds and test suites through the host-wide queue
instead of directly: `planar-agent queue run --detach --vendor <vendor> --role <role> --
<command>`, then use `planar-agent queue wait <seq> --timeout <budget> --json`.
Choose a finite budget covering expected backlog and runtime, or use one short
slice and save the ticket for an explicit later wait on that same sequence.
Observation timeout or interruption leaves the job running; inspect the
structured reason and recorded outcome before judging the gate. An older
queue-capable install without `wait` uses the finite JSON-status fallback in
the queue rule; refusal never authorizes a direct build.
`planar-agent queue rule` prints the full rule. The queue runs the command in your
directory with your environment and exits with its status; it coordinates every project
on the host and is not a security boundary. When a `queue` verb refuses at exit 125,
stop and report; never fall back to running the command directly. `queue run --claim
<token>` renews your lease while the command waits and runs; against a database that is
ahead of the binary it warns that it cannot renew, and the lease can lapse, so in a
migration cycle use the freshly built `planar-agent` for that renewal. An installed
`planar-agent` older than the queue has no `queue` verb at all; say so in the report and
run the gates directly.

Size the gates to the scope. A task runs focused tests for its acceptance signal and the
modules it touched, plus the format, build, parity and policy checks its change can
break; a filtered run that matches no tests is a failure, so report the matched count.
The full regression suite runs once at the milestone barrier on the merged candidate,
not after every task, and a task pass is never reported as a full pass. The repository's
own contributor guide names the exact commands for each scope.

## Operator-side claim recovery

- `planar-agent reconcile --dry-run` previews expired claims and orphaned actions; the
  same verb without `--dry-run` marks them stale. It only touches claims whose lease has
  already expired, never a live, heartbeating claim. The dry run can surface another
  session's stale claim: never reconcile work that is not yours without the operator.
- `planar-agent abort --claim <token> --reason <text>` force-releases one specific claim
  regardless of owner. It is an operator action.
- A stale claim leaves the task `doing`; a later `pull` can take it again.
- Full doctor flow: [recovery.md](recovery.md).

The full contract is in `methodology.md` in the Planar agents directory, under
Coordination claims and Heartbeat status contract.

Report results with the envelope in [feedback-contract.md](feedback-contract.md).
