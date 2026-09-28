// @file catalog.t.cpp
// @brief Unit tests for `planar.engine.workflows.catalog` (plan 996, task
// 6096).
//
// HOME SAFETY. This suite never reads the process environment and never
// resolves a path it did not construct. `catalog::resolve_dirs` takes an
// explicit lookup callable, so every test below hands it a std::map over a
// per-test scratch directory under std::filesystem::temp_directory_path().
// There is no code path from these tests to the developer's real
// `~/.planar` -- not by convention, but because the module has no getenv in
// it. `resolve_dirs` IS exercised against a synthetic environment (see the
// resolution cases) so the fallback chain is covered without touching a real
// HOME.
//
// ORACLE PROVENANCE. Captured by RUNNING the Zig binary with PLANAR_HOME
// pointed at a scratch tree, never from `--help` and never from the Zig
// source (whose own header comment about the @meta block is WRONG in two
// places -- see below):
//
//   export PLANAR_HOME=/tmp/op/wfhome PLANAR_DB=/tmp/op/wf.db
//   mkdir -p $PLANAR_HOME/workflows $PLANAR_HOME/local/workflows
//   # shipped: finalize_closeout.lua (full @meta, name finalize-closeout)
//   #          bare.lua              (no @meta at all)
//   #          partial.lua           (@meta, name aaa-partial, no phases/seam)
//   #          notes.txt             (NOT .lua -- must be ignored)
//   # sandbox: sandbox_one.lua       (@meta, name mmm-sandbox)
//
//   $Z workflow list
//     name                      kind      phases                description
//     aaa-partial               shipped                         Only two fields.
//     bare                      shipped
//     finalize-closeout         shipped   closeout              Deterministic closeout gate.
//     mmm-sandbox               local     build,test            A sandbox workflow.
//
// --- @meta parsing: the Zig header comment is WRONG, the code is the spec --
// It claims the block "MUST start on line 1 ... (no leading whitespace)".
// Neither half holds. Each of these was written to a file and listed:
//
//   two leading BLANK lines before `--[[ @meta`  -> name leading-blanks (found)
//   three spaces of indent before `--[[ @meta`   -> name indented-open  (found)
//   CRLF line endings throughout                -> name crlf-name      (found)
//   block never closed by `--]]`                -> name no-close        (found)
//   `name:    spaced-value   `                  -> name spaced-value   (trimmed)
//   `description:` with nothing after the colon -> description ""
//
// --- effective name: @meta name, else the filename stem -------------------
//   partial.lua with `name: aaa-partial` lists as aaa-partial
//   bare.lua with no @meta at all      lists as bare
//
// --- ordering: two INDEPENDENT sorts, concatenated -------------------------
// Adding a sandbox `aaa-local` to the fixture above:
//   aaa-partial  shipped
//   bare         shipped
//   finalize-closeout shipped
//   aaa-local    local        <-- FOURTH, not first: not a merged sort
//   mmm-sandbox  local
//
// --- name collision: BOTH listed, `show` resolves SHIPPED ------------------
//   cp $PLANAR_HOME/workflows/finalize_closeout.lua $PLANAR_HOME/local/workflows/
//   $Z workflow list  -> two finalize-closeout rows, shipped then local
//   $Z workflow show finalize-closeout --json -> "kind":"shipped"
//
// --- absent directories ----------------------------------------------------
//   PLANAR_HOME with no workflows/ at all:
//   $Z workflow list        -> exit 0, "no shipped + sandbox workflows found"
//   $Z workflow list --json -> exit 0, ZERO BYTES on stdout

#include <catch2/catch_test_macros.hpp>

import std;
import planar.engine.workflows.catalog;

namespace {

namespace cat = planar::engine::workflows::catalog;

/// @brief A per-test scratch tree, removed on destruction.
///
/// Everything this suite writes lives under `root_`, which is created fresh
/// beneath `temp_directory_path()`. Nothing outside it is ever written.
struct scratch_home {
  std::filesystem::path root_;

  scratch_home()
      : root_(std::filesystem::temp_directory_path() / std::format("planar_workflows_test_{}_{}",
                                                                   std::chrono::steady_clock::now().time_since_epoch().count(),
                                                                   reinterpret_cast<std::uintptr_t>(this))) {
    std::error_code ec;
    std::filesystem::create_directories(root_ / "workflows", ec);
    std::filesystem::create_directories(root_ / "local" / "workflows", ec);
  }

  scratch_home(const scratch_home&)            = delete;
  scratch_home& operator=(const scratch_home&) = delete;

  ~scratch_home() {
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
  }

  [[nodiscard]] auto shipped() const -> std::filesystem::path {
    return root_ / "workflows";
  }
  [[nodiscard]] auto sandbox() const -> std::filesystem::path {
    return root_ / "local" / "workflows";
  }
  [[nodiscard]] auto dirs() const -> cat::directories {
    return cat::directories{.shipped = shipped(), .sandbox = sandbox()};
  }

  auto write(const std::filesystem::path& dir, std::string_view name, std::string_view content) const -> void {
    std::ofstream out(dir / name, std::ios::binary);
    REQUIRE(out.good());
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
  }
};

/// @brief A synthetic environment. The ONLY environment this suite ever
/// hands to `resolve_dirs` -- `std::getenv` is never called.
auto env_of(std::map<std::string, std::string, std::less<>> vars) -> std::function<std::optional<std::string>(std::string_view)> {
  return [vars = std::move(vars)](std::string_view name) -> std::optional<std::string> {
    const auto it = vars.find(name);
    if (it == vars.end()) {
      return std::nullopt;
    }
    return it->second;
  };
}

constexpr std::string_view k_full_meta = "--[[ @meta\n"
                                         "name: finalize-closeout\n"
                                         "description: Deterministic closeout gate.\n"
                                         "phases: closeout\n"
                                         "seam: planar run start/event/finish, planar plan closeout\n"
                                         "--]]\n"
                                         "function closeout() end\n";

constexpr std::string_view k_no_meta = "-- plain comment, no @meta\nfunction run() end\n";

constexpr std::string_view k_partial_meta = "--[[ @meta\n"
                                            "name: aaa-partial\n"
                                            "description: Only two fields.\n"
                                            "unknown_key: ignored\n"
                                            "--]]\n";

constexpr std::string_view k_sandbox_meta = "--[[ @meta\n"
                                            "name: mmm-sandbox\n"
                                            "description: A sandbox workflow.\n"
                                            "phases: build,test\n"
                                            "--]]\n";

/// @brief Seed the exact fixture the oracle transcript above describes.
auto seed_oracle_fixture(const scratch_home& home) -> void {
  home.write(home.shipped(), "finalize_closeout.lua", k_full_meta);
  home.write(home.shipped(), "bare.lua", k_no_meta);
  home.write(home.shipped(), "partial.lua", k_partial_meta);
  home.write(home.shipped(), "notes.txt", "not lua\n");
  home.write(home.sandbox(), "sandbox_one.lua", k_sandbox_meta);
}

auto names_of(const std::vector<cat::entry>& entries) -> std::vector<std::string> {
  std::vector<std::string> out;
  out.reserve(entries.size());
  for (const auto& value : entries) {
    out.emplace_back(cat::effective_name(value));
  }
  return out;
}

} // namespace

TEST_CASE("workflows.catalog: a full @meta block yields all four fields", "[workflows]") {
  const auto parsed = cat::parse_meta(k_full_meta);
  REQUIRE(parsed.found);
  REQUIRE(parsed.meta.name == "finalize-closeout");
  REQUIRE(parsed.meta.description == "Deterministic closeout gate.");
  REQUIRE(parsed.meta.phases == "closeout");
  REQUIRE(parsed.meta.seam == "planar run start/event/finish, planar plan closeout");
}

TEST_CASE("workflows.catalog: an absent @meta block is not an error", "[workflows]") {
  const auto parsed = cat::parse_meta(k_no_meta);
  REQUIRE_FALSE(parsed.found);
  REQUIRE(parsed.meta.name.empty());
  REQUIRE(parsed.meta.description.empty());

  const auto empty = cat::parse_meta("");
  REQUIRE_FALSE(empty.found);

  // The opener must match EXACTLY after trimming, case included.
  REQUIRE_FALSE(cat::parse_meta("--[[ @META\nname: x\n--]]\n").found);
  REQUIRE_FALSE(cat::parse_meta("--[[@meta\nname: x\n--]]\n").found);
  REQUIRE_FALSE(cat::parse_meta("--[[ @meta extra\nname: x\n--]]\n").found);
}

TEST_CASE("workflows.catalog: the parser is far more tolerant than its docs claim", "[workflows]") {
  // The Zig module's header says the block "MUST start on line 1 ... (no
  // leading whitespace)". Both halves are false in the shipped code, and each
  // of these was confirmed by writing the file and running `workflow list`.
  // Pinning them keeps a future reader from "fixing" the parser back to the
  // comment and silently dropping metadata from real workflow files.

  // Leading BLANK lines: the tokenizer drops empty segments, so `--[[ @meta`
  // is still the first token. Oracle: name `leading-blanks`.
  const auto leading = cat::parse_meta("\n\n--[[ @meta\nname: leading-blanks\ndescription: D.\n--]]\n");
  REQUIRE(leading.found);
  REQUIRE(leading.meta.name == "leading-blanks");

  // Indented opener: each line is trimmed of " \t\r". Oracle: `indented-open`.
  const auto indented = cat::parse_meta("   --[[ @meta\nname: indented-open\ndescription: D.\n--]]\n");
  REQUIRE(indented.found);
  REQUIRE(indented.meta.name == "indented-open");

  // CRLF throughout: the '\r' is trimmed, so no newline normalization pass is
  // needed. Oracle: `crlf-name`.
  const auto crlf = cat::parse_meta("--[[ @meta\r\nname: crlf-name\r\ndescription: CRLF file.\r\n--]]\r\n");
  REQUIRE(crlf.found);
  REQUIRE(crlf.meta.name == "crlf-name");
  REQUIRE(crlf.meta.description == "CRLF file."); // NOT "CRLF file.\r"

  // Unclosed block: parses to EOF and still reports found. Oracle: `no-close`.
  const auto unclosed = cat::parse_meta("--[[ @meta\nname: no-close\ndescription: Block never closed.\n");
  REQUIRE(unclosed.found);
  REQUIRE(unclosed.meta.name == "no-close");
  REQUIRE(unclosed.meta.description == "Block never closed.");

  // Values are trimmed; a key with nothing after the colon yields empty.
  // Oracle: `spaced-value` with an empty description.
  const auto spaced = cat::parse_meta("--[[ @meta\n\nname:    spaced-value   \n\ndescription:\n--]]\n");
  REQUIRE(spaced.found);
  REQUIRE(spaced.meta.name == "spaced-value");
  REQUIRE(spaced.meta.description.empty());
}

TEST_CASE("workflows.catalog: unknown keys and colonless lines are skipped", "[workflows]") {
  const auto parsed = cat::parse_meta("--[[ @meta\n"
                                      "name: example\n"
                                      "unknown_key: ignored value\n"
                                      "a line with no colon at all\n"
                                      "description: A test workflow.\n"
                                      "--]]\n");
  REQUIRE(parsed.found);
  REQUIRE(parsed.meta.name == "example");
  REQUIRE(parsed.meta.description == "A test workflow.");
  REQUIRE(parsed.meta.phases.empty());
  REQUIRE(parsed.meta.seam.empty());
}

TEST_CASE("workflows.catalog: content after --]] is not parsed", "[workflows]") {
  // The closer ends the block. A `name:` line in the Lua body below it must
  // not be picked up -- otherwise ordinary Lua string content could rewrite a
  // workflow's identity.
  const auto parsed = cat::parse_meta("--[[ @meta\n"
                                      "name: real-name\n"
                                      "--]]\n"
                                      "local s = 'name: forged-name'\n"
                                      "description: not a field\n");
  REQUIRE(parsed.found);
  REQUIRE(parsed.meta.name == "real-name");
  REQUIRE(parsed.meta.description.empty());
}

TEST_CASE("workflows.catalog: the effective name prefers @meta over the filename", "[workflows]") {
  cat::entry named{.path       = "/x/partial.lua",
                   .filename   = "partial.lua",
                   .meta       = {.name = "aaa-partial"},
                   .meta_found = true,
                   .is_local   = false};
  REQUIRE(cat::effective_name(named) == "aaa-partial");

  cat::entry bare{.path = "/x/bare.lua", .filename = "bare.lua", .meta = {}, .meta_found = false, .is_local = false};
  REQUIRE(cat::effective_name(bare) == "bare");

  // A @meta block WITHOUT a name falls back to the stem too -- `meta_found`
  // alone is not enough.
  cat::entry nameless{
      .path = "/x/thing.lua", .filename = "thing.lua", .meta = {.description = "d"}, .meta_found = true, .is_local = false};
  REQUIRE(cat::effective_name(nameless) == "thing");

  // A filename without the extension is used whole.
  cat::entry odd{.path = "/x/README", .filename = "README", .meta = {}, .meta_found = false, .is_local = false};
  REQUIRE(cat::effective_name(odd) == "README");
}

TEST_CASE("workflows.catalog: scan ignores non-lua files and sorts by effective name", "[workflows]") {
  scratch_home home;
  seed_oracle_fixture(home);

  const auto shipped = cat::scan(home.shipped(), false);
  // notes.txt is skipped; the three .lua files are sorted by EFFECTIVE name,
  // so partial.lua sorts first as `aaa-partial` despite its filename.
  REQUIRE(names_of(shipped) == std::vector<std::string>{"aaa-partial", "bare", "finalize-closeout"});
  REQUIRE(shipped[0].filename == "partial.lua");
  REQUIRE(shipped[0].meta_found);
  REQUIRE(shipped[1].filename == "bare.lua");
  REQUIRE_FALSE(shipped[1].meta_found);
  REQUIRE(shipped[2].meta.phases == "closeout");
  for (const auto& value : shipped) {
    REQUIRE_FALSE(value.is_local);
    REQUIRE(value.path.starts_with(home.shipped().string()));
  }

  const auto sandbox = cat::scan(home.sandbox(), true);
  REQUIRE(names_of(sandbox) == std::vector<std::string>{"mmm-sandbox"});
  REQUIRE(sandbox[0].is_local);
}

TEST_CASE("workflows.catalog: the sort is by effective name, not by filename or disk order", "[workflows]") {
  // A BREAK-PROBE HARDENING. Deleting `std::ranges::sort` from `scan` was
  // caught by only ONE test, and only by luck: the other ordering assertions
  // used fixtures whose directory-iteration order happened to already match
  // the sorted order on this filesystem. That is not discrimination, it is a
  // coincidence that a different filesystem (or a different creation order)
  // would silently withdraw.
  //
  // This fixture removes the luck. Six files whose FILENAMES ascend (a..f)
  // carry @meta names that DESCEND (zzz..aaa), so filename order, creation
  // order, and sorted order are three different sequences and only a real
  // sort produces the expected one.
  scratch_home                                             home;
  const std::array<std::pair<const char*, const char*>, 6> files{{{"a.lua", "zzz-six"},
                                                                  {"b.lua", "yyy-five"},
                                                                  {"c.lua", "mmm-four"},
                                                                  {"d.lua", "ddd-three"},
                                                                  {"e.lua", "bbb-two"},
                                                                  {"f.lua", "aaa-one"}}};
  for (const auto& [filename, name] : files) {
    home.write(home.shipped(), filename, std::format("--[[ @meta\nname: {}\n--]]\n", name));
  }

  const auto scanned = cat::scan(home.shipped(), false);
  REQUIRE(names_of(scanned) == std::vector<std::string>{"aaa-one", "bbb-two", "ddd-three", "mmm-four", "yyy-five", "zzz-six"});
  // And the filenames are correspondingly REVERSED relative to the names --
  // proof the sort keyed on the effective name rather than on the path.
  REQUIRE(scanned.front().filename == "f.lua");
  REQUIRE(scanned.back().filename == "a.lua");
  REQUIRE(std::ranges::is_sorted(scanned, {}, [](const cat::entry& e) { return cat::effective_name(e); }));
}

TEST_CASE("workflows.catalog: an absent directory is an empty list, never an error", "[workflows]") {
  scratch_home home;
  REQUIRE(cat::scan(home.root_ / "does-not-exist", false).empty());
  // An existing but EMPTY directory is likewise empty.
  REQUIRE(cat::scan(home.shipped(), false).empty());
  // A path that is a FILE rather than a directory is also empty, not a throw.
  home.write(home.shipped(), "afile.lua", k_no_meta);
  REQUIRE(cat::scan(home.shipped() / "afile.lua", false).empty());
}

TEST_CASE("workflows.catalog: subdirectories are not descended into", "[workflows]") {
  scratch_home    home;
  std::error_code ec;
  std::filesystem::create_directories(home.shipped() / "nested", ec);
  home.write(home.shipped() / "nested", "deep.lua", k_full_meta);
  home.write(home.shipped(), "top.lua", k_no_meta);

  // The scan is one level deep: `nested/deep.lua` must not appear, and the
  // directory `nested` itself must not be mistaken for an entry.
  REQUIRE(names_of(cat::scan(home.shipped(), false)) == std::vector<std::string>{"top"});
}

TEST_CASE("workflows.catalog: list concatenates two INDEPENDENT sorts, shipped first", "[workflows]") {
  scratch_home home;
  seed_oracle_fixture(home);
  // A sandbox workflow that would sort FIRST under a single merged sort.
  home.write(home.sandbox(), "aaa_local.lua", "--[[ @meta\nname: aaa-local\ndescription: Sorts first overall.\n--]]\n");

  // Oracle-verified: aaa-local lists FOURTH, after every shipped entry.
  REQUIRE(names_of(cat::list(home.dirs(), false)) ==
          std::vector<std::string>{"aaa-partial", "bare", "finalize-closeout", "aaa-local", "mmm-sandbox"});

  // --local skips the shipped directory entirely.
  REQUIRE(names_of(cat::list(home.dirs(), true)) == std::vector<std::string>{"aaa-local", "mmm-sandbox"});
}

TEST_CASE("workflows.catalog: a colliding name is listed twice, shipped then local", "[workflows]") {
  scratch_home home;
  seed_oracle_fixture(home);
  home.write(home.sandbox(), "finalize_closeout.lua", k_full_meta);

  const auto listed = cat::list(home.dirs(), false);
  REQUIRE(names_of(listed) ==
          std::vector<std::string>{"aaa-partial", "bare", "finalize-closeout", "finalize-closeout", "mmm-sandbox"});
  // The two rows differ only in kind -- neither shadows the other in `list`.
  REQUIRE_FALSE(listed[2].is_local);
  REQUIRE(listed[3].is_local);

  // But `find` resolves SHIPPED. Oracle-verified:
  //   $Z workflow show finalize-closeout --json -> "kind":"shipped"
  // Worth pinning precisely because "sandbox overrides shipped" is the
  // intuitive guess and it is WRONG here.
  const auto found = cat::find(home.dirs(), "finalize-closeout");
  REQUIRE(found.has_value());
  REQUIRE_FALSE(found->is_local);
  REQUIRE(found->path.starts_with(home.shipped().string()));
}

TEST_CASE("workflows.catalog: find matches the effective name exactly", "[workflows]") {
  scratch_home home;
  seed_oracle_fixture(home);

  REQUIRE(cat::find(home.dirs(), "aaa-partial").has_value());
  REQUIRE(cat::find(home.dirs(), "bare").has_value());
  // A sandbox-only workflow is reachable through the same call.
  const auto sandbox = cat::find(home.dirs(), "mmm-sandbox");
  REQUIRE(sandbox.has_value());
  REQUIRE(sandbox->is_local);

  // No match, no partial match, no filename match.
  REQUIRE_FALSE(cat::find(home.dirs(), "nope").has_value());
  REQUIRE_FALSE(cat::find(home.dirs(), "aaa").has_value());
  REQUIRE_FALSE(cat::find(home.dirs(), "partial").has_value());     // the FILENAME stem
  REQUIRE_FALSE(cat::find(home.dirs(), "partial.lua").has_value()); // the filename
  REQUIRE_FALSE(cat::find(home.dirs(), "").has_value());
  REQUIRE_FALSE(cat::find(home.dirs(), "AAA-PARTIAL").has_value()); // case-sensitive
}

TEST_CASE("workflows.catalog: directory resolution follows the documented fallbacks", "[workflows]") {
  // Every lookup below is against a SYNTHETIC environment map. No test in
  // this file ever calls std::getenv, so none can resolve a real ~/.planar.
  const auto explicit_home = cat::resolve_dirs(env_of({{"PLANAR_HOME", "/scratch/ph"}}));
  REQUIRE(explicit_home.shipped == std::filesystem::path("/scratch/ph/workflows"));
  REQUIRE(explicit_home.sandbox == std::filesystem::path("/scratch/ph/local/workflows"));

  // PLANAR_HOME wins over HOME.
  const auto both = cat::resolve_dirs(env_of({{"PLANAR_HOME", "/scratch/ph"}, {"HOME", "/scratch/user"}}));
  REQUIRE(both.shipped == std::filesystem::path("/scratch/ph/workflows"));

  // HOME/.planar is the fallback.
  const auto from_home = cat::resolve_dirs(env_of({{"HOME", "/scratch/user"}}));
  REQUIRE(from_home.shipped == std::filesystem::path("/scratch/user/.planar/workflows"));
  REQUIRE(from_home.sandbox == std::filesystem::path("/scratch/user/.planar/local/workflows"));

  // With NOTHING set, /tmp/.planar -- an absolute, harmless location rather
  // than a relative path that would follow the cwd.
  const auto nothing = cat::resolve_dirs(env_of({}));
  REQUIRE(nothing.shipped == std::filesystem::path("/tmp/.planar/workflows"));
  REQUIRE(nothing.sandbox == std::filesystem::path("/tmp/.planar/local/workflows"));
  REQUIRE(cat::resolve_planar_home(env_of({})) == std::filesystem::path("/tmp/.planar"));

  // PLANAR_WORKFLOWS_DIR overrides ONLY the shipped directory; the sandbox
  // stays under PLANAR_HOME. Getting this wrong would silently relocate an
  // operator's sandbox.
  const auto overridden = cat::resolve_dirs(env_of({{"PLANAR_HOME", "/scratch/ph"}, {"PLANAR_WORKFLOWS_DIR", "/elsewhere/wf"}}));
  REQUIRE(overridden.shipped == std::filesystem::path("/elsewhere/wf"));
  REQUIRE(overridden.sandbox == std::filesystem::path("/scratch/ph/local/workflows"));
}

TEST_CASE("workflows.catalog: resolution never consults the real environment", "[workflows]") {
  // A direct assertion of the HOME-safety property: an empty synthetic
  // environment resolves to /tmp/.planar REGARDLESS of what the process's own
  // HOME or PLANAR_HOME happen to be. If this module fell back to getenv,
  // this test would resolve to the developer's real home and fail here rather
  // than silently writing there.
  REQUIRE(cat::resolve_dirs(env_of({})).shipped == std::filesystem::path("/tmp/.planar/workflows"));

  const auto* real_home = std::getenv("HOME"); // NOLINT(concurrency-mt-unsafe) -- read once, for an assertion only.
  if (real_home != nullptr && std::string_view{real_home} != "/tmp") {
    const auto resolved = cat::resolve_dirs(env_of({})).shipped.string();
    REQUIRE(resolved.find(real_home) == std::string::npos);
  }
}
