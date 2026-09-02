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
// Instead: an exclusive lock file (`O_CREAT|O_EXCL`) under `$TMPDIR`
// designates ONE "sweep owner" process at a time. A test binary that loses
// the race writes a contention marker and skips sweeping entirely for
// itself — it leaves its own arenas for a later run to clean up, never
// removing anything. The owner checks that marker again at the end of its
// own run; if ANY other process started while it was running, the owner
// ALSO skips its sweep for that run, because directories that appeared
// during its window can no longer be safely attributed to itself alone.
// The failure mode under detected concurrency is therefore always "sweep
// less than usual, once", never "delete a live sibling's arena". Serial
// ctest (today's reality) is unaffected: exactly one process ever holds
// the lock, contention is never recorded, and the sweep runs every time.
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

    int const fd = ::open(lock_path_.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
    if (fd < 0) {
      // Another test binary already owns the sweep for this window.
      // Record contention for it, and take no further part ourselves.
      std::ofstream marker(contended_path_, std::ios::app);
      std::cerr << "[arena-sweep] a concurrent test run already owns the "
                   "sweep -- this run's arenas will NOT be swept this "
                   "time (ctest runs serially by default in this tree; "
                   "set CTEST_PARALLEL_LEVEL=1 to keep the sweep active "
                   "under parallel runs)\n";
      return;
    }
    ::close(fd);
    owns_lock_ = true;
    std::error_code rm_ec;
    fs::remove(contended_path_, rm_ec); // clear a stale marker from a crashed prior owner
    before_ = snapshot_planar_entries(tmp_root_);
  }

  void testRunEnded(Catch::TestRunStats const&) override {
    if (!owns_lock_) {
      return;
    }
    std::error_code ec;
    fs::remove(lock_path_, ec);

    bool const contended = fs::exists(contended_path_);
    fs::remove(contended_path_, ec);
    if (contended) {
      std::cerr << "[arena-sweep] a concurrent test run overlapped this "
                   "one -- skipping the sweep so a still-live sibling "
                   "arena is never removed\n";
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
      std::exit(1);
    }
  }

private:
  fs::path               tmp_root_;
  fs::path               lock_path_;
  fs::path               contended_path_;
  bool                   owns_lock_ = false;
  std::set<std::string>  before_;
};

CATCH_REGISTER_LISTENER(arena_sweep_listener)

} // namespace
