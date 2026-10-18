## Builds and tests go through the host queue

The operator's policy requires this: `use_queue = true` for your scope. Every
build and every test run on this machine goes through the host queue. Submit
the command to the queue and collect its recorded outcome. Do not run it
yourself.

Submit a command with `planar-build-queue run --tag <your claim token> -- <command>`.
Collect its result with `planar-build-queue wait <seq>` after a detached submit,
or run it in the foreground and read its result there.

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
`cancelled` or `wait_timeout` is an outcome of a ticket you hold, and
`planar-build-queue rule` prints the table of outcomes that says what to do.
The command is still never run directly.

### Finding the queue binary

Run `planar-build-queue rule >/dev/null`; exit 0 means it is installed. Exit 127
from the shell (command not found) means the install predates the split: use
the installed `planar-agent queue run --claim <token>` instead, as that release
documents. Any other non-zero exit from `planar-build-queue` is a refusal to
report, never a reason to fall back.
