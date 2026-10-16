# The host queue

`planar-agent queue` is one line, shared by every project on the machine, for
commands that are expensive to run, such as builds and test suites. A command
submitted to it waits for its turn, runs in the submitter's directory with the
submitter's environment, and has its outcome recorded. The queue is a
coordination aid and not a security boundary. This reference says how to use it.

## Whether to use it is the operator's policy

Planar does not require the queue. Whether agents must send builds and tests
through it is the operator's policy, `use_queue` in `config.toml`:

```toml
[policy]
use_queue = true

[associations."org:acme".policy]
use_queue = false   # an association's table beats the top-level one
```

- The key is read from the scope's effective policy: an association's
  `[associations."<slug>".policy]` table over `[policy]` over the default,
  which is `false`. No environment variable overrides it.
- `planar-agent brief <plan-id>` shows the effective value. It shows queue
  command templates only when the value is `true`.
- `planar-agent queue rule` always prints how to use the queue. It adds the
  instruction to send every build and test through the queue, followed by a
  `Source:` line naming the policy, only when the caller's scope has
  `use_queue = true`.
- A `task_gate` policy requires `use_queue = true` in the same scope, because
  its evidence is a queued run (see [Gate evidence](#gate-evidence)).

With `use_queue` unset or `false`, the queue is an available tool.

## Verbs

| Verb | What it does |
|------|--------------|
| `queue run [flags] -- <command>` | Waits for the command's turn, runs it, exits with its status. |
| `queue wait <seq>` | Observes one ticket until its outcome is recorded or a finite observer budget ends. Read-only. |
| `queue status <seq>` | Reports the entry's state and place in the queue, how it ended, the exit status and where its output was saved. Read-only. |
| `queue cancel <seq>` | Removes a waiting entry or stops a running one. |
| `queue rule` | Prints the how-to above and, where the policy requires it, the mandate. |

Flags are in `planar-agent queue <verb> --help`.

### Submitting

Foreground, for a command that finishes quickly. The command's output comes
straight back and the exit status is the command's own:

```sh
planar-agent queue run --vendor <vendor> --role <role> -- <command> [args...]
```

Detached, for a command that may wait behind others or run long:

```sh
planar-agent queue run --detach --timeout 2h --vendor <vendor> --role <role> -- <command> [args...]
```

A detached submit prints two lines and exits 0: the ticket's sequence number,
then the path of the output file (`queue-logs/<seq>.log` beside the database).
The command has not run yet. Output, standard output and standard error
together, is kept in that file for a limited time after the command ends.

Flags that matter on `queue run`:

- `--vendor` and `--role` are recorded with the entry (defaults
  `$PLANAR_VENDOR` and `$PLANAR_ROLE`), so the operator can see in the listing
  and history who uses the queue.
- `--claim <token>` renews that claim at half its lease while the entry waits
  and while the command runs (see [Claim renewal](#claim-renewal)).
- `--timeout <duration>` limits how long the command may run once started;
  default 30 minutes, at most 24 hours. `--wait-timeout <duration>` limits how
  long the entry may wait for its turn; default no limit.
- `--label` records an operator-facing label. `--notices` writes the queue
  position, start and final outcome to standard error.

The queue refuses a model launcher (`claude`, `codex`, `gemini` and similar) at
exit 2 and runs nothing.

### Observing a ticket

```sh
planar-agent queue wait <seq> --timeout 3h --json
```

`--timeout` is the observer's budget, default 30 minutes, at most 24 hours; it
is not the command's run limit. It stops only the observation. Read
`wait_reason` and `status.outcome` together:

- `completed` means the ticket has a recorded final outcome. `status.outcome`
  `exited` with `status.exit_code` 0 is a passing run.
- `timed_out` and `interrupted` mean the observer stopped; the command can still
  be queued or running. `wait` again on the same sequence number rather than
  submitting a second time.
- `stalled`, `history_unavailable` and `error` leave the outcome unknown. An
  empty output file, an absent `status` or an empty active queue is not a result.

Recorded outcomes of `queue status`:

| Outcome | Meaning |
|---------|---------|
| `exited` | The command ran; `exit_code` is its status. |
| `signaled` | A signal ended the command; `signal` names it. |
| `timeout` | The run limit passed and the command was stopped. |
| `cancelled` | Someone cancelled the entry; `cancelled_by` says who. |
| `wait_timeout` | `--wait-timeout` passed before the command's turn. |
| `not_started` | The command could not be started at its turn: not found (127) or not executable (126). |
| `abandoned` | The entry has no recorded final result; a successor entry may follow. |

`queue wait` follows successors. A queue-capable install older than `wait` has
no such verb; `queue status <seq> --json` is the read it offers instead, and
`state: ended` is final only with a recorded outcome.

### Exit codes of `queue run`

The exit status is the command's own, passed through unchanged (a signal N
becomes 128 plus N). The queue adds: 1, usage error, nothing run; 2, refused
before queueing (a model launcher, or an invalid `--timeout`); 124, the command
was stopped at its run limit; 125, the queue failed; 126, the command could not
be executed; 127, the command was not found. A command can exit with any of
those itself, so add `--notices` when the code matters, and read
`queue status <seq> --json` for the authoritative record.

## Exit 125: the queue refused

Exit 125 from `queue run` means the queue failed and the command did not run:
`planar.db` is unreachable, behind the binary, or ahead with queue tables the
binary cannot use, the wait limit passed, the entry was cancelled, or there
was an internal error. Nothing is recorded for a submit that was refused. The
refusal is exactly one `error: queue: <reason>; <remedy>` line. The queue never
runs the command itself as a fallback. When `use_queue` is on, the mandate says
a refused queue is reported and not bypassed.

`queue wait` exiting 125 can also carry a recorded child exit 125, a
cancellation, a pre-run wait timeout, a stalled observation or an observer
error; `wait_reason` and `status` tell them apart. A cancelled or `wait_timeout`
entry is an outcome of a ticket, not a queue failure.

## Claim renewal

`queue run --claim <token>` renews the claim at half its lease while the entry
waits and while the command runs, so a long build does not lapse a lease. A
renewal that fails is reported on standard error, or in the output file for a
detached run, and never stops the command. `queue wait` is read-only and never
renews a claim; heartbeat separately with `planar-agent heartbeat --claim
<token>` when `wait` is the only call in flight.

## Gate evidence

When the scope's policy sets `task_gate`, `planar-agent complete` looks in the
queue's history for a passing run whose argv equals the gate element by
element, whose recorded claim token is the completing claim's, and whose
directory is the claim's repository root or worktree. Submit it as:

```sh
planar-agent queue run --claim <token> --vendor <vendor> --role <role> -- <gate argv>
```

from the claim's repository root or worktree. A run in a subdirectory, with a
different argv or without the claim token does not count. `planar config validate`
refuses a `task_gate` without `use_queue = true` in the same scope.

## Schema tolerance

The queue's tables (`queue_entries`, `queue_history` and `queue_schema`) live
in `planar.db`, and `planar-agent` is the only writer.

- `queue` verbs refuse a `planar.db` that is behind the binary, at exit 125 with
  `schema_version_behind`; `planar init` brings it forward.
- Against a `planar.db` that is ahead, they run when the queue's own
  compatibility check passes: the tables and marker are present, the marker's
  `compat` is not above this build's, and every column the binary reads exists.
  When the check fails they refuse at exit 125 with `queue_schema_incompatible`
  (ahead; a newer build fixes it) or `queue_schema_foreign` (same version
  number, different migration; a newer build does not fix it). Neither is the
  exit 7 `schema_version_ahead` that other verbs give.
- Claims and heartbeats still require `planar.db` at exactly the binary's
  version. Against an ahead database, `queue run --claim` prints a warning that
  it cannot renew the claim, the command still runs, and the lease can lapse. A
  `planar-agent` built from the same source as the database renews it; so does a
  separate `planar-agent heartbeat` from that build.

## An install without the queue

An installed `planar-agent` older than the queue has no `queue` verb. Check the
installed command catalog (`planar-agent schema`) rather than a nonzero exit of
`queue rule`, which can mean the queue exists and refused.
