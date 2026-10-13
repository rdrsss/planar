## Builds and tests go through the host queue

The operator's policy requires this: `use_queue = true` for your scope. Every
build and every test run on this machine goes through the host queue. Submit
the command to the queue and collect its recorded outcome. Do not run it
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

### If the queue refuses, never bypass it

When a submit exits 125, or the queue cannot be used, the command must not be
run directly. Do not look for another way to run the build. The operator
decided that an unreachable queue is refused, not bypassed. An entry that ended
`cancelled` or `wait_timeout` is an outcome of a ticket you hold, and the table
of outcomes says what to do. The command is still never run directly.

### If Planar has no queue: run directly and say so

Use this only after verifying the installed Planar lacks the `queue` verb.
Nonzero `queue rule` alone does not establish absence. An older install with
`queue` but without `wait` uses the finite compatibility path.

Run the build or test command directly. Then tell the operator that Planar
needs upgrading so that builds and tests can go through the queue.

### Do not confuse the two

| What you see | What it means | What to do |
|---|---|---|
| Installed catalog confirms no `queue` verb | No queue exists | Run the command directly and tell the operator Planar needs upgrading |
| Queue exists but `wait` is unsupported | Older queue-capable install | Use foreground for short/script work or finite JSON status observation of a detached ticket |
| Queue refuses or its store is unreadable | No valid gate result | Stop and report. Do not run the command directly |

Discover the capability once; never treat a refused queue as an absent one.
