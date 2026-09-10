// @file arena_sweep_listener.cpp
// @brief A Catch2 event listener, linked into every test binary, that
// removes the `$TMPDIR/planar_*` scratch arenas a test run creates
// (plan 996, task 6311; concurrency fix task 6417).
//
// ## Why a listener instead of 134 per-fixture destructors
//
// Every `*.t.cpp` in the tree defines its own anonymous-namespace
// `fixture`/`make_fixture(tag)` (see e.g. src/cmd/planar/handlers.t.cpp)
// building a scratch root under `std::filesystem::temp_directory_path()`
// named `planar_<discriminator>_<counter>`. `src/cmd/parity_harness.hpp`'s
// `arena` is the ONE exception with its own RAII destructor (commit
// 3650c7e); that fix closed only the parity arenas (they held steady at
// 1155 afterward) while ~889 other fixture prefixes kept leaking, because
// nothing else has a destructor. Patching all of them is high-churn and a
// new fixture added tomorrow reintroduces the leak on day one.
//
// This listener instead cleans up by NAME CONVENTION, not by touching any
// fixture, and (see below) by PID-scoped ownership of an entire subtree so
// no fixture ever needs to know about PID-tagging either.
//
// ## Subprocess-created arenas ARE covered
//
// Several suites (parity tests, `dispatch.t.cpp`'s health probe, etc.)
// exec the built `planar`/`planar-agent`/`planar-watch` binaries, which
// create their own scratch directories rather than the test process doing
// it directly. Because ownership below is established by redirecting
// `TMPDIR` for THIS process before any fixture runs, and `TMPDIR` is part
// of the process environment inherited by every child (`fork`+`execv` in
// `planar::process::run_inherited`, and `posix_spawnp(..., environ)` in
// `planar::process::capture` both hand the current environment straight
// through — see src/lib/process/process.cpp), any subprocess that itself
// calls `std::filesystem::temp_directory_path()` (or reads `TMPDIR`
// directly, as `editor.cpp` does) resolves to the SAME PID-owned root this
// listener already owns. A directory-presence check does not care which
// process created an entry inside that root — it only asks "is this inside
// the subtree this listener carved out for its own PID" — so
// subprocess-created arenas are removed exactly like directly-created ones,
// with no snapshot diff required at all (see below).
//
// A test that deliberately overrides `TMPDIR` for one specific spawn (e.g.
// `drafting_leaves.t.cpp`'s fixture, which points a subprocess at its own
// `root/tmp`) is unaffected: that override wins for that one spawn just as
// it always did, and whatever it creates lives inside that fixture's own
// tree, which is itself nested under this run's PID root and swept as part
// of it.
//
// ## Parallelism safety: PID-scoped ownership, not snapshot-diff
//
// The original design (task 6311) achieved safety under `ctest -j N` (or
// under concurrent independent `ctest` invocations, which is this
// project's actual normal working mode — multiple orchestrated lanes each
// running the suite at once) via a snapshot-diff plus an advisory
// `flock()`-designated sweep owner: snapshot `$TMPDIR/planar_*` at
// testRunStarting, and at testRunEnded remove whatever is new. That
// protected the SWEEP (only one process ever removes anything at a time)
// but not the SNAPSHOT WINDOW: process A snapshots, process B starts and
// creates arenas, B exits, A's end-snapshot sees B's arenas as "new" and
// deletes them -- even while a still-running third sibling depends on them.
// The lock's own contention-skip made this look safe (a losing process
// writes a `.contended` marker and skips; the owner re-checks the marker at
// its end and also skips) but the two skip branches compose into "whenever
// two runs overlap, NOBODY sweeps" -- measured on this tree: `$TMPDIR`
// `planar_*` count went from 33 to 4,997 over about two hours of ordinary
// multi-lane use, and to 72,600 directories / 78G on a separate occasion,
// which manufactured phantom test failures across unrelated modules
// (task 6417).
//
// This version removes the race BY CONSTRUCTION instead of by lock
// discipline: at testRunStarting, before any fixture can create anything,
// this process creates `$TMPDIR/planar-arena-pid<PID>/` (the *current* `TMPDIR`,
// PID-suffixed) and then sets `TMPDIR` (via `setenv`, process-local -- it
// cannot race a sibling process's own environment) to that new path for the
// remainder of this process's life. Every subsequent call to
// `std::filesystem::temp_directory_path()`, in this process OR any
// subprocess it spawns, now resolves inside that PID-exclusive subtree.
// No other LIVE process can ever share this PID (the OS guarantees PID
// uniqueness among concurrently running processes), so nothing else can
// ever write into it. There is therefore:
//
//   - no contention to detect (nobody else can create an entry here),
//   - no lock to hold (ownership is the directory tree itself),
//   - no skip path (this process always sweeps its own tree), and
//   - no leak (concurrent runs simply sweep their own root, unconditionally,
//     every time -- not "when uncontended").
//
// Sweeping is then just "remove the whole PID root at testRunEnded",
// rather than a before/after set diff -- there is nothing else in that
// tree to distinguish from anything else, because nothing else can be.
//
// The root is deliberately named `planar-arena-pid<PID>`, NOT
// `planar_pid<PID>` -- it must NOT start with the literal `planar_` (with an
// underscore) that the OLD snapshot-diff sweep (and any test binary in the
// tree still running that version during a mixed rollout, e.g. a sibling
// worktree that has not yet rebuilt against this file) matches on. Measured
// while validating this fix: a concurrent OLD-listener process's
// end-of-run snapshot-diff swept away a live NEW-listener process's
// PID-owned root mid-run purely because it also started with `planar_`,
// reproducing the exact "No such file or directory" failure signature this
// task exists to eliminate -- just one version transition removed instead
// of one ctest process removed. Avoiding the shared prefix means an
// unmigrated OLD listener's snapshot-diff never matches this root at all,
// so it can coexist safely with this version during rollout. Once every
// process on the machine runs this listener, the two schemes can never
// collide again (this version deletes only its own exact PID root, nothing
// found by a name scan).
//
// A stale `planar-arena-pid<N>` directory can in principle exist from an earlier,
// long-dead process whose PID the OS later reused for this run (PIDs wrap
// and get reused once the prior holder has exited -- and if the prior
// holder is dead, reuse is exactly what makes it safe: no process is
// running under that PID right now, so nothing still depends on that
// leftover content). This listener does not special-case that: leftover
// content under `$TMPDIR/planar-arena-pid<PID>` when a fresh process is assigned
// that PID is, definitionally, orphaned from a run that has already ended,
// and is swept along with everything this run itself creates.
//
// ## The recurrence guard
//
// `PLANAR_KEEP_ARENAS` (the opt-out `parity_harness.hpp`'s destructor
// already honors) skips removal so a failing differential's arena can be
// inspected. Outside of that, if a directory this run created cannot
// actually be removed, this listener treats that as the exact silent
// regression the task asks to guard against and fails loudly: it prints
// the failure and exits the test binary with status 1, rather than letting
// the leak quietly resume accumulating disk over weeks the way it did
// three times before task 6311, and a fourth time (this task) after it.

#include <catch2/catch_test_macros.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#include <unistd.h>

import std;

namespace {

namespace fs = std::filesystem;

/// @brief True when `PLANAR_KEEP_ARENAS` opts out of arena removal.
/// @return Whether arenas should be retained instead of swept.
auto keep_arenas() -> bool {
  char const* keep = std::getenv("PLANAR_KEEP_ARENAS"); // NOLINT(concurrency-mt-unsafe)
  return keep != nullptr && *keep != '\0' && *keep != '0';
}

/// @brief Removes every `$TMPDIR/planar_*` entry this test binary's run
/// creates, by giving this process's PID exclusive ownership of an entire
/// `TMPDIR` subtree for the run's duration. See the file header for why
/// this removes the concurrency race by construction rather than by lock
/// discipline.
class arena_sweep_listener : public Catch::EventListenerBase {
public:
  using Catch::EventListenerBase::EventListenerBase;

  void testRunStarting(Catch::TestRunInfo const&) override {
    std::error_code ec;
    auto const      base = fs::temp_directory_path(ec);
    if (ec) {
      return; // no sane $TMPDIR -- nothing this listener can safely do
    }

    auto const pid_root = base / std::format("planar-arena-pid{}", ::getpid());
    fs::create_directories(pid_root, ec);
    if (ec) {
      std::cerr << "[arena-sweep] could not create the PID-owned sweep root "
                   "at "
                << pid_root << " -- this run's arenas will NOT be swept this time\n";
      return;
    }

    // Process-local: this cannot race a sibling process's environment, and
    // every subsequent temp_directory_path() call in THIS process (and any
    // subprocess it spawns, which inherits the environment -- see the file
    // header) now resolves inside pid_root, exclusively.
    if (::setenv("TMPDIR", pid_root.c_str(), 1) != 0) { // NOLINT(concurrency-mt-unsafe)
      std::cerr << "[arena-sweep] could not redirect TMPDIR to the PID-owned "
                   "sweep root -- this run's arenas will NOT be swept this "
                   "time\n";
      return;
    }

    // Confirm the redirection actually took (a defensive check, not a
    // load-bearing one -- setenv succeeding is expected to be sufficient).
    auto const confirm = fs::temp_directory_path(ec);
    if (ec || confirm != pid_root) {
      std::cerr << "[arena-sweep] TMPDIR redirection did not take effect -- "
                   "this run's arenas will NOT be swept this time\n";
      return;
    }

    pid_root_ = pid_root;
    owns_root_ = true;
  }

  void testRunEnded(Catch::TestRunStats const&) override {
    if (!owns_root_) {
      return;
    }
    if (keep_arenas()) {
      return;
    }

    std::error_code ec;
    fs::remove_all(pid_root_, ec);
    if (ec || fs::exists(pid_root_)) {
      std::cerr << "[arena-sweep] ARENA LEAK GUARD FAILED: could not remove "
                   "this run's PID-owned arena root at "
                << pid_root_
                << " -- failing this run loudly so the leak cannot silently "
                   "resume accumulating disk -- investigate before "
                   "re-running (task 6311, task 6417).\n";
      std::exit(1);
    }
  }

private:
  fs::path pid_root_;
  bool     owns_root_ = false;
};

CATCH_REGISTER_LISTENER(arena_sweep_listener)

} // namespace
