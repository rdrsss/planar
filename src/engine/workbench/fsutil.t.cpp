// @file fsutil.t.cpp
// @brief Unit tests for `planar.engine.workbench.fsutil` (plan 996, task
// 6037).
//
// FILESYSTEM SAFETY. Every path below is constructed under
// `std::filesystem::temp_directory_path()` with a clock-keyed name. Nothing
// here reads the environment or touches a path the test did not create.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.engine.workbench.fsutil;

namespace {

namespace wfs = planar::engine::workbench::fsutil;

/// @brief A scratch directory removed on scope exit.
struct scratch_dir {
  std::filesystem::path path_;

  scratch_dir()
      : path_(std::filesystem::temp_directory_path() / std::format("planar_wb_fsutil_{}_{}",
                                                                   std::chrono::steady_clock::now().time_since_epoch().count(),
                                                                   reinterpret_cast<std::uintptr_t>(this))) {
    std::error_code ec;
    std::filesystem::create_directories(path_, ec);
  }
  scratch_dir(const scratch_dir&)            = delete;
  scratch_dir& operator=(const scratch_dir&) = delete;
  ~scratch_dir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
};

} // namespace

TEST_CASE("read_file returns unset for an absent path and for a directory", "[workbench][fsutil]") {
  scratch_dir scratch;
  CHECK_FALSE(wfs::read_file(scratch.path_ / "nope.md").has_value());
  // A DIRECTORY must read as unset, not as an empty file: `sync` treats
  // "no content" as `deleted_on_fs` and would soft-delete the entity.
  CHECK_FALSE(wfs::read_file(scratch.path_).has_value());
}

TEST_CASE("write_file_atomic round-trips, creating parents", "[workbench][fsutil]") {
  scratch_dir scratch;
  auto const  target = scratch.path_ / "a" / "b" / "c.md";
  REQUIRE(wfs::write_file_atomic(target, "hello\n"));
  CHECK(wfs::read_file(target) == "hello\n");
  CHECK(wfs::path_exists(scratch.path_ / "a" / "b"));
}

TEST_CASE("write_file_atomic leaves no .tmp behind and truncates on rewrite", "[workbench][fsutil]") {
  scratch_dir scratch;
  auto const  target = scratch.path_ / "c.md";
  REQUIRE(wfs::write_file_atomic(target, "a longer first payload\n"));
  REQUIRE(wfs::write_file_atomic(target, "short\n"));
  CHECK(wfs::read_file(target) == "short\n");
  CHECK_FALSE(wfs::path_exists(std::filesystem::path{target.string() + ".tmp"}));
}

TEST_CASE("write_file_atomic writes an empty payload as an empty file", "[workbench][fsutil]") {
  // Not a no-op: `.sync` for a feature with no manifest rows IS zero bytes,
  // and the file must still exist.
  scratch_dir scratch;
  auto const  target = scratch.path_ / ".sync";
  REQUIRE(wfs::write_file_atomic(target, ""));
  CHECK(wfs::path_exists(target));
  CHECK(wfs::read_file(target) == "");
}

TEST_CASE("collect_markdown walks recursively and takes only .md", "[workbench][fsutil]") {
  scratch_dir scratch;
  REQUIRE(wfs::write_file_atomic(scratch.path_ / "a.md", "x"));
  REQUIRE(wfs::write_file_atomic(scratch.path_ / "notes.txt", "x"));
  REQUIRE(wfs::write_file_atomic(scratch.path_ / "tasks" / "cross" / "b.md", "x"));
  REQUIRE(wfs::write_file_atomic(scratch.path_ / ".sync", "x"));
  auto found = wfs::collect_markdown(scratch.path_);
  std::ranges::sort(found);
  REQUIRE(found.size() == 2);
  CHECK(found[0] == (scratch.path_ / "a.md").string());
  CHECK(found[1] == (scratch.path_ / "tasks" / "cross" / "b.md").string());
}

TEST_CASE("collect_markdown on an absent directory is EMPTY, not an error", "[workbench][fsutil]") {
  scratch_dir scratch;
  CHECK(wfs::collect_markdown(scratch.path_ / "nope").empty());
}

TEST_CASE("delete_tree removes a real directory", "[workbench][fsutil]") {
  scratch_dir scratch;
  auto const  tree = scratch.path_ / "feature";
  REQUIRE(wfs::write_file_atomic(tree / "tasks" / "a.md", "x"));
  CHECK(wfs::delete_tree(tree));
  CHECK_FALSE(wfs::path_exists(tree));
}

TEST_CASE("delete_tree REFUSES a symlink to a directory", "[workbench][fsutil][safety]") {
  // The load-bearing refusal. `workbench archive` calls this on a path built
  // from database-supplied slugs; following a symlink out of the workbench
  // root would delete an unrelated tree. `symlink_status`, not `status`.
  scratch_dir scratch;
  auto const  real_tree = scratch.path_ / "real";
  auto const  link      = scratch.path_ / "link";
  REQUIRE(wfs::write_file_atomic(real_tree / "a.md", "x"));
  std::error_code ec;
  std::filesystem::create_directory_symlink(real_tree, link, ec);
  if (ec) {
    SUCCEED("symlinks unavailable on this filesystem; refusal not exercised");
    return;
  }
  CHECK_FALSE(wfs::delete_tree(link));
  // Both the link AND its target survive.
  CHECK(wfs::path_exists(link));
  CHECK(wfs::read_file(real_tree / "a.md") == "x");
}

TEST_CASE("delete_tree refuses a regular file and an absent path", "[workbench][fsutil][safety]") {
  scratch_dir scratch;
  REQUIRE(wfs::write_file_atomic(scratch.path_ / "a.md", "x"));
  CHECK_FALSE(wfs::delete_tree(scratch.path_ / "a.md"));
  CHECK(wfs::path_exists(scratch.path_ / "a.md"));
  CHECK_FALSE(wfs::delete_tree(scratch.path_ / "nope"));
}

TEST_CASE("remove_file removes a file and is idempotent", "[workbench][fsutil]") {
  scratch_dir scratch;
  auto const  target = scratch.path_ / "a.md";
  REQUIRE(wfs::write_file_atomic(target, "x"));
  CHECK(wfs::remove_file(target));
  CHECK_FALSE(wfs::path_exists(target));
  // Removing an already-absent file is not an error: `gc` and the push
  // cleanup both rely on that.
  CHECK(wfs::remove_file(target));
}

TEST_CASE("make_path_all is idempotent and reports success on an existing directory", "[workbench][fsutil]") {
  scratch_dir scratch;
  auto const  nested = scratch.path_ / "a" / "b";
  CHECK(wfs::make_path_all(nested));
  CHECK(wfs::make_path_all(nested));
  CHECK(wfs::path_exists(nested));
}

TEST_CASE("make_path_all reports FAILURE when a path component is a regular file", "[workbench][fsutil]") {
  // `create_directories` cannot descend through a regular file, so the
  // recursive mkdir must fail here. Closes a break-probe SURVIVOR (task
  // 6423): a mutant that hardcoded `return true` after the
  // `create_directories` call passed every other fixture, because every
  // existing case only ever asked for a path that COULD be created. Only a
  // real failure path can tell "created" apart from "claimed success".
  scratch_dir scratch;
  auto const  blocker = scratch.path_ / "blocker";
  {
    std::ofstream file(blocker, std::ios::binary);
    REQUIRE(file);
  }
  CHECK_FALSE(wfs::make_path_all(blocker / "nested"));
  CHECK_FALSE(wfs::path_exists(blocker / "nested"));
}

TEST_CASE("collect_markdown skips a DIRECTORY whose name ends in .md", "[workbench][fsutil]") {
  // The recursive walk filters on two independent things: the `.md`
  // extension and `is_regular_file`. The existing case's only
  // subdirectories are `tasks/` and `tasks/cross/`, neither of which ends
  // in `.md`, so the extension filter rejects them first and the
  // regular-file check never has to do any work. Closes a break-probe
  // SURVIVOR (task 6781): dropping it collected the directory itself,
  // which the caller then tries to read as a file.
  scratch_dir scratch;
  REQUIRE(wfs::write_file_atomic(scratch.path_ / "real.md", "x"));
  REQUIRE(wfs::make_path_all(scratch.path_ / "archive.md"));
  REQUIRE(wfs::write_file_atomic(scratch.path_ / "archive.md" / "inner.md", "x"));

  auto found = wfs::collect_markdown(scratch.path_);
  std::ranges::sort(found);
  // The nested file counts; the `.md`-suffixed DIRECTORY holding it does not.
  REQUIRE(found.size() == 2);
  CHECK(found[0] == (scratch.path_ / "archive.md" / "inner.md").string());
  CHECK(found[1] == (scratch.path_ / "real.md").string());
}
