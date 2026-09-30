## Builds and tests go through the host queue

Every build and every test run on this machine goes through one host-wide
queue, so that agents in different projects do not build at the same time.
Submit the command to the queue and poll for its result. Do not run it
yourself.

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

If it exits 0, use the queue as described below. If it exits with any other
status, follow "If Planar has no queue" below. Do not use `--help` for this
check: an older Planar answers an unknown command's `--help` with its general
help and exits 0, so the check would pass when there is no queue.

### Submit, then poll

Submit the command detached. Agent harnesses stop long foreground commands,
and a wait behind other builds can be long.

```
planar-agent queue run --detach --vendor <vendor> --role <role> -- <command> [args...]
```

Run it from the directory the command needs. The command runs there, with
your environment.

Always pass `--vendor` and `--role`. Give your own vendor (`claude`,
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

Then ask for the ticket's state every 30 seconds:

```
planar-agent queue status <seq>
```

Use 30 seconds. The waiting entry advances on its own every second, so a
faster poll finishes nothing sooner. It only spends your turns. A longer
interval makes you notice a finished build late. Keep doing any work that
does not need the build's result between polls. If you hold a task claim,
add `--claim <token>` to the submit: the queue renews that claim while the
entry waits and while the command runs, so you do not renew it between polls.
A renewal that fails is reported in the output file and does not stop the
command.

Read the `state` line:

- `waiting`: the `position` line gives its place. Poll again.
- `running`: the command is running. Poll again.
- `ended`: read the `outcome` line and act as below.

Add `--json` to `queue status` to read the same answer as one JSON object.

The command's output, standard output and standard error together, is in the
output file from the ticket. Read the end of it first. It is kept for a
limited time after the command ends.

### What to do on each outcome

The `outcome` line of `queue status` is the record of how the command ended.
Use it, not a guess from other output.

| Outcome | What happened | What to do |
|---|---|---|
| `exited` with `exit_code` 0 | The command ran and passed | Continue |
| `exited` with another `exit_code` | The command ran and failed | Read the output file, fix the cause, submit again |
| `signaled` | A signal terminated the command; `signal` names it | Read the output file. Report it if it is not your command's doing |
| `timeout` | The command reached its run limit (30 minutes by default) and was stopped | Do not simply resubmit. Find why it is slow or hung. Raise the limit with `--timeout <duration>` only when the command legitimately takes longer |
| `cancelled` | Someone cancelled the entry; `cancelled_by` says who | Do not resubmit unless told to. Report it |
| `wait_timeout` | The `--wait-timeout` you set ran out before the command's turn | Report it, or submit again with a longer limit |
| `not_started` | The command could not be started at its turn: not found (127) or not executable (126) | Fix the command line and submit again |
| `abandoned` | The entry was removed before it finished: its submitter died, or another process removed it. The command may not have run | Read the output file to see whether it ran, then submit again once. If it is abandoned again, report it |

An entry that has not ended and shows `live: false` has lost its submitter.
Poll again. If it is still there after two polls, report it.

`queue status` exits 1 when no such ticket exists, and 125 when the queue's
store cannot be read; for 125, follow "If the queue refuses" below.

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
sequence number and the outcome, and `queue status <seq>` gives the record.

### If the queue refuses (exit 125): stop and report

A queue command that exits 125 means the queue exists and failed: its store
cannot be opened, the wait limit was reached, the entry was cancelled, or the
queue failed inside. Usually the command did not run. It may have, if a
running entry was cancelled, and a command can itself exit 125. If you passed
`--notices`, the last line on standard error says whether the queue or the
command produced the 125. `queue status <seq>` gives the record.

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

Use this only when `planar-agent queue rule >/dev/null` exits with a non-zero
status. That means the installed Planar is older than the queue, or
`planar-agent` is not installed, and there is no queue to use.

Run the build or test command directly. Then tell the operator that Planar
needs upgrading so that builds and tests can go through the queue.

### Do not confuse the two

| What you see | What it means | What to do |
|---|---|---|
| `planar-agent queue rule >/dev/null` exits with a non-zero status | There is no queue | Run the command directly and tell the operator Planar needs upgrading |
| Any queue command exits 125 | The queue exists and refused | Stop and report. Do not run the command directly |

The first row is a fact about the installed Planar, checked once with `queue rule`.
The second is an answer from a queue that is there. Never treat the second as
the first.
