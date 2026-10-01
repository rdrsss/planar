# Testing

How Planar's test suite is layered, what each gate proves, and the rules for
adding to it. The build and toolchain are covered in
[toolchain-parity.md](toolchain-parity.md); the state machines the scenarios
walk are in [lifecycles.md](lifecycles.md).

## Gates

| Command | What it proves |
|---------|----------------|
| `make test` | Builds the `debug` preset, then runs every Catch2 case under `ctest` in parallel. `TEST_JOBS=1` runs them serially. |
| `make test-cpp-report` | The same suite, plus its skip tally. The expected tally is zero. |
| `make ctest-registry-check` | ctest runs exactly the cases the test binaries contain. Needs `build/debug` built. |
| `make coverage` | The `(verb, subcommand)` leaf-coverage ratio has not dropped below `scripts/coverage-baseline.txt`. |
| `make cli-usage-check` | Authored surfaces (`agents/`, `skills/src/`, `docs/`) only use commands and flags the five binaries expose, and pass the semantic surface lint, including the host-queue rule (`surface-queue-command`, [architecture.md](architecture.md#authored-surface-validation)). |
| `make surface-check` | Each binary's live schema and help surface matches `scripts/surface-baseline.txt`. |
| `make exit-code-contract` | The exit codes documented in [cli-reference.md](cli-reference.md) are the ones the binaries return. |
| `make eval-contracts` | The provider-free eval lanes. |
| `make cpp-lint-gate` | `clang-format --Werror` and the Doxygen doc-comment pass. |
| `make test-all` | All of the above, composed. This is the gate to run before a pull request. |
| `make cpp-lint` | `cpp-lint-gate` plus `clang-tidy`. Not part of `test-all`. |
| `make linux-gate` | The `debug` build and the whole ctest suite on Debian trixie in Docker (native arm64). Not part of `test-all`. See [The Linux gate](#the-linux-gate). |
| `make test-cpp-solver` | The ctest suite against a `-DPLANAR_WITH_MTKAHYPAR=ON` build. |

## Builds and tests go through the host queue

On a machine shared by several agents and projects, a build or test is not
started directly. It is submitted to the host-wide queue, so two builds do
not run at once and one agent's `make test` does not starve another's. The
queue is `planar-agent queue`; see [the queue verbs](cli-reference.md#queue-verbs)
for the full contract and [operations.md](operations.md#5-the-host-build-and-test-queue)
for running it. `planar-agent queue rule` prints the rule agents follow.

Every gate above has a queued form: the gate's own command after the argument
terminator, run from the directory it needs.

```bash
# Short gate, in the foreground: the command's output and exit status come straight back.
planar-agent queue run --vendor <vendor> --role <role> -- make fmt-check     # cli-lint-ignore: `--` is the argument terminator
planar-agent queue run --vendor <vendor> --role <role> -- make cli-usage-check     # cli-lint-ignore: `--` is the argument terminator

# Long gate, detached: prints a sequence number and a log path, then returns at once.
planar-agent queue run --detach --vendor <vendor> --role <role> -- make test     # cli-lint-ignore: `--` is the argument terminator
planar-agent queue status <seq>                                                  # poll every 30 seconds until state is ended
```

`make test-all`, `make cpp-lint` and `make linux-gate` run long enough to
reach the queue's default run limit of 30 minutes, at which the command is
stopped and its entry ends as `timeout`. Pass `--timeout <duration>` for a
gate that legitimately takes longer (for example `--timeout 2h`), and
`--claim <token>` when the caller holds a task claim, so the queue renews it
while the command waits and runs.

A detached gate builds when its turn comes, not when it is submitted. Submit
it only once the tree is final for that gate, and do not edit the tree until
its ticket has ended. Read the outcome from `queue status`, which is the
record of how the command ended, not from the log. A probe rebuild and a gate
write the same build directory, so submit both through the queue and never
run one beside the other.

The queue is for builds and tests that would otherwise run side by side. It
does not replace the gates: a queued `make test` is the same `make test`.
The test suite's own black-box cases never touch the real queue, because
`run_pinned` gives each one its own `PLANAR_DB`, and the queue lives in that
file.

`clang-tidy` is advisory. The recipe runs it without `--warnings-as-errors`
and `.clang-tidy` declares no `WarningsAsErrors` key, so its warnings never
fail a run. It enables one check, `readability-identifier-naming`.

## The Linux gate

`make linux-gate` builds the `debug` preset and runs the whole ctest suite in
a `debian:trixie-slim` container, so code with a Linux-only branch
(`close_range` and `/proc/self/fd` in `src/lib/process`, `pipe2` in the
runner, `/proc/<pid>/fd` in the queue tests) is compiled and run somewhere
other than macOS. It is the Linux lane; CI is not relied on for it.

```bash
make linux-gate                                  # full build + suite
make linux-gate LINUX_GATE_CTEST_ARGS="-L process"   # a subset
make linux-gate LINUX_GATE_JOBS=8                # build and ctest parallelism (default 4)
make linux-gate-prune                            # docker builder prune -f
```

`docker/linux-gate.Dockerfile` installs apt.llvm.org's LLVM 23 (clang,
libc++ with its modules manifest, libc++abi), Kitware CMake pinned by version
and SHA-256, ninja, git, python3, `sqlite3`, `libtbb-dev`, `libssl-dev` (vendored libcurl's TLS on Linux) and `patch` (CPM's `PATCHES` keyword). The image
build never fails on a red suite. It records `configure.log`, `build.log`,
`ctest.log` and `status.txt`, and the Makefile exports them to
`build/linux-gate/`, prints the ctest verdict and exits nonzero unless
`status=0`. Read `ctest.log` there rather than the build output: BuildKit
clips a step's log at 2 MiB.

Centurion lives in the gitignored `external/` directory and its repository is
private. The gate does not fetch it and no token enters an image layer. The
Makefile passes the pinned tag's already-populated directory (from this
checkout, or from the primary checkout when run in a linked worktree) as a
named build context and bind-mounts it for the one `RUN` that builds. Populate
it once on the host with `GITHUB_TOKEN=$(gh auth token) cmake --preset debug`,
or set `LINUX_GATE_EXTERNAL`.

The build tree lives in a BuildKit cache mount, so a second run is
incremental, and it costs disk while it stays. Run `make linux-gate-prune`
when done; a past unpruned run of this kind filled the machine's disk.
Docker is a developer tool and not one of the installer's `BUILD_DEPS`.

**Disk cost.** One full `make linux-gate` run grew Docker's virtual disk by
about 16 GB, and the cache mount is kept after the run. Have at least 25 GB
free before starting, and run `make linux-gate-prune` afterwards, every time.
Do not run the gate on a machine that is short of disk. Through the queue
it is submitted detached with a longer run limit, and it holds the queue's
slot for its whole run:

```bash
planar-agent queue run --detach --timeout 2h --vendor <vendor> --role <role> -- make linux-gate     # cli-lint-ignore: `--` is the argument terminator
```

## When every build is a full rebuild

A build with no source change should run zero steps. If it recompiles
thousands of objects, ninja's dependency log, `build/<preset>/.ninja_deps`,
is probably damaged. Two builds running in the same build directory at once
can interleave their records. Ninja then stops reading at the first bad
record on every run and discards everything recorded after it. The only sign
is one line in the build output:

```
ninja: warning: premature end of file; recovering
```

`scripts/ninja-deps-check.py <build-dir>` reports a damaged log, and
`--repair` removes it. The Makefile runs the repair before each debug build.
The build that follows recompiles everything once. To see why ninja
considers an object stale, run `ninja -C build/debug -d explain`.

The Makefile configures the debug tree only when it has never been
configured. Ninja re-runs CMake itself when a CMake input or a globbed
directory changes. After changing a preset or a `-D` option, run `cmake
--preset debug` by hand.

## Two tiers

Planar uses two kinds of test. Both are needed, and they are not
interchangeable.

**Unit tests** are Catch2 `TEST_CASE`s in `*.t.cpp` files colocated with the
module under test throughout `src/lib/`, `src/engine/` and `src/cmd/`. They
call the module directly, with an in-memory or per-test-tmpdir SQLite
database when they need one. They catch logic and SQL regressions.

**Black-box tests** run the built binary: argv in, and stdout, stderr, exit
code and on-disk state out. They grade the shipped contract, so an internal
refactor cannot silently change what an operator sees. They are Catch2 cases
too, and run under the same `ctest`.

Multi-command lifecycle scenarios that dispatch in-process live in
`src/cmd/integration_tests/`; see its
[README](../src/cmd/integration_tests/README.md) for the fixture rules.

## A green ctest run is not proof the suite ran

`catch_discover_tests` writes one `<target>-<hash>_tests.cmake` file per
target, and ctest runs exactly what those files list. Nothing checks that the
list still matches the binaries, and it drifts in both directions while ctest
reports "100% tests passed":

- A truncated discovery file silently drops cases. One run executed 3,685 of
  4,984 registered tests and reported a clean pass.
- A discovery file appended to rather than replaced, for a target that was
  not rebuilt, registers every name twice. Fifteen targets were once measured
  at exactly double, inflating the reported total from 3,553 to 4,871.

`make ctest-registry-check` compares each binary's own `--list-tests` count
against the `add_test` lines registered for it. It needs no baseline and
fails on a mismatch in either direction. Do not quote a ctest total as a
count of distinct tests without it.

Two related traps:

- A `ctest -R <pattern>` that matches nothing produces a green run. Check the
  matched count.
- ctest passes the case name as an argv token, so a `TEST_CASE` name must not
  start with `-`.

## What "parity" means

The word survives in file names (`src/cmd/*/parity.t.cpp`) and test tags. It
no longer means a comparison against a second implementation. Those cases pin
bytes that were transcribed from the earlier Zig and Go implementations
before they were deleted, and assert them against the current binary alone.
They remain the strongest grading on help text, exit codes, error wording and
the `schema` catalog, and they cannot be re-derived. A comment that says "the
oracle declares X" records where a pinned byte came from; do not add new
ones. `scripts/parity-data/parity-triage.md` holds the rationale for the
pins.

## Exit codes

The Exit Codes table in [cli-reference.md](cli-reference.md) is a contract
that skills and scripts branch on. The authoritative source is
`src/cmd/planar/exit.cppm`. `make exit-code-contract` runs representative
refusals on `planar`, `planar-agent`, `planar-watch` and `planar-ext`, and
also fails when the table omits a code that a checked case returns.

A parse failure exits `2` on `planar` and `1` on the other three. Exit `64`
means not implemented. When the table and a binary disagree, fix the table.

## Black-box harness

The harness is `src/cmd/parity_harness.hpp`. It is a header, included rather
than linked, because a `cmd_*` target may not depend on another `cmd_*`
target.

- `make_arena("<tag>")` builds a scratch environment under a temp root with
  its own `PLANAR_DB`, `HOME` and `PLANAR_WORKBENCH_ROOT`.
- `run_pinned(bin, argv, root, tag)` runs a binary inside that environment
  and captures stdout, stderr and the exit code.
- `launch_pinned_detached` and `await_sentinel` cover the few cases that need
  a live process, such as `planar-watch feed --follow`.

Both halves of the arena matter. `PLANAR_HOME` alone does not redirect the
database: without a scratch `PLANAR_DB` the runtime falls back to
`~/.planar/planar.db` and applies pending migrations to it. The
host queue lives in the same `planar.db` (and its detached-run logs in
`queue-logs/` beside it), so pinning `PLANAR_DB` pins the queue too: there is
no second database to redirect. `run_pinned` also removes an inherited
`PLANAR_QUEUE_SLOT`, so a suite run under `queue run` does not make every
arena submitter look like a nested run. Never run a from-source binary outside
`run_pinned`.

Two case styles coexist:

- **Focused per-leaf tests**
  (`src/cmd/planar/handlers/<family>/*_leaves.t.cpp`, `*_leaf.t.cpp`) pin one
  verb's contract: flags, JSON shape, exit code, error wording. Most call the
  binary's own `dispatch(fx, {"verb", "sub"})` in-process. They use
  `run_pinned` when the property under test belongs to the process, such as
  an exit code or a file the process creates.
- **Cross-process scenario tests** (`src/cmd/planar/cross_process.t.cpp`,
  `src/cmd/*/parity.t.cpp`) walk an operator workflow through many verbs and
  several binaries. They catch a verb that works alone and breaks in the
  flow.

## Conventions

- Name a scenario for the workflow, not the verb.
- Seed fixture state through the CLI, never by raw SQL inside a test.
- After every mutating step, assert on the post-state with `<entity> show
  --json` or `list --json`. Exit code 0 is not sufficient, because a verb
  that silently does nothing also exits 0.
- To compare a field across two steps, snapshot the JSON between them rather
  than deriving the expectation from a constant.
- Scenarios may cross verb boundaries on purpose: add a question, link it to
  a plan, advance the plan, then assert the question still appears in `plan
  show`.
- Test files open with plain `//` comments. A `///` header on a `*.t.cpp`
  makes the Doxygen pass demand a doc comment on every `TEST_CASE`.

## Contribution policy

- A change that adds a top-level verb, subcommand or flag also adds or
  extends a test that exercises it in a realistic operator workflow. A test
  that only shows the verb exists and emits JSON does not count.
- A change to a verb's JSON shape, exit-code mapping or status-transition
  rules updates the corresponding focused test in the same change.
- A change that adds an external-plane adapter or a migration adds a case
  that walks the new path end to end. HTTP adapters use the in-process
  fixture server, `src/lib/http/fixture_server.hpp`.
- `make coverage` extracts leaves from `dispatch(fx, {...})` and
  `run_pinned(cpp_bin(), ...)` call sites. `scripts/coverage-extract.py`
  documents two cases it must not count: table-driven loops, and a leaf name
  that appears only inside a pinned `schema` catalog string.

## Bug fixes are red, then green

When work surfaces a real bug, first add a test that asserts the documented
contract and fails. Commit it alone, with a subject that starts "Red test:".
Land the fix in the next commit, and the same test goes green.

Do not edit a test to assert only the subset that works. If the bug is out of
scope for the cycle, file it as a task on the anchor plan and mark the test
with `TODO(plan:<id>, task:<id>)`; the red test still goes in first.

## Helpers fail loudly

A helper that cannot honour its contract fails the case. It never returns a
plausible-looking value. `run_pinned` returns a `capture` that the caller
asserts on, and helpers built on it use `REQUIRE` with the diagnostic
attached through `INFO`.

A case that needs to inspect a non-zero exit asserts on `capture::code`
directly.

## Running a from-source binary by hand

`planar` resolves `$PLANAR_DB`, not `$PLANAR_HOME`, falls back to
`~/.planar/planar.db`, and applies pending migrations on first use. One bare
run of a binary built from a branch that carries a new migration moves the
live database past the schema version every installed binary supports. Every
other agent on the machine then fails with `SchemaVersionAhead`, and the
migration has to be rolled back by hand.

- `make smoke ARGS="<verb>"` runs the debug build against a throwaway
  `planar.db` under `build/debug/.smoke/`, as `PLANAR_DB`. `make smoke-reset`
  deletes it.
- `make run` runs against the real database.
- By hand: `PLANAR_DB=<scratch-path> build/debug/bin/planar <verb>`, with a
  scratch `HOME` as well whenever the verb touches `~/.planar` for anything
  other than the database.

The gate scripts that run a built binary (`scripts/exit-code-contract.sh`,
`scripts/surface-snapshot.sh`, `scripts/coverage-check.sh`) pin
`PLANAR_DB` under their own scratch directory, and `src/cmd/planar/arena_pins.t.cpp`
runs each of them and `make smoke` with a recording wrapper in place of the
binary to keep it that way.

To check a migration as raw SQL:

```bash
sqlite3 /tmp/cp-smoke.db < migrations/00001_foundation.up.sql
```
