// @file root.t.cpp
// @brief Unit tests for `planar.engine.workbench.root` (plan 996, task 6037):
// the three-layer workbench-root precedence.
//
// FILESYSTEM SAFETY. This suite never reads the process environment and
// never touches a path it did not construct. `resolve_root` takes an
// explicit env-lookup callable AND an explicit file reader, so every case
// below hands it a std::map and an in-memory document. There is no code path
// from these tests to the operator's real `~/.planar/workbench/` -- not by
// convention, but because the module contains no `std::getenv` and, in the
// two-argument form, opens no file at all.
//
// ORACLE PROVENANCE. Each layer was probed by running `workbench list` under
// a scratch config and observing which directory the binary CREATED:
//
//   PLANAR_WORKBENCH_ROOT=/tmp/x        -> /tmp/x                 (layer 1)
//   config `[workbench]` + `root = "…"` -> that path              (layer 2)
//   config top-level `workbench.root =` -> that path              (layer 2)
//   config `root = "~/wbroot_tilde"`    -> $HOME/wbroot_tilde     (layer 2 + ~)
//   config `root = ""`                  -> $HOME/.planar/workbench (layer 3)

#include <catch2/catch_test_macros.hpp>

import std;
import planar.engine.workbench.root;

namespace {

namespace wr = planar::engine::workbench::root;

/// @brief An env lookup over an explicit map. An entry present but EMPTY
/// reads as an empty string, not as unset -- layer 1's "set and non-empty"
/// test depends on the two being distinguishable.
auto map_env(std::map<std::string, std::string, std::less<>> vars) -> wr::env_lookup {
  return [vars = std::move(vars)](std::string_view name) -> std::optional<std::string> {
    auto const it = vars.find(name);
    if (it == vars.end()) {
      return std::nullopt;
    }
    return it->second;
  };
}

/// @brief A file reader over an explicit map of path -> contents.
auto map_files(std::map<std::string, std::string, std::less<>> files) {
  return [files = std::move(files)](const std::filesystem::path& path) -> std::optional<std::string> {
    auto const it = files.find(path.string());
    if (it == files.end()) {
      return std::nullopt;
    }
    return it->second;
  };
}

auto no_files() {
  return map_files({});
}

} // namespace

// --- layer 1: the environment variable -----------------------------------

TEST_CASE("PLANAR_WORKBENCH_ROOT wins outright", "[workbench][root]") {
  auto const root = wr::resolve_root(
      map_env({{"PLANAR_WORKBENCH_ROOT", "/tmp/explicit"}, {"HOME", "/home/u"}, {"PLANAR_CONFIG_PATH", "/cfg.toml"}}),
      map_files({{"/cfg.toml", "[workbench]\nroot = \"/from/config\"\n"}}));
  REQUIRE(root.has_value());
  CHECK(*root == "/tmp/explicit");
}

TEST_CASE("an EMPTY PLANAR_WORKBENCH_ROOT falls through to the next layer", "[workbench][root]") {
  // Set-but-empty is not "set". This is why the lookup must distinguish an
  // absent variable from an empty one.
  auto const root = wr::resolve_root(map_env({{"PLANAR_WORKBENCH_ROOT", ""}, {"HOME", "/home/u"}}), no_files());
  REQUIRE(root.has_value());
  CHECK(*root == "/home/u/.planar/workbench");
}

TEST_CASE("a tilde in the env var expands against HOME", "[workbench][root]") {
  auto const rooted = wr::resolve_root(map_env({{"PLANAR_WORKBENCH_ROOT", "~/wb"}, {"HOME", "/home/u"}}), no_files());
  REQUIRE(rooted.has_value());
  CHECK(*rooted == "/home/u/wb");
  auto const bare = wr::resolve_root(map_env({{"PLANAR_WORKBENCH_ROOT", "~"}, {"HOME", "/home/u"}}), no_files());
  REQUIRE(bare.has_value());
  CHECK(*bare == "/home/u");
}

TEST_CASE("a tilde with no HOME is unresolved", "[workbench][root]") {
  auto const root = wr::resolve_root(map_env({{"PLANAR_WORKBENCH_ROOT", "~/wb"}}), no_files());
  REQUIRE_FALSE(root.has_value());
  CHECK(root.error() == wr::root_error::unresolved);
}

// --- layer 2: the config file --------------------------------------------

TEST_CASE("a `[workbench]` section's root is used", "[workbench][root]") {
  auto const root = wr::resolve_root(map_env({{"PLANAR_CONFIG_PATH", "/cfg.toml"}, {"HOME", "/home/u"}}),
                                     map_files({{"/cfg.toml", "[workbench]\nroot = \"/from/section\"\n"}}));
  REQUIRE(root.has_value());
  CHECK(*root == "/from/section");
}

TEST_CASE("a top-level dotted `workbench.root` is equivalent", "[workbench][root]") {
  // Oracle-probed: both spellings created the same directory.
  auto const root = wr::resolve_root(map_env({{"PLANAR_CONFIG_PATH", "/cfg.toml"}, {"HOME", "/home/u"}}),
                                     map_files({{"/cfg.toml", "workbench.root = \"/from/dotted\"\n"}}));
  REQUIRE(root.has_value());
  CHECK(*root == "/from/dotted");
}

TEST_CASE("a config root of `~/x` expands against HOME", "[workbench][root]") {
  auto const root = wr::resolve_root(map_env({{"PLANAR_CONFIG_PATH", "/cfg.toml"}, {"HOME", "/home/u"}}),
                                     map_files({{"/cfg.toml", "[workbench]\nroot = \"~/wbroot\"\n"}}));
  REQUIRE(root.has_value());
  CHECK(*root == "/home/u/wbroot");
}

TEST_CASE("an EMPTY config root falls through to the default", "[workbench][root]") {
  auto const root = wr::resolve_root(map_env({{"PLANAR_CONFIG_PATH", "/cfg.toml"}, {"HOME", "/home/u"}}),
                                     map_files({{"/cfg.toml", "[workbench]\nroot = \"\"\n"}}));
  REQUIRE(root.has_value());
  CHECK(*root == "/home/u/.planar/workbench");
}

TEST_CASE("the config path defaults to $HOME/.planar/config.toml", "[workbench][root]") {
  auto const root = wr::resolve_root(map_env({{"HOME", "/home/u"}}),
                                     map_files({{"/home/u/.planar/config.toml", "[workbench]\nroot = \"/x\"\n"}}));
  REQUIRE(root.has_value());
  CHECK(*root == "/x");
}

TEST_CASE("an absent, unreadable or unparseable config is SILENTLY skipped", "[workbench][root]") {
  // A broken config must not make the workbench unreachable -- it falls
  // through to the default rather than failing the verb.
  auto const missing = wr::resolve_root(map_env({{"PLANAR_CONFIG_PATH", "/nope.toml"}, {"HOME", "/home/u"}}), no_files());
  REQUIRE(missing.has_value());
  CHECK(*missing == "/home/u/.planar/workbench");

  auto const garbage = wr::resolve_root(map_env({{"PLANAR_CONFIG_PATH", "/cfg.toml"}, {"HOME", "/home/u"}}),
                                        map_files({{"/cfg.toml", "this is not toml at all {{{\n"}}));
  REQUIRE(garbage.has_value());
  CHECK(*garbage == "/home/u/.planar/workbench");
}

// --- layer 3, and the failure -------------------------------------------

TEST_CASE("with only HOME, the built-in default is used", "[workbench][root]") {
  auto const root = wr::resolve_root(map_env({{"HOME", "/home/u"}}), no_files());
  REQUIRE(root.has_value());
  CHECK(*root == "/home/u/.planar/workbench");
}

TEST_CASE("with NOTHING set the result is unresolved, never the cwd", "[workbench][root]") {
  // Explicitly NOT `.`. A resolver that fell back to the working directory
  // would make `workbench archive` delete a subtree of wherever the operator
  // happened to be standing.
  auto const root = wr::resolve_root(map_env({}), no_files());
  REQUIRE_FALSE(root.has_value());
  CHECK(root.error() == wr::root_error::unresolved);
}

// --- the narrow config scan ----------------------------------------------

TEST_CASE("read_config_workbench_root handles comments, quoting and sections", "[workbench][root][toml]") {
  CHECK(wr::read_config_workbench_root("# a comment\n[workbench]\nroot = \"/x\"\n") == "/x");
  CHECK(wr::read_config_workbench_root("[workbench]\nroot = '/x'\n") == "/x");
  CHECK(wr::read_config_workbench_root("[workbench]\n  root  =  \"/x\"   # trailing comment\n") == "/x");
  CHECK(wr::read_config_workbench_root("[\"workbench\"]\nroot = \"/x\"\n") == "/x");
  // A `#` INSIDE a quoted value is data, not a comment.
  CHECK(wr::read_config_workbench_root("[workbench]\nroot = \"/a#b\"\n") == "/a#b");
}

TEST_CASE("read_config_workbench_root ignores other sections and keys", "[workbench][root][toml]") {
  CHECK_FALSE(wr::read_config_workbench_root("[other]\nroot = \"/x\"\n").has_value());
  CHECK_FALSE(wr::read_config_workbench_root("[workbench]\nother = \"/x\"\n").has_value());
  CHECK_FALSE(wr::read_config_workbench_root("").has_value());
  CHECK(wr::read_config_workbench_root("[other]\nroot = \"/wrong\"\n[workbench]\nroot = \"/right\"\n") == "/right");
}

TEST_CASE("read_config_workbench_root rejects a non-string value", "[workbench][root][toml]") {
  // This scan accepts string values only; anything else falls through to the
  // next precedence layer exactly as an unreadable file would.
  CHECK_FALSE(wr::read_config_workbench_root("[workbench]\nroot = 42\n").has_value());
  CHECK_FALSE(wr::read_config_workbench_root("[workbench]\nroot = true\n").has_value());
}

TEST_CASE("expand_tilde leaves an absolute path alone", "[workbench][root]") {
  auto const absolute = wr::expand_tilde("/already/absolute", map_env({{"HOME", "/home/u"}}));
  REQUIRE(absolute.has_value());
  CHECK(*absolute == "/already/absolute");
  // A tilde that is not a path prefix is not expanded either.
  auto const embedded = wr::expand_tilde("/a/~/b", map_env({{"HOME", "/home/u"}}));
  REQUIRE(embedded.has_value());
  CHECK(*embedded == "/a/~/b");
}
