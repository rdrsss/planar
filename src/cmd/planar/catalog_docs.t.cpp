// @file catalog_docs.t.cpp
// @brief Every leaf verb of every binary documents its exit codes, and the
// `Examples:` and `Exit codes:` sections of `--help` read the same table as
// the `docs` object of the `schema` catalog (plan 1104, milestone 1105, task
// 7203).
//
// ## What this pins
//
//   - A leaf's `--help` ends with `Examples:` and `Exit codes:`, and both
//     sections repeat `docs.examples` and `docs.exitCodes` line for line, so
//     the two surfaces cannot drift.
//   - A group (`planar task`) has no sections and an empty `docs.examples`.
//   - Every leaf lists at least one exit code, always including `0`, and
//     every code is one its binary's exit table knows.
//   - Every `planar` and `planar-agent` leaf except `schema`, `completion`,
//     `version` and `help` carries at least one example that starts with its
//     own command path.
//
// ## Provenance
//
// The allowed code sets below come from each binary's `exit.cppm`
// (`planar`: 0, 1, 2, 3, 5, 6, 7, 8 and 64; the other three CLI11 binaries
// the same minus 8, plus `planar-agent queue`'s 124, 125, 126 and 127;
// `planar-execute`: 0, 1, 2 and 75 from `runflow.cpp`). Each list is a copy
// because D18 forbids this test target importing another binary's module.

#include <catch2/catch_test_macros.hpp>

import std;

#include "parity_harness.hpp"
#include <glaze/glaze.hpp>

namespace {

using ::planar::cmd::parity::make_arena;
using ::planar::cmd::parity::run_pinned;

struct binary_under_test {
  std::string           name;
  std::filesystem::path path;
  std::set<int>         codes;             // The codes this binary's exit table knows.
  bool                  has_footer = true; // False for planar-execute, whose help is a hand-written banner.
};

auto binaries() -> std::vector<binary_under_test> {
  return {{"planar", PLANAR_CPP_BIN, {0, 1, 2, 3, 5, 6, 7, 8, 64}, true},
          {"planar-agent", PLANAR_AGENT_CPP_BIN, {0, 1, 2, 3, 5, 6, 7, 64, 124, 125, 126, 127}, true},
          {"planar-watch", PLANAR_WATCH_CPP_BIN, {0, 1, 2, 3, 5, 6, 7, 64}, true},
          {"planar-ext", PLANAR_EXT_CPP_BIN, {0, 1, 2, 3, 5, 6, 7, 64}, true},
          {"planar-execute", PLANAR_EXECUTE_CPP_BIN, {0, 1, 2, 75}, false}};
}

// One command of a catalog, reduced to what these tests read.
struct command_docs_row {
  std::string                              command;
  bool                                     leaf = false;
  std::vector<std::string>                 examples;
  std::vector<std::pair<int, std::string>> exit_codes;
};

// Parse a `schema` document. Returns nullopt when the text is not a catalog.
auto parse_catalog(std::string const& text) -> std::optional<std::vector<command_docs_row>> {
  auto parsed = glz::read_json<glz::generic>(text);
  if (!parsed || !parsed->contains("commands")) {
    return std::nullopt;
  }
  std::vector<command_docs_row> rows;
  for (auto const& entry : parsed->at("commands").get<glz::generic::array_t>()) {
    command_docs_row row;
    row.command = entry.at("command").get<std::string>();
    row.leaf =
        entry.at("subcommands").get<glz::generic::array_t>().empty() && !entry.at("path").get<glz::generic::array_t>().empty();
    auto const& docs = entry.at("docs");
    for (auto const& e : docs.at("examples").get<glz::generic::array_t>()) {
      row.examples.push_back(e.get<std::string>());
    }
    for (auto const& e : docs.at("exitCodes").get<glz::generic::array_t>()) {
      row.exit_codes.emplace_back(static_cast<int>(e.at("code").get<double>()), e.at("meaning").get<std::string>());
    }
    rows.push_back(std::move(row));
  }
  return rows;
}

auto catalog_of(binary_under_test const& bin) -> std::vector<command_docs_row> {
  auto const arena = make_arena(std::format("catdocs_{}", bin.name));
  auto const ran   = run_pinned(bin.path, std::vector<std::string>{"schema"}, arena.cpp_root, "schema");
  INFO(bin.name << " schema stderr: " << ran.err);
  REQUIRE(ran.code == 0);
  auto rows = parse_catalog(ran.out);
  REQUIRE(rows.has_value());
  return *rows;
}

// The text `--help` must end with for a row, without the final newline.
auto expected_footer(command_docs_row const& row) -> std::string {
  std::string out;
  if (!row.examples.empty()) {
    out += "Examples:";
    for (auto const& e : row.examples) {
      out += std::format("\n  {}", e);
    }
  }
  if (!row.exit_codes.empty()) {
    out += out.empty() ? "" : "\n\n";
    out += "Exit codes:";
    for (auto const& [code, meaning] : row.exit_codes) {
      out += std::format("\n  {}  {}", code, meaning);
    }
  }
  return out;
}

auto help_of(binary_under_test const& bin, std::string const& command) -> ::planar::cmd::parity::capture {
  std::vector<std::string> args;
  std::istringstream       words{command};
  std::string              word;
  std::getline(words, word, ' '); // the binary name
  while (std::getline(words, word, ' ')) {
    args.push_back(word);
  }
  args.emplace_back("--help");
  auto const arena = make_arena(std::format("cathelp_{}", bin.name));
  return run_pinned(bin.path, args, arena.cpp_root, "help");
}

constexpr std::array<std::string_view, 4> k_no_example_required = {"schema", "completion", "version", "help"};

// Removed-verb stubs: declared so the old spelling gets a refusal (exit 2,
// "was removed") instead of a parse error. An example would advertise an
// invocation that cannot work, so none is required or written for them.
constexpr std::array<std::string_view, 3> k_removed_verb_stubs = {"planar scope use", "planar scope pop", "planar scope clear"};

auto example_required(binary_under_test const& bin, command_docs_row const& row) -> bool {
  if (bin.name != "planar" && bin.name != "planar-agent") {
    return false;
  }
  if (std::ranges::find(k_removed_verb_stubs, row.command) != k_removed_verb_stubs.end()) {
    return false;
  }
  auto const leaf_name = row.command.substr(row.command.rfind(' ') + 1);
  return std::ranges::find(k_no_example_required, leaf_name) == k_no_example_required.end();
}

} // namespace

TEST_CASE("planar task update --help ends with Examples and Exit codes drawn from the catalog", "[catalog][docs]") {
  auto const planar = binaries().front();
  auto const rows   = catalog_of(planar);
  auto const found  = std::ranges::find_if(rows, [](command_docs_row const& r) { return r.command == "planar task update"; });
  REQUIRE(found != rows.end());
  REQUIRE_FALSE(found->examples.empty());

  std::vector<int> codes;
  for (auto const& [code, meaning] : found->exit_codes) {
    codes.push_back(code);
    CHECK_FALSE(meaning.empty());
  }
  for (int const wanted : {0, 1, 2, 5}) {
    CHECK(std::ranges::find(codes, wanted) != codes.end());
  }

  auto const help = help_of(planar, "planar task update");
  INFO(help.err);
  REQUIRE(help.code == 0);
  auto const footer = expected_footer(*found);
  CAPTURE(help.out);
  CHECK(help.out.ends_with(footer + "\n"));
  CHECK(help.out.contains("\nExamples:\n  planar task update "));
  CHECK(help.out.contains("\n\nExit codes:\n  0  "));
}

TEST_CASE("a group has no help sections and an empty docs.examples", "[catalog][docs]") {
  auto const planar = binaries().front();
  auto const rows   = catalog_of(planar);
  auto const found  = std::ranges::find_if(rows, [](command_docs_row const& r) { return r.command == "planar task"; });
  REQUIRE(found != rows.end());
  CHECK_FALSE(found->leaf);
  CHECK(found->examples.empty());
  CHECK(found->exit_codes.empty());

  auto const help = help_of(planar, "planar task");
  REQUIRE(help.code == 0);
  CHECK_FALSE(help.out.contains("Examples:"));
  CHECK_FALSE(help.out.contains("Exit codes:"));
}

TEST_CASE("every leaf of every binary lists exit codes its exit table knows", "[catalog][docs]") {
  std::size_t leaves = 0;
  for (auto const& bin : binaries()) {
    auto const rows = catalog_of(bin);
    for (auto const& row : rows) {
      if (!row.leaf) {
        CHECK(row.exit_codes.empty());
        CHECK(row.examples.empty());
        continue;
      }
      ++leaves;
      CAPTURE(row.command);
      REQUIRE_FALSE(row.exit_codes.empty());
      std::set<int> seen;
      for (auto const& [code, meaning] : row.exit_codes) {
        CAPTURE(code);
        CHECK(bin.codes.contains(code));
        CHECK_FALSE(meaning.empty());
        CHECK(seen.insert(code).second);
      }
      // Every leaf can succeed, except the removed-verb stubs that always refuse.
      if (!row.command.starts_with("planar scope ") || row.command == "planar scope show" ||
          row.command == "planar scope suggest") {
        CHECK(seen.contains(0));
      }
    }
  }
  // 219 + 29 + 14 + 13 + 10 leaves measured 2026-10-04.
  CAPTURE(leaves);
  CHECK(leaves >= 280);
}

TEST_CASE("every planar and planar-agent leaf has an example under its own command path", "[catalog][docs]") {
  std::size_t checked = 0;
  for (auto const& bin : binaries()) {
    auto const rows = catalog_of(bin);
    for (auto const& row : rows) {
      for (auto const& example : row.examples) {
        CAPTURE(row.command, example);
        CHECK((example == row.command || example.starts_with(row.command + " ")));
      }
      if (!row.leaf || !example_required(bin, row)) {
        continue;
      }
      ++checked;
      CAPTURE(row.command);
      CHECK_FALSE(row.examples.empty());
    }
  }
  CAPTURE(checked);
  CHECK(checked >= 235);
}

TEST_CASE("--help repeats the catalog's docs for every leaf of the four CLI11 binaries", "[catalog][docs]") {
  std::size_t checked = 0;
  for (auto const& bin : binaries()) {
    if (!bin.has_footer) {
      continue;
    }
    auto const rows = catalog_of(bin);
    for (auto const& row : rows) {
      if (!row.leaf) {
        continue;
      }
      auto const help = help_of(bin, row.command);
      CAPTURE(row.command, help.out, help.err);
      REQUIRE(help.code == 0);
      CHECK(help.out.ends_with(expected_footer(row) + "\n"));
      ++checked;
    }
  }
  CAPTURE(checked);
  CHECK(checked >= 270);
}
