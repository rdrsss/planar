## Builds and tests go through the host queue

Every build and every test run on this machine goes through one host-wide
queue, so that agents in different projects do not build at the same time.
Submit the command to the queue and collect its recorded outcome. Do not run
it yourself.

### What counts as a build or test command

A command goes through the queue when it does any of these:

- compiles or links code;
- runs a test suite, or any part of one;
- uses more than one core for more than a minute.

Examples: `make`, `make test`, `cmake --build`, `ninja`, `ctest`, `cargo build`, `cargo test`, `go build`, `go test`, `npm test`, `pytest`, `tox`, `gradle build`, and linters and static analysers that build first.

Not queued: reading files, searching, version control, formatting a single file, a program's `--help`, and the Planar commands themselves.

When a command is not on the list and you are unsure, queue it. Queuing a
cheap command costs a short wait. Not queuing an expensive one costs every
other agent on the host.

### Before the first submission

Check once per session that the installed Planar has the queue:

```
planar-agent queue rule >/dev/null
```

Exit 0 establishes that the queue exists. For a nonzero exit, inspect the
diagnostic and installed command catalog; only a verified absence of the
`queue` verb permits the no-queue path below. A queue that exists but cannot
be read has refused. Do not use general `--help` alone as an absence test:
older Planar versions can answer unknown-command help with general help and
exit 0. If the queue exists, discover the `wait` leaf once using the compact
schema or leaf help. A queue-capable older install can lack `wait`; use the
finite compatibility path below. A failed queue command is not proof that
the leaf is unsupported.

### Submit, then observe one ticket

Submit the command detached. Agent harnesses stop long foreground commands,
and a wait behind other builds can be long.

```
planar-agent queue run --detach --timeout 2h --vendor <vendor> --role <role> --claim <token> -- <command> [args...]
```

Run it from the directory the command needs. The command runs there, with
your environment.

Pass `--claim` only when holding a caller-supervised claim. Always pass
`--vendor` and `--role`. Give your own vendor (`claude`,
`codex`, `copilot`, `gemini`) and your own role (`coder`, `test-coder`,
`reviewer`, `janitor`, `orchestrator`). The operator reads them in the
queue's listing and history to see which agents use the queue. Planar does
not guess them.

On success the command prints two lines and exits 0: the ticket's sequence
number, then the path of the output file. Keep both. The command has not
run yet.

A non-zero exit from the submit means no ticket was issued and nothing was
queued. With 126 or 127 the command could not be executed or was not found:
fix the command line. With 1 or 2 the invocation is wrong, the program is a
model launcher, or a duration is invalid: fix the invocation. With 125 the
queue failed: stop and report, as in "If the queue refuses" below.

Wait for the issued ticket with a finite observation budget covering expected
queue backlog plus command runtime. For example, three hours covers an
expected hour of backlog and a two-hour run limit:

```
planar-agent queue wait <seq> --timeout 3h --json
```

The observer defaults to 30 minutes; an explicit duration must be positive
and at most 24 hours. `queue run --timeout` limits runtime after the command
starts, and `queue run --wait-timeout` separately limits its time in backlog.
Neither is the observer budget. If an agent needs a shorter turn, use one
finite slice such as `queue wait <seq> --timeout 10m --json`. Record the
result and explicitly decide whether to do independent work, hand off the
same ticket, or wait again. Do not wrap slices in an unbounded retry loop.
A suspended agent session will not be woken by the queued command; a later
session must resume from the saved ticket.

Read `wait_reason` and `status.outcome` together. `completed` means the
logical ticket has a recorded final outcome. `timed_out` or `interrupted`
stops only the observation: the command can remain queued or running. Inspect
or wait again on the **same sequence number** after an explicit decision.
Never duplicate submission or start a conflicting build merely because
observation stopped. `stalled`, `history_unavailable`, and `error` leave the
job outcome unknown; inspect the ticket and queue health and report the gap.
An empty log, absent `status`, or empty active queue proves no result.
`queue wait` is read-only and never renews a claim. `queue run --claim`
renews a caller-supervised claim while the submitter waits and runs; a failed
renewal appears in the output file without stopping the command. Heartbeat
separately when that renewal is unavailable.

The command's output, standard output and standard error together, is in the
output file from the ticket. Read the end of it first. It is kept for a
limited time after the command ends.

### What to do on each outcome

Only `wait_reason: completed` with `status.outcome: exited` and
`status.exit_code: 0` proves a passing command. Use the recorded outcome,
not a guess from the process exit or other output.

| Outcome | What happened | What to do |
|---|---|---|
| `exited` with `exit_code` 0 | The command ran and passed | Continue |
| `exited` with another `exit_code` | The command ran and failed | Read the output file, fix the cause, submit again |
| `signaled` | A signal terminated the command; `signal` names it | Read the output file. Report it if it is not your command's doing |
| `timeout` | The command reached its run limit (30 minutes by default) and was stopped | Do not simply resubmit. Find why it is slow or hung. Raise the limit with `--timeout <duration>` only when the command legitimately takes longer |
| `cancelled` | Someone cancelled the entry; `cancelled_by` says who | Do not resubmit unless told to. Report it |
| `wait_timeout` | The `--wait-timeout` you set ran out before the command's turn | Report it, or submit again with a longer limit |
| `not_started` | The command could not be started at its turn: not found (127) or not executable (126) | Fix the command line and submit again |
| `abandoned` | The entry has no recorded final logical-job result | Await a successor within the finite budget, or report uncertainty; never assume completion or resubmit automatically |

A child can itself exit 124 or 125. An observer timeout can exit 124; queue
refusal, stalled observation, or another observer error can exit 125. The
structured reason and recorded outcome distinguish them. An abandoned row
without a successor remains pending to the finite deadline; missing
successor history is uncertainty, never success.

Do not queue a model launcher (`claude`, `codex`, `gemini` and similar). The
queue refuses one at exit 2 and runs nothing.

### Short commands in the foreground

For a short command, or in a script, run it in the foreground. The command's
output comes straight back:

```
planar-agent queue run --vendor <vendor> --role <role> -- <command> [args...]
```

If you hold a task claim, add `--claim <token>` here too. The exit status is
the command's own. The queue adds these: 124, the command
was stopped at its run limit; 125, the queue failed; 126, the command could
not be executed; 127, the command was not found; 128 plus N, a signal N
terminated it. A command can exit with any of those itself, so when the code
matters, add `--notices`. The last line on standard error then names the
sequence number and the outcome, and `queue status <seq> --json` gives the
authoritative record.

### Older queue-capable installs without native wait

Use foreground `queue run` only for short commands or scripts that can wait
through backlog and runtime. For a long command under an agent harness, keep
the detached ticket and use a bounded, error-checking JSON status observer.
An already-issued ticket uses the same observer without resubmission.

The fallback has one fixed overall deadline covering backlog, runtime,
status-helper calls, and cleanup. Its read is:

```
planar-agent queue status <seq> --json
```

Give each status helper a finite timeout. Reserve a finite cleanup allowance
*before* launching it;
do not launch another helper if the remaining budget cannot cover its timeout
and cleanup. Check its exit and parse its JSON. Stop on status errors instead
of retrying an unreadable store. `state: ended` is terminal only with a
recorded logical-job outcome; never infer completion from a missing `live`
line. Follow explicit successors. An abandoned row without a successor stays
uncertain until the deadline, and missing successor history is an error.
Active `live: null` is unknown. Confirm active `live: false` on the same
sequence at least one second later before reporting stalled, never completed.
Do not add an unbounded outer retry. On expiry or interruption, return
control and preserve the same ticket for later observation.

The fallback owns only its status subprocesses and pipes. On success, error,
timeout, interruption, or partial startup, close its pipes, stop a still
running status helper, and reap it within the reserved cleanup allowance.
Address only its own helper identity or process group. Never signal, cancel,
or reap the detached submitter, queued command, or their process group; never
release the claim or delete logs. No helper or background poller survives the
observation episode.

### If the queue refuses (exit 125): stop and report

A submit that exits 125 means the queue exists and failed. A wait that exits
125 can instead report a recorded child exit 125, cancellation, pre-run wait
timeout, stalled observation, or observer error. Inspect `wait_reason` and
`status.outcome`; process exit alone is ambiguous. For a foreground run,
`--notices` gives the sequence number and outcome. Its record is available
with `queue status <seq> --json`.

When a submit exits 125, or the queue cannot be used (its store cannot be
opened, or it failed inside), stop. Report the `error:` line to the operator,
word for word, and say which command you were trying to run. The command must
not be run directly. Do not retry in a loop, and do not look for another way
to run the build. The operator decided that an unreachable queue is refused,
not bypassed.

An entry that ended `cancelled` or `wait_timeout` is not this case. It is an
outcome of a ticket you hold, and the table above says what to do. The
command is still never run directly.

### If Planar has no queue: run directly and say so

Use this only after verifying the installed Planar lacks the `queue` verb.
Nonzero `queue rule` alone does not establish absence. An older install with
`queue` but without `wait` uses the finite compatibility path above.

Run the build or test command directly. Then tell the operator that Planar
needs upgrading so that builds and tests can go through the queue.

### Do not confuse the two

| What you see | What it means | What to do |
|---|---|---|
| Installed catalog confirms no `queue` verb | No queue exists | Run the command directly and tell the operator Planar needs upgrading |
| Queue exists but `wait` is unsupported | Older queue-capable install | Use foreground for short/script work or finite JSON status observation of a detached ticket |
| Queue refuses or its store is unreadable | No valid gate result | Stop and report. Do not run the command directly |

Discover the capability once; never treat a refused queue as an absent one.
