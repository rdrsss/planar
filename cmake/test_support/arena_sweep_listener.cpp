// @file arena_sweep_listener.cpp
// @brief A Catch2 event listener, linked into every test binary, that
// removes the `$TMPDIR/planar_*` scratch arenas a test run creates
// (plan 996, task 6311).
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
// fixture: it snapshots the `planar_*` entries directly under `$TMPDIR`
// before a test binary's run and removes whatever new ones exist after.
// Any future fixture that follows the existing `planar_*` naming
// convention is covered automatically, with zero code in the fixture
// itself.
//
// ## Subprocess-created arenas ARE covered
//
// Several suites (parity tests, `dispatch.t.cpp`'s health probe, etc.)
// exec the built `planar`/`planar-agent`/`planar-watch` binaries, which
// create their own scratch directories rather than the test process doing
// it directly. A per-fixture RAII destructor in the TEST process would
// never see those. A directory-presence diff does not care which process
// created an entry — it only asks "did a `planar_*` name appear under
// `$TMPDIR` between this run's start and its end" — so subprocess-created
// arenas are removed exactly like directly-created ones.
//
// ## Parallelism safety
//
// `CTEST_PARALLEL_LEVEL` is unset in this tree today, so ctest runs
// serially and a plain before/after directory diff is unambiguous: nothing
// else is creating `planar_*` entries while one test binary runs.
//
// If parallelism is ever turned on, that stops being true: two test
// binaries could run concurrently, and a naive diff in binary A could
// delete a directory binary B created (and is actively using) between A's
// snapshot and A's sweep. Tagging every one of ~889 fixture-name prefixes
// with a PID would make ownership unambiguous, but that reintroduces
// exactly the per-fixture churn this listener exists to avoid, for a
// contingency this tree does not currently exercise.
//
// Instead: an ADVISORY `flock()` (`LOCK_EX | LOCK_NB`) on a lock file under
// `$TMPDIR` designates ONE "sweep owner" process at a time. A test binary
// that loses the race writes a contention marker and skips sweeping
// entirely for itself — it leaves its own arenas for a later run to clean
// up, never removing anything. The owner checks that marker again at the
// end of its own run; if ANY other process started while it was running,
// the owner ALSO skips its sweep for that run, because directories that
// appeared during its window can no longer be safely attributed to itself
// alone. The failure mode under detected concurrency is therefore always
// "sweep less than usual, once", never "delete a live sibling's arena".
// Serial ctest (today's reality) is unaffected: exactly one process ever
// holds the lock, contention is never recorded, and the sweep runs every
// time.
//
// `flock()` rather than the earlier `O_CREAT|O_EXCL` sentinel-file scheme
// (task 6311 iteration 2, a real bug an independent verification run
// reproduced by accident): a killed/OOM'd/Ctrl-C'd process leaves an
// `O_CREAT|O_EXCL` sentinel FILE behind forever — nothing ever deletes it
// once the owning process is gone, so every subsequent run treats it as
// "another run currently owns the sweep", writes its own contention
// marker, and skips. The next run does the same. The sweep is disabled
// PERMANENTLY, silently, with the very recurrence guard this task exists
// to add never tripping, until an operator manually deletes the file —
// exactly the failure mode the acceptance criteria call out. `ctest`
// spawns one process PER TEST CASE (not once per binary), so a single
// killed run poisons every subsequent test-binary invocation, in the same
// run and every run after.
//
// An advisory `flock()` cannot orphan this way BY CONSTRUCTION: a lock
// held via a file descriptor is released by the kernel the instant the
// holding process's last reference to that descriptor goes away, for ANY
// reason a process ends — normal exit, `exit()`/`_exit()`, an uncaught
// signal including `SIGKILL`, or an OOM kill. There is no window in which
// the lock outlives the process that held it, so no age or PID heuristic
// is needed and no case exists where a human has to intervene to unstick
// it. The lock FILE itself is left in place (never unlinked) precisely so
// there is nothing to race: the next process opens the same path with
// `O_CREAT` (creating it if genuinely absent) and attempts its own
// `flock()`, which succeeds the instant the previous holder's lock is
// gone — whether that holder exited cleanly or was killed.
//
// ## The recurrence guard
//
// `PLANAR_KEEP_ARENAS` (the opt-out `parity_harness.hpp`'s destructor
// already honors) skips removal so a failing differential's arena can be
// inspected. Outside of that, if a directory this run created cannot
// actually be removed, this listener treats that as the exact silent
// regression the task asks to guard against and fails loudly: it prints
// every arena it could not remove and exits the test binary with status 1,
// rather than letting the leak quietly resume accumulating disk over
// weeks the way it did three times before this task.

#include <catch2/catch_test_macros.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

import std;

namespace {

namespace fs = std::filesystem;

/// @brief True when `PLANAR_KEEP_ARENAS` opts out of arena removal.
/// @return Whether arenas should be retained instead of swept.
auto keep_arenas() -> bool {
  char const* keep = std::getenv("PLANAR_KEEP_ARENAS");
  return keep != nullptr && *keep != '\0' && *keep != '0';
}

/// @brief Every `planar_*` entry directly under `$TMPDIR` right now.
/// @param tmp The temp root to scan.
/// @return The set of matching entry names (not full paths).
auto snapshot_planar_entries(const fs::path& tmp) -> std::set<std::string> {
  std::set<std::string> names;
  std::error_code       ec;
  for (auto const& entry : fs::directory_iterator(tmp, fs::directory_options::skip_permission_denied, ec)) {
    if (ec) {
      break;
    }
    auto const name = entry.path().filename().string();
    if (name.starts_with("planar_")) {
      names.insert(name);
    }
  }
  return names;
}

/// @brief Removes every `$TMPDIR/planar_*` entry this test binary's run
/// creates. See the file header for the parallelism-safety and
/// recurrence-guard design.
class arena_sweep_listener : public Catch::EventListenerBase {
public:
  using Catch::EventListenerBase::EventListenerBase;

  void testRunStarting(Catch::TestRunInfo const&) override {
    std::error_code ec;
    auto const      tmp = fs::temp_directory_path(ec);
    if (ec) {
      return; // no sane $TMPDIR -- nothing this listener can safely do
    }
    tmp_root_       = tmp;
    lock_path_      = tmp / ".planar_arena_sweep.lock";
    contended_path_ = tmp / ".planar_arena_sweep.contended";

    // O_EXCL is NOT used here (see the file header, task 6311 iteration
    // 2): the file is a target for flock(), not itself the ownership
    // signal, so it is fine -- expected, even -- for it to already exist
    // from a prior run. Ownership is decided entirely by whether THIS
    // process wins the advisory lock below.
    int const fd = ::open(lock_path_.c_str(), O_CREAT | O_RDWR, 0600);
    if (fd < 0) {
      // Could not even open the lock file (e.g. an unwritable $TMPDIR).
      // Fail safe exactly like losing the lock race: do not sweep.
      std::cerr << "[arena-sweep] could not open the sweep lock file at "
                << lock_path_ << " -- this run's arenas will NOT be swept "
                   "this time\n";
      return;
    }
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
      // Another test binary currently holds the lock -- it is genuinely
      // alive right now (the kernel releases a flock the instant its
      // holder's process ends, by any means), so this is real contention,
      // never a stale leftover. Record it for the owner, and take no
      // further part ourselves.
      ::close(fd);
      std::ofstream marker(contended_path_, std::ios::app);
      std::cerr << "[arena-sweep] a concurrent test run already owns the "
                   "sweep -- this run's arenas will NOT be swept this "
                   "time (ctest runs serially by default in this tree; "
                   "set CTEST_PARALLEL_LEVEL=1 to keep the sweep active "
                   "under parallel runs)\n";
      return;
    }
    lock_fd_   = fd;
    owns_lock_ = true;
    std::error_code rm_ec;
    fs::remove(contended_path_, rm_ec); // clear a marker a losing sibling left for us
    before_ = snapshot_planar_entries(tmp_root_);
  }

  void testRunEnded(Catch::TestRunStats const&) override {
    if (!owns_lock_) {
      return;
    }
    std::error_code ec;

    bool const contended = fs::exists(contended_path_);
    fs::remove(contended_path_, ec);
    if (contended) {
      std::cerr << "[arena-sweep] a concurrent test run overlapped this "
                   "one -- skipping the sweep so a still-live sibling "
                   "arena is never removed\n";
      release_lock();
      return;
    }

    auto const                after = snapshot_planar_entries(tmp_root_);
    bool const                keep  = keep_arenas();
    std::vector<std::string>  failed;
    std::size_t                swept = 0;
    for (auto const& name : after) {
      if (before_.contains(name)) {
        continue; // pre-existing before this run started -- not ours to touch
      }
      if (keep) {
        continue;
      }
      std::error_code rm_ec;
      fs::remove_all(tmp_root_ / name, rm_ec);
      if (rm_ec || fs::exists(tmp_root_ / name)) {
        failed.push_back(name);
      } else {
        ++swept;
      }
    }
    static_cast<void>(swept);

    if (!failed.empty()) {
      std::cerr << "[arena-sweep] ARENA LEAK GUARD FAILED: " << failed.size()
                << " arena(s) created by this run could not be removed:\n";
      for (auto const& name : failed) {
        std::cerr << "  " << name << "\n";
      }
      std::cerr << "[arena-sweep] failing this run loudly so the leak "
                   "cannot silently resume accumulating disk -- "
                   "investigate before re-running (task 6311).\n";
      release_lock();
      std::exit(1);
    }
    release_lock();
  }

private:
  /// @brief Release the advisory lock and close its descriptor. The lock
  /// FILE itself is deliberately left in place -- see the file header for
  /// why leaving it is what makes staleness impossible rather than merely
  /// unlikely.
  void release_lock() {
    if (lock_fd_ >= 0) {
      ::flock(lock_fd_, LOCK_UN);
      ::close(lock_fd_);
      lock_fd_ = -1;
    }
  }

  fs::path               tmp_root_;
  fs::path               lock_path_;
  fs::path               contended_path_;
  int                    lock_fd_   = -1;
  bool                   owns_lock_ = false;
  std::set<std::string>  before_;
};

CATCH_REGISTER_LISTENER(arena_sweep_listener)

} // namespace
