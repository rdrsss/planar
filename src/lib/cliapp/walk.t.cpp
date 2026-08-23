// @file walk.t.cpp
// @brief Tests for `planar.cliapp.walk` — the `CLI::App` tree-walk
// primitives the schema emitter and the completion generator share
// (plan 996, task 6123).
//
// Small module, three things worth defending:
//
//   1. PRUNING, not filtering. A visible child of a HIDDEN parent is not
//      reachable from argv, so it must not appear in a catalog or a
//      completion script. The deleted `planar.cli.schema` shipped the
//      filtering bug (B3 of the M2 boundary review): it flattened the whole
//      tree with `all_nodes` and then dropped hidden nodes per-node, which
//      let a visible grandchild through. This walk prunes at the parent.
//
//   2. INHERITED-FLAG ORDER. Root-first, immediate-parent-last, EXCLUDING
//      the target's own flags. The schema emitter labels these
//      `"inherited"` and the target's own `"local"`; getting the boundary
//      wrong mislabels every flag on every nested command and is invisible
//      in help.
//
//   3. `--help` IS NOT PART OF THE SURFACE. CLI11 adds it to every node;
//      no Planar catalog has ever listed it, and `cli_usage_lint` carries
//      it in its own `global_ok_flags` for exactly that reason.

#include <catch2/catch_test_macros.hpp>

import std;
import cli11;
import planar.cliapp.walk;

namespace {

using planar::cliapp::all_nodes;
using planar::cliapp::canonical_name;
using planar::cliapp::children;
using planar::cliapp::find_node;
using planar::cliapp::inherited_flags;
using planar::cliapp::local_flags;
using planar::cliapp::local_positionals;

/// @brief Names of every option in `options`, in order.
/// @param options The options to name.
/// @return The canonical names.
auto names_of(std::vector<const CLI::Option*> const& options) -> std::vector<std::string> {
  std::vector<std::string> out;
  for (const CLI::Option* opt : options) {
    out.push_back(canonical_name(*opt));
  }
  return out;
}

/// @brief A three-level tree with flags at every level and a hidden branch.
/// @param app The root app to populate.
auto build_tree(CLI::App& app) -> void {
  app.require_subcommand(0);
  app.add_flag("--root-flag");

  CLI::App* mid = app.add_subcommand("mid", "Middle");
  mid->require_subcommand(0);
  mid->add_flag("--mid-flag");

  CLI::App* leaf = mid->add_subcommand("leaf", "Leaf");
  leaf->add_flag("--leaf-flag");
  leaf->add_option("target")->required();
  leaf->add_option("extra");

  CLI::App* hidden = app.add_subcommand("hidden", "Hidden branch");
  hidden->group("");
  hidden->require_subcommand(0);
  hidden->add_subcommand("visible-child", "Looks ordinary");
}

} // namespace

TEST_CASE("all_nodes walks depth-first and excludes the root", "[cliapp][walk]") {
  CLI::App app{"", "tool"};
  build_tree(app);
  std::vector<std::string> paths;
  for (auto const& node : all_nodes(app)) {
    paths.push_back(std::format("{}", node.path));
  }
  CHECK(paths == std::vector<std::string>{R"(["mid"])", R"(["mid", "leaf"])"});
}

TEST_CASE("a hidden subtree is PRUNED, not merely skipped", "[cliapp][walk][visibility]") {
  // ITEM 1. `visible-child` is itself visible; its parent is not. A
  // per-node filter would let it through — that was a real bug in the
  // emitter this module replaces.
  CLI::App app{"", "tool"};
  build_tree(app);
  for (auto const& node : all_nodes(app)) {
    CHECK(node.node->get_name() != "hidden");
    CHECK(node.node->get_name() != "visible-child");
  }
  CHECK(children(app).size() == 1);
  CHECK(children(app).front()->get_name() == "mid");
}

TEST_CASE("local_flags excludes positionals and CLI11's --help", "[cliapp][walk]") {
  // ITEM 3.
  CLI::App app{"", "tool"};
  build_tree(app);
  const CLI::App* leaf = find_node(app, std::array<std::string, 2>{"mid", "leaf"});
  REQUIRE(leaf != nullptr);
  CHECK(names_of(local_flags(*leaf)) == std::vector<std::string>{"--leaf-flag"});
  CHECK(names_of(local_positionals(*leaf)) == std::vector<std::string>{"target", "extra"});
}

TEST_CASE("inherited_flags is root-first and excludes the target's own", "[cliapp][walk][inherited]") {
  // ITEM 2. Both halves matter: the ORDER (root before immediate parent)
  // and the EXCLUSION (`--leaf-flag` is local, not inherited).
  CLI::App app{"", "tool"};
  build_tree(app);
  auto const leaf_path = std::array<std::string, 2>{"mid", "leaf"};
  CHECK(names_of(inherited_flags(app, leaf_path)) == std::vector<std::string>{"--root-flag", "--mid-flag"});

  auto const mid_path = std::array<std::string, 1>{"mid"};
  CHECK(names_of(inherited_flags(app, mid_path)) == std::vector<std::string>{"--root-flag"});

  // The root itself inherits nothing.
  CHECK(inherited_flags(app, std::span<std::string const>{}).empty());
}

TEST_CASE("find_node resolves a path and refuses one that does not", "[cliapp][walk]") {
  CLI::App app{"", "tool"};
  build_tree(app);
  CHECK(find_node(app, std::span<std::string const>{}) == &app);
  CHECK(find_node(app, std::array<std::string, 1>{"mid"})->get_name() == "mid");
  CHECK(find_node(app, std::array<std::string, 2>{"mid", "leaf"})->get_name() == "leaf");
  CHECK(find_node(app, std::array<std::string, 1>{"nosuch"}) == nullptr);
  CHECK(find_node(app, std::array<std::string, 2>{"mid", "nosuch"}) == nullptr);
  // A hidden node is not resolvable either — `children` is the only way
  // down and it filters.
  CHECK(find_node(app, std::array<std::string, 1>{"hidden"}) == nullptr);
}

TEST_CASE("canonical_name prefers the long form and falls back to the short", "[cliapp][walk]") {
  CLI::App app{"", "tool"};
  auto*    both  = app.add_flag("-v,--verbose");
  auto*    only  = app.add_flag("-q");
  auto*    pos   = app.add_option("target");
  auto*    alias = app.add_flag("--primary,--secondary");
  CHECK(canonical_name(*both) == "--verbose");
  CHECK(canonical_name(*only) == "-q");
  CHECK(canonical_name(*pos) == "target");
  // The FIRST long name is canonical; later ones are aliases, which is the
  // split `planar.cliapp.schema` reports as `"long"` vs `"aliases"`.
  CHECK(canonical_name(*alias) == "--primary");
}
