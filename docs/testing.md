# Testing

How Planar's test suite is layered, what each gate proves, and the rules for
adding to it. The build and toolchain are covered in
[toolchain-parity.md](toolchain-parity.md); the state machines the scenarios
walk are in [lifecycles.md](lifecycles.md).

## Gates

| Command | What it proves |
|---------|----------------|
| `make test` | Builds the `debug` preset including the `planar_tests` aggregate (test binaries are not in `all`), then runs every Catch2 case under `ctest` in parallel. `TEST_JOBS=1` runs them serially. |
| `make test-cpp-report` | The same suite, plus its skip tally. The expected tally is zero. |
| `make ctest-registry-check` | ctest runs exactly the cases the test binaries contain. Needs `build/debug` built. |
| `make coverage` | The `(verb, subcommand)` leaf-coverage ratio has not dropped below `scripts/coverage-baseline.txt`. |
| `make cli-usage-check` | Authored surfaces (`agents/`, `skills/`, `docs/`) and the `docs.examples` the binaries publish in `schema` only use commands and flags the five binaries expose, and pass the semantic surface lint, including the host-queue rule (`surface-queue-command`, [architecture.md](architecture.md#authored-surface-validation)). |
| `make surface-check` | Each binary's live schema and help surface matches `scripts/surface-baseline.txt`. |
| `make exit-code-contract` | The exit codes documented in [cli-reference.md](cli-reference.md) are the ones the binaries return. |
| `make eval-contracts` | The provider-free eval lanes. |
| `make eval-queue-observation-integration` | A live scratch-database detached ticket observed through native wait and the older-queue compatibility path. Needs the debug binaries built first. |
| `make cpp-lint-gate` | `clang-format --Werror` and the Doxygen doc-comment pass. |
| `make test-all` | All of the above, composed. This is the gate to run before a pull request. |
| `make cpp-lint` | `cpp-lint-gate` plus `clang-tidy`. Not part of `test-all`. |
| `make linux-gate` | The `debug` build, whole ctest suite and queue observer Python checks on Debian trixie in Docker (native arm64). Not part of `test-all`. See [The Linux gate](#the-linux-gate). |

## How the skill tree is tested

The `planar` skill (`skills/planar/`) and the agents (`agents/planar-*.md`) are
authored files that install unchanged, so there is no render step to test;
the checks are on the source.

- **Format.** `make surface-lint` applies the Agent Skills rules to every
  `skills/<name>/SKILL.md` (frontmatter, name, description length, allowed
  keys, body of at most 500 lines, links that resolve, references one level
  deep) and prints one line per rule that holds. Link checks verify file
  existence only.
- **Budget.** `skills/planar/SKILL.md` has a tighter limit than the format's:
  150 lines of body. A Catch2 case in the surface-lint test binary enforces it.
- **Commands.** `make cli-usage-check` runs `cli_usage_lint` over `agents/`,
  `skills/`, `docs/` and `CLAUDE.md`, so every command and flag a reference
  or agent names must exist in a binary's `schema` catalog; its catalog mode
  checks the `docs.examples` the binaries publish the same way.
- **Contract.** The eval contract tier (`make eval-contracts`) asserts, through
  `evals/orchestrator/cases/skill-planar.json`, that `SKILL.md` states each of
  the thirteen invariants, names the five binaries and cites the feedback
  contract.

The orchestrator lifecycle fixture's final repository test uses a detached
queue ticket in its isolated arena. The harness records the ticket and log
path, then takes one 20-second observation slice. Native `queue wait --json`
is preferred; an installed queue without `wait` uses bounded `queue status
--json` observations. Only recorded `ended`/`exited`/exit-code-zero proves
the fixture test passed. A slice that expires retains the ticket under
`fixture-test-ticket.json`; resume its observation and grading with
`python3 evals/orchestrator/harness.py --resume-queue-ticket <artifact-dir>`.
This command never resubmits the fixture test. The fixture arena has its own
`PLANAR_DB` and queue logs; the outer host queue still serializes the eval
gate itself.

`make eval-queue-observation-integration` is the focused check for a live
ticket. It submits one scratch-database command per observer mode, lets a
short observation slice expire while the command is still active, and then
observes the original ticket through completion. It is part of
`make test-all`, which builds the debug binaries first. The full Linux CI
tier runs both `make eval-orchestrator-unit` and this integration check in its
pinned toolchain container. `make linux-gate` runs the same two Python checks
after ctest and exports their logs with the gate result.

## Task and milestone cadence

Task cycles run focused acceptance and affected-behavior tests, together with
relevant static/type, formatting, build, artifact parity, and policy checks.
For methodology or workflow-surface edits, the focused checks are
`make cli-usage-check`, `make surface-lint` and `make eval-contracts`. Derive other task commands from
the changed subsystem's guidance; check that filtered tests actually match.

Run `make test-all` and any other required full regression or end-to-end checks
at the milestone barrier on the exact accumulated candidate after task fan-in,
including before a pull request. A standalone task's final delivery boundary
serves as its barrier. Task completion and fan-in use focused evidence and the
selected review disposition; they do not establish a full pass or final closeout.
A scheduled barrier gate is not missing task evidence. Failed task checks block
the task; a failed barrier blocks milestone promotion and final closeout.

After barrier failures, corrective tasks run focused checks. Rerun the full
milestone profile when re-entering the barrier with the final candidate, rather
than after each fix. Preserve every run's failures, exact commands, exit status,
logs, revision, and dirty/diff identity. Default repeat is 1; explicit stability
requirements remain binding, and retrying until green is not validation.
Review cadence remains independent, including deferred review at its boundary.

### The task profile in this repository

Every task runs these, in this order, and reports each exit status:

1. **Build everything, incrementally.**
   `cmake --build build/debug --target all planar_tests`. It catches a compile
   break in any module, including ones the task never touched, for the cost of
   an incremental build. Only the full test run moves to the milestone.
2. **Run the tests for each module the task touched.** The ctest label is the
   name in the directory's `planar_module(<name> …)` call, or `cmd_<binary>`
   under `src/cmd/<binary>/`. For example, `src/engine/hostqueue/` is
   `engine_hostqueue` and `src/lib/cliapp/` is `cliapp`:

   ```sh
   ctest --test-dir build/debug -L '^engine_hostqueue$' --output-on-failure
   ```

   Anchor the label with `^…$`, since `-L` is a regular expression. Report the
   matched count from ctest's summary line. A filter that matches nothing also
   exits 0, so zero matched tests is a failure, not a pass.
3. **Narrow `cmd_planar`.** That label holds about a quarter of the suite.
   A task that changes one verb family adds a name filter, because each test
   name starts `planar.cmd_planar.` followed by its Catch2 case name:

   ```sh
   ctest --test-dir build/debug -L '^cmd_planar$' -R 'task update' --output-on-failure
   ```

4. **Run the new or changed tests by name** if steps 2 and 3 did not already
   select them, and confirm they appear in the matched set.
5. **Run the cheap cross-cutting checks the change can break.** Most failures a
   milestone run used to find were stale pins, not logic errors.
   - C++ changed: `make cpp-lint-gate`.
   - Help text, flags or the catalog changed: `make surface-check`, and the
     whole-catalog pin,
     `ctest --test-dir build/debug -L '^cmd_planar$' -R 'catalog is pinned'`.
     A deliberate change recaptures the baseline with
     `scripts/surface-snapshot.sh capture` in its own commit.
   - Authored docs, the skill or an agent changed: `make surface-lint`,
     `make cli-usage-check` and `make eval-contracts`.
   - `install.sh` or its scripts changed: `make test-install-stage`. Its health
     scenarios need the debug `planar` from step 1, and they report SKIPPED
     without it.
   - A migration changed: `ctest --test-dir build/debug -L '^db$'`, which
     includes the up, down, up round trip.

### The milestone profile in this repository

Once per milestone, on the merged candidate, before the milestone closes and
before any pull request:

- `make test-all`: the whole ctest suite, the registry check that proves the
  whole suite ran, and the coverage, surface, exit-code, eval and lint gates in
  the table above.
- `make cpp-lint` when the milestone changed C++, for the advisory `clang-tidy`
  pass.

A milestone that changes platform-sensitive code also runs `make linux-gate`.
Both profiles go through the host queue, below.

## Continuous integration

CI is deliberately small, because agents merge often and a per-merge gate
would run constantly. `make test-all` through the host queue (below) is still
the gate to run before a pull request; CI is the independent backstop.

| Workflow | Runs | What |
|---|---|---|
| `ci.yml` (fast tier) | Pull requests into `master`, and pushes to `master` | `make fmt-check`, the installer fixtures, and the planning eval harness unit tests. It does not build the C++ tree. A newer push cancels the older run. |
| `full.yml` (full tier) | Nightly, on manual dispatch, on `v*` tags, and on a pull request labelled `ci:full` | The `debug` build, the whole ctest suite, the orchestrator eval harness unit tests and the scratch queue observer integration test, on Linux in the same pinned toolchain image as `make linux-gate`. Builds from cold. |

The fast tier does nothing for a change that touches only `agents/`, `skills/`,
`docs/`, `.github/ISSUE_TEMPLATE/`, `.github/PULL_REQUEST_TEMPLATE.md`, or a
top-level `*.md` or `LICENSE` file. The `checks` job still starts and reports
success with its steps skipped, so a required `checks` status is satisfied
(a workflow-level `paths:` filter would never start the job, and a required
check that never reports blocks the merge). Any other path counts as code, as
does a run that cannot be classified. Because those files feed pinned
projections and doc gates, the nightly full tier still checks them: run
`make test-all` before a pull request that edits `agents/` or `skills/`.

Pull requests into any other branch run neither workflow. Agents integrate
into `dev/integration` and open a pull request from it to `master` when a
batch is ready, so CI runs once per batch, not once per merge. Add the
`ci:full` label to a `master`-bound pull request that touches C++ or CMake and
should be built on Linux before it merges.

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
planar-agent queue run --detach --timeout 2h --vendor <vendor> --role <role> --claim <token> -- make test     # cli-lint-ignore: `--` is the argument terminator
planar-agent queue wait <seq> --timeout 3h --json
```

`make test-all`, `make cpp-lint` and `make linux-gate` run long enough to
reach the queue's default run limit of 30 minutes, at which the command is
stopped and its entry ends as `timeout`. Pass `--timeout <duration>` for a
gate that legitimately takes longer (for example `--timeout 2h`), and
`--claim <token>` when the caller holds a task claim, so the queue renews it
while the command waits and runs. Omit `--claim` only when there is no claim or
the claim is renewed separately. The observer's `--timeout` is independent:
allow for expected queue backlog plus runtime, as the three-hour wait above
does for an expected hour of backlog and a two-hour run limit. A submitted
`--wait-timeout` separately limits how long an entry may wait for its turn.

A detached gate builds when its turn comes, not when it is submitted. Submit
it only once the tree is final for that gate, and do not edit the tree until
its ticket has ended. Read `queue wait`'s `wait_reason` and authoritative
`status.outcome` together; a child can itself exit 124 or 125, and a wait
deadline or interruption is no gate verdict. An empty log or missing active
entry is no gate verdict either. A probe rebuild and a gate
write the same build directory, so submit both through the queue and never
run one beside the other.

For a short agent turn, `queue wait <seq> --timeout 10m --json` returns control
within a finite slice. If it reports `timed_out` or `interrupted`, do other
work or hand off the same sequence number for another finite wait. Neither
result stops the queued command, and neither calls for a second submission.
Do not hide an unbounded retry loop around finite slices. A `stalled`,
`history_unavailable` or `error` result needs inspection; a queue refusal
never authorizes a direct build. `queue wait` only reads state and never
renews a claim. See [the wait contract](cli-reference.md#waiting-for-a-logical-ticket-queue-wait)
and [operations](operations.md#5-the-host-build-and-test-queue).

The queue is for builds and tests that would otherwise run side by side. It
does not replace the gates: a queued `make test` is the same `make test`.
The test suite's own black-box cases never touch the real queue, because
`run_pinned` gives each one its own `PLANAR_DB`, and the queue lives in that
file.

`clang-tidy` is advisory. The recipe runs it without `--warnings-as-errors`
and `.clang-tidy` declares no `WarningsAsErrors` key, so its warnings never
fail a run. It enables one check, `readability-identifier-naming`.

## The Linux gate

`make linux-gate` builds the `debug` preset, runs the whole ctest suite and
the queue observer Python checks in
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
and SHA-256, ninja, git, python3, GNU `wget` (the `bootstrap.release` test), `sqlite3` and `libssl-dev` (vendored libcurl's TLS on Linux). The image
build never fails on a red suite. It records `configure.log`, `build.log`,
`ctest.log`, `eval-unit.log`, `eval-queue-observation.log` and `status.txt`,
and the Makefile exports them to
`build/linux-gate/`, prints the gate verdict and exits nonzero unless
`status=0`. Read `ctest.log` there rather than the build output: BuildKit
clips a step's log at 2 MiB.

Every dependency is committed under `vendor/`, so the gate needs no token, no
network fetch during configure, and no extra build context.

The build tree lives in a BuildKit cache mount, so a second run is
incremental, and it costs disk while it stays. Run `make linux-gate-prune`
when done; a past unpruned run of this kind filled the machine's disk.
Docker is a developer tool and not one of the installer's `BUILD_DEPS`.
The container builds as root for compatibility with its build cache, then runs
ctest as the unprivileged `planar-test` user. File-permission regressions must
exercise SQLite's access failures rather than root's permission bypass.

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

## Release bundles and installer script tests

`make dist` configures and builds the `dist` preset (`PLANAR_PORTABLE=ON`,
version metadata, a macOS 26.0 deployment target), runs the portable-binary
checks on the five staged binaries, and assembles
`dist/planar-<platform>.tar.gz` with `SHA256SUMS`, `VERSION`, a standalone
`get-planar.sh` and the archive's `.gates.json` evidence (`scripts/dist.sh`).
`make linux-dist` does the same for Linux x86_64 in Docker on Debian bookworm.
Both are release builds and not part of `make test` or `make test-all`; the
gates they feed are in [operations.md § 6](operations.md#6-release-gate-evidence)
and [toolchain-parity.md](toolchain-parity.md#portable-distribution-builds).
Run them through the host queue and never with a validation tag in the real
repository.

The scripts they ship and the installer have script tests. Each is a ctest case
registered with ctest (in `CMakeLists.txt`, `src/cmd/CMakeLists.txt` for the two
that need built binaries, `src/cmd/planar/CMakeLists.txt` and
`src/tools/CMakeLists.txt`), so `make test` runs them. The `make test` target
also depends on three installer tests that are not ctest cases:
`test-install-manifest`, `test-install-stage` and `test-install-deps`, which run
`scripts/install-manifest-test.sh`, `install-stage-test.sh` and
`install-deps-test.sh`. `install-manifest-test.sh` also pins INSTALL.md
§ Prerequisites. They use scratch `HOME`,
`TMPDIR` and database paths and fake bundles, and touch nothing real. Select one
by label, and check the matched count:

```sh
ctest --test-dir build/debug -L '^dist_layout$' --output-on-failure
```

| ctest case | Label | Entry point | Covers |
|------------|-------|-------------|--------|
| `dist.identity` | `dist_identity` | `scripts/dist-identity.test.py` | Assembler identity and schema refusals. See [Bundle assembler identity checks](#bundle-assembler-identity-checks). |
| `dist.layout` | `dist_layout` | `scripts/dist-test.sh` | The real assembler over fake binaries: entries, bytes, `release.json`, checksums. |
| `release.publish` | `release_publish` | `scripts/release-publish-test.sh` | `release-gates.sh`, `release-publish.sh` and `make release-cut` against a scratch annotated tag, fake bundles and a fake `gh`, `docker` and `uname`: every refusal runs no `gh`. |
| `release.workflow` | `release_workflow` | `scripts/release-workflow.test.py` | A lint of `.github/workflows/release.yml` (trigger, `needs`, gate-before-upload, permissions) and its publisher steps run against fakes. Nothing runs on GitHub. |
| `bootstrap.release` | `bootstrap` | `scripts/get-planar-test.sh` | `get-planar.sh` end to end. See [The release bootstrap test](#the-release-bootstrap-test). |
| `install.bash32` | `install_bash32` | `scripts/install-bash32-test.sh` | A lint for bash-4-only constructs and unguarded empty-array expansions, and a prebuilt install and uninstall under `/bin/bash`. |
| `install.prefix_guard` | `install_prefix_guard` | `scripts/install-prefix-guard-test.sh` | The install-root guard for `install.sh` and `--uninstall`. |
| `install.data_paths` | `install_data_paths` | `scripts/install-data-paths-test.sh` | Data paths are never removed, and INSTALL.md's preserved-paths list matches `scripts/install-lib/data-paths.sh`. |
| `install.prebuilt` | `install_prebuilt` | `scripts/install-prebuilt-test.sh` | `install.sh --prebuilt`: an incomplete bundle, `--link`, and the move-aside of the old queue database. |
| `install.retired_targets` | `install_retired_targets` | `scripts/install-retired-targets-test.sh` | The retired `make install` targets are gone, and no install doc carries a `make install` recipe. |
| `install.managed` | `install_managed` | `scripts/install-managed-test.sh` | Managed subtrees, retired paths, link and copy mode, and the missing-only `templates/` rule. |
| `install.lock` | `install_lock` | `scripts/install-lock-test.sh` | The mutation lock with real processes: killed and pid-reused owners, simultaneous reclaims, the update handoff. |
| `install.uninstall` | `install_uninstall` | `scripts/install-uninstall-test.sh` | `scripts/uninstall.sh`: removals, `--purge`, unknown entries, `~/.local/bin`, interrupted-uninstall retry. |
| `install.queue_probe` | `install_queue_probe` | `scripts/install-lib/queue_probe.test.py` | The installer's database probe with no `python3`, against the real built binaries. |
| `install.order` | `install_order` | `scripts/install-order-test.sh` | The order of an install end to end, against bundles of the real built binaries: databases fresh, behind, ahead and faulty, kill-and-resume at every swap point, concurrent owners, the updater handoff. |
| `queue_retire_reader` | `queue;scripts` | `scripts/install-lib/queue_retire.test.py` | The reader the installer runs to judge the retired queue store's entries, including its `/proc` cases. |
| `codex_agents_render` | `scripts` | `scripts/render_codex_agents_test.py` | The Codex custom-agent TOML renderer. |

`portable.binaries` and `portable.inspector` (label `portable`) exist only in a
configuration with `PLANAR_PORTABLE=ON`; see
[toolchain-parity.md](toolchain-parity.md#portable-distribution-builds).
`planar update` itself is covered by Catch2 cases under
`src/cmd/planar/handlers/update/` (label `cmd_planar`).

`scripts/release-workflow-crosscheck.py` is a manual check outside ctest. It
compares the workflow reader's result with PyYAML and runs `actionlint`, and it
fails when either tool is missing. Run it by hand after editing
`.github/workflows/release.yml`.

## Bundle assembler identity checks

The `dist.identity` ctest (label `dist_identity`) runs nine focused controlled
fixture tests for stable tag/HEAD/dirty identity, git-free snapshot inputs and Docker wrapper propagation,
metadata init/health/schema refusals, zero or failed portable checks, and schema
field type preservation and external scratch cwd/cleanup. The fixtures substitute external
build commands and binaries to reach refusal paths; they do not establish real
binary portability. Comprehensive layout/checksum coverage belongs to the
separate bundle layout test.

Fake-product assembly fixtures select Linux x86_64 through a scratch `uname`
command, independently of the host running ctest (including Linux arm64).
Layout expectations use that declared fixture target. This controls only tests
of assembly with Python-script executables; it establishes no native binary
portability or additional shipping platform. A separate refusal case gives the
actual assembler Linux aarch64 and requires rejection before configure with no
archive emitted. Production bundles still support only macOS arm64 and Linux
x86_64, and the Linux gate keeps its existing platform.

The `dist.layout` ctest (label `dist_layout`, entry point
`scripts/dist-test.sh`) runs five layout scenarios against the actual
`scripts/dist.sh` assembly path in disposable source copies. Configure, build,
portable inspection and install commands are controlled fixtures that stage
exactly five tiny fake executables. The real assembler copies the current
authored assets, renders Codex agents, collects metadata and emits the archive.
The test unpacks it and checks exact entries, executable and authored bytes,
one rendered file per authored agent, flat metadata with the current migration
maximum, sorted entries with fixed owners, and tagged repeat cuts. It verifies
the emitted checksum record with both `sha256sum -c` and `shasum -a 256 -c`
when available, and requires at least one checker.

The root `uninstall.sh` is always shipped, a byte copy of `scripts/uninstall.sh`;
assembly stops when that file is missing. The root `get-planar.sh` is likewise
always shipped, a byte copy of `scripts/get-planar.sh`, and the same bytes are
emitted as the standalone `dist/get-planar.sh` with their own `SHA256SUMS`
record (checked by the same two checkers); assembly stops when the source is
missing. Deliberate mutations prove
that wrong entry counts, installer, uninstaller and bootstrap bytes and metadata fail; appending a byte
to the archive must make the shell entry point's checksum assertion fail and
name the asset. To check a retained fixture manually, use
`scripts/dist-test.sh --check-checksums <directory> <asset-basename>` through
the host queue. All fixture tags, output and scratch trees are isolated from
the checkout's tags and retained `dist/` artifacts. Fake executables establish
layout and checksum behavior; the real native/Linux portability and smoke
evidence remains a separate gate.

Task validation also runs real `make dist` and `make linux-dist` through the host
queue. Assembly requires exactly two portable tests, then inspects the staged
products. Retain the archive and its checksum-bound `.gates.json`, plus
`build/dist-evidence/` init, health and portability logs. Tagged tests use a
disposable checkout; never create validation release tags in the real repository.
The milestone barrier runs the full suite on the merged candidate separately.

## The release bootstrap test

The `bootstrap.release` ctest (label `bootstrap`, entry point
`scripts/get-planar-test.sh`) runs `scripts/get-planar.sh` end to end against a
loopback release server, in a scratch `HOME` with its own `TMPDIR`; nothing in
the real `~/.planar` is read or written. About forty cases run in two to three
minutes under a 900 second timeout.

- **Server.** `python3 -m http.server 0 --bind 127.0.0.1` serves a release
  directory laid out as the GitHub release base is (`latest/download/VERSION`,
  `download/<tag>/{SHA256SUMS,planar-<platform>.tar.gz}`). It binds port 0 and
  the test reads the port back from the server's first line, so there is no
  port race, and an EXIT trap kills it. The server log is the evidence for
  where each asset came from. Bundles are the shared fake bundles published by
  `src/cmd/planar/handlers/update/release_fixture.sh`.
- **Test-only dependencies.** `python3` (the server) and GNU `wget` (the
  fallback case) are dependencies of this test, never of an operator path
  (decision 1333). The bootstrap itself runs with a PATH that holds no python3.
  A missing `python3` or `wget`, or a `wget` that is not GNU Wget, fails the
  test at the start with a message naming it; the test never skips, because
  the expected skip tally is zero.
- **Host shims.** `uname`, `sw_vers` and `ldd` are shimmed, so one host reaches
  the unsupported-platform, old-macOS and glibc refusals. Installs run on the
  host's own platform. A `curl` wrapper fires a hook at a chosen request (publish
  a newer release after `VERSION` is read, signal the bootstrap during the
  tarball download, rewrite the recovery journal after the download); curl is
  absent from `PATH` in the wget cases.
- **Per case.** Each asserts the exit status, the refusal's fixed opening text,
  that a refusal left `HOME` byte for byte as it was, and that no
  `planar-get.*` temporary directory remained. Each case prints `PASS <name>`
  or `FAIL <name>: <reason>`; the run ends with the counts and exits non-zero
  on any failure.
- **Fault injection.** The interrupted-install cases use the installer's
  `PLANAR_INSTALL_TEST_FAULT=kill@after-mutating` test fault, armed with
  `PLANAR_INSTALL_TEST_FAULT_ARMED=test-only`.

Run it through the host queue:

```sh
ctest --test-dir build/debug -L '^bootstrap$' --output-on-failure
```
