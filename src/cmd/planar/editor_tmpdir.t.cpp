// editor_tmpdir.t.cpp -- where `editor::invoke` puts its scratch file
// (task 6427).
//
// Plain `//`, not `///`: test TUs in this tree do not carry Doxygen `@file`
// blocks (see check.t.cpp, task 6737).
//
// The defect: the TMPDIR fallback was the LITERAL "/tmp". Every fixture built
// with `context::map_env` hits it, because that lookup is a CLOSED table --
// it returns nullopt for any key it was not given and never consults the real
// environment -- and none of the eight suites exercising the editor put
// "TMPDIR" in their map. So each of them wrote `planar-edit-XXXXXXXX` into
// the system /tmp, outside the fixture's scratch root and outside the
// listener's PID-scoped arena root.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.cmd.planar.context;
import planar.cmd.planar.editor;

namespace {

// A scratch directory removed when the guard goes out of scope.
struct scratch_dir {
  std::filesystem::path path_;

  scratch_dir()
      : path_(std::filesystem::temp_directory_path() /
              std::format("planar_editor_tmpdir_{}", std::chrono::steady_clock::now().time_since_epoch().count())) {
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

// A stub "editor" that records the path it was handed and leaves the file be.
// `invoke_opts::editor_override` exists for exactly this.
auto write_recording_editor(const std::filesystem::path& script, const std::filesystem::path& record) -> void {
  std::ofstream out(script);
  out << "#!/bin/sh\nprintf '%s' \"$1\" > '" << record.string() << "'\n";
  out.close();
  std::filesystem::permissions(script, std::filesystem::perms::owner_all, std::filesystem::perm_options::add);
}

} // namespace

TEST_CASE("editor::invoke writes under the REAL TMPDIR when the env lookup has none", "[cmd][editor][6427]") {
  scratch_dir scratch;
  auto const  script = scratch.path_ / "stub-editor";
  auto const  record = scratch.path_ / "seen-path";
  write_recording_editor(script, record);

  // Redirect the REAL environment, then hand `invoke` a synthetic lookup that
  // does NOT carry TMPDIR -- the exact shape every `map_env` fixture has.
  auto const        previous = std::getenv("TMPDIR");
  std::string const saved    = previous != nullptr ? previous : "";
  REQUIRE(::setenv("TMPDIR", scratch.path_.c_str(), 1) == 0);

  auto const env    = planar::cmd::map_env({{"HOME", scratch.path_.string()}});
  auto const result = planar::cmd::invoke(env, "seed body", planar::cmd::invoke_opts{.editor_override = script.string()});

  if (saved.empty()) {
    ::unsetenv("TMPDIR");
  } else {
    ::setenv("TMPDIR", saved.c_str(), 1);
  }

  REQUIRE(result.has_value());

  // The path the stub was handed is the discriminating evidence: before this
  // fix it began with "/tmp/planar-edit-", regardless of the redirect.
  std::ifstream     seen(record);
  std::string const handed{std::istreambuf_iterator<char>(seen), std::istreambuf_iterator<char>()};
  REQUIRE_FALSE(handed.empty());
  INFO("editor was handed: " << handed);
  CHECK(handed.starts_with(scratch.path_.string()));
  CHECK(handed.contains("planar-edit-"));

  // Non-vacuity: an env lookup that DOES carry TMPDIR still wins over the
  // fallback, so this change did not invert the precedence.
  scratch_dir explicit_dir;
  auto const  explicit_env = planar::cmd::map_env({{"HOME", scratch.path_.string()}, {"TMPDIR", explicit_dir.path_.string()}});
  auto const  explicit_result =
      planar::cmd::invoke(explicit_env, "seed body", planar::cmd::invoke_opts{.editor_override = script.string()});
  REQUIRE(explicit_result.has_value());

  std::ifstream     seen2(record);
  std::string const handed2{std::istreambuf_iterator<char>(seen2), std::istreambuf_iterator<char>()};
  INFO("editor was handed: " << handed2);
  CHECK(handed2.starts_with(explicit_dir.path_.string()));
}
