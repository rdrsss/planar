/// @file main.cpp
/// @brief `cli_docs_coverage` — a REGROWTH GUARD against undocumented CLI
/// leaves (plan 996, task 6758).
///
/// `cli_usage_lint` checks that every `--flag` an authored surface
/// references actually exists on the binary's `schema` catalog. It does
/// NOT check the converse — that every catalog leaf is mentioned anywhere
/// in `docs/cli-reference.md` — so that gap is invisible to every existing
/// gate and grows silently with each new verb.
///
/// This tool closes that blind spot the same way `scripts/coverage-check.sh`
/// closes the leaf-EXERCISE-coverage one: against a checked-in baseline
/// (`scripts/cli-docs-baseline.txt`) of known-undocumented leaves, rather
/// than requiring the full backlog (61 leaves, measured 2026-09-12) to be
/// authored before the gate can exist at all. A leaf already on the
/// baseline is a known, tracked gap; a leaf undocumented and NOT on the
/// baseline is a genuinely NEW omission, and that is what fails the build.
/// Documenting a baseline leaf never requires touching this tool or the
/// baseline file — the leaf simply stops appearing in the "still
/// undocumented" set the next run computes; the stale baseline line is
/// harmless until someone tidies it.
///
/// Usage:
///   cli_docs_coverage <repo-root> <bin-path>
///
/// Exit codes: 0 = no new gap (equal to or narrower than the baseline);
/// 1 = at least one leaf is undocumented and not on the baseline; 2 = usage
/// or schema-loading error.
///
/// Deliberately narrow: ONE binary (`planar`, the binary `docs/cli-reference.md`
/// documents), not the four `cli_usage_lint` loads — the other three
/// binaries have no reference doc of this shape to check leaves against.

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

import std;
import planar.json_dom;

namespace {

namespace fs = std::filesystem;
using planar::json_dom::json_kind;
using planar::json_dom::json_value;
using planar::json_dom::parse_json;

// ---------------------------------------------------------------------------
// Subprocess: run `<bin> schema` with an isolated PLANAR_DB.
//
// Identical in shape to cli_usage_lint's `run_schema_subprocess` — not
// shared, because this tool is a standalone executable (no first-party
// header tree to put a shared helper in, and modules-only forbids a header)
// and the two tools' schema-loading needs are otherwise unrelated (this one
// needs only leaf PATHS, not per-command flag sets).
// ---------------------------------------------------------------------------

struct subprocess_result {
  std::string stdout_text;
  bool        exited_zero = false;
};

/// @brief Run `bin schema`, capturing stdout, with `PLANAR_DB` pointed at a
/// scratch path so the subprocess never touches the operator's real
/// database (see CLAUDE.md, "Never run a from-source binary against the
/// real database").
auto run_schema_subprocess(std::string const& bin_path) -> subprocess_result {
  std::random_device rd;
  std::string        hex;
  {
    std::uniform_int_distribution<int> dist(0, 15);
    for (int i = 0; i < 16; ++i) {
      static constexpr char digits[] = "0123456789abcdef";
      hex.push_back(digits[dist(rd)]);
    }
  }
  fs::path const tmp_root = [] {
    if (char const* t = std::getenv("TMPDIR"))
      return fs::path{t};
    if (char const* t = std::getenv("TEMP"))
      return fs::path{t};
    if (char const* t = std::getenv("TMP"))
      return fs::path{t};
    return fs::path{"."};
  }();
  fs::path const db_path = tmp_root / std::format("planar-cli-docs-coverage-{}.db", hex);

  int pipe_fds[2];
  if (pipe(pipe_fds) != 0)
    return {};

  pid_t const pid = fork();
  if (pid < 0) {
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    return {};
  }
  if (pid == 0) {
    close(pipe_fds[0]);
    dup2(pipe_fds[1], STDOUT_FILENO);
    close(pipe_fds[1]);
    int const devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      dup2(devnull, STDERR_FILENO);
      close(devnull);
    }
    setenv("PLANAR_DB", db_path.c_str(), 1);
    execlp(bin_path.c_str(), bin_path.c_str(), "schema", static_cast<char*>(nullptr));
    _exit(127);
  }

  close(pipe_fds[1]);
  std::string out;
  char        buf[4096];
  ssize_t     n;
  while ((n = read(pipe_fds[0], buf, sizeof(buf))) > 0)
    out.append(buf, static_cast<std::size_t>(n));
  close(pipe_fds[0]);

  int status = 0;
  waitpid(pid, &status, 0);

  std::error_code ec;
  fs::remove(db_path, ec);
  fs::remove(fs::path{db_path.string() + "-wal"}, ec);
  fs::remove(fs::path{db_path.string() + "-shm"}, ec);

  return {.stdout_text = std::move(out), .exited_zero = WIFEXITED(status) && WEXITSTATUS(status) == 0};
}

// ---------------------------------------------------------------------------
// Leaf extraction.
// ---------------------------------------------------------------------------

/// @brief Recursively collect every leaf command's full path string (e.g.
/// `"planar task edit"`) from a `schema` catalog's `commands` array.
/// @param commands The catalog's top-level `commands` JSON array.
/// @param out Leaf path strings, appended to.
auto collect_leaves(json_value const& commands, std::vector<std::string>& out) -> void {
  if (commands.kind != json_kind::array)
    return;
  for (auto const& cmd : commands.array) {
    if (cmd.kind != json_kind::object)
      continue;
    auto const* path    = cmd.find("path");
    auto const* subs    = cmd.find("subcommands");
    bool const  is_leaf = path != nullptr && path->kind == json_kind::array && !path->array.empty() &&
                          (subs == nullptr || subs->kind != json_kind::array || subs->array.empty());
    if (is_leaf) {
      std::string full = "planar";
      for (auto const& seg : path->array) {
        if (seg.kind == json_kind::string) {
          full += ' ';
          full += seg.string;
        }
      }
      out.push_back(std::move(full));
    }
  }
}

/// @brief Load every leaf command path from `bin_path`'s `schema` output.
/// @param bin_path Path to the binary to shell.
/// @param out Leaf path strings, appended to.
/// @return False on a subprocess or parse failure.
auto load_leaves(std::string const& bin_path, std::vector<std::string>& out) -> bool {
  auto const [stdout_text, ok] = run_schema_subprocess(bin_path);
  if (!ok || stdout_text.empty())
    return false;
  auto parsed = parse_json(stdout_text);
  if (!parsed.has_value())
    return false;
  auto const* commands = parsed->find("commands");
  if (commands == nullptr)
    return false;
  collect_leaves(*commands, out);
  return true;
}

// ---------------------------------------------------------------------------
// Baseline file.
// ---------------------------------------------------------------------------

/// @brief Read `scripts/cli-docs-baseline.txt`: one leaf path per
/// non-comment, non-blank line.
/// @param path Path to the baseline file.
/// @return The known-undocumented leaf set, or empty if the file is absent
/// (a fresh repo with no baseline yet — every gap is then reported as new).
auto load_baseline(fs::path const& path) -> std::set<std::string> {
  std::set<std::string> out;
  std::ifstream         in(path);
  if (!in)
    return out;
  std::string line;
  while (std::getline(in, line)) {
    auto const hash  = line.find('#');
    auto       body  = hash == std::string::npos ? line : line.substr(0, hash);
    auto const first = body.find_first_not_of(" \t\r");
    if (first == std::string::npos)
      continue;
    auto const last = body.find_last_not_of(" \t\r");
    out.insert(body.substr(first, last - first + 1));
  }
  return out;
}

} // namespace

/// @brief Entry point: fail only on a leaf undocumented AND absent from the
/// checked-in baseline.
/// @param argc Argument count; must be 3 (`<repo-root> <bin-path>`).
/// @param argv Argument vector.
/// @return 0 clean, 1 new gap found, 2 usage/schema error.
auto main(int argc, char** argv) -> int {
  if (argc != 3) {
    std::println(stderr, "usage: {} <repo-root> <bin-path>", argc > 0 ? argv[0] : "cli_docs_coverage");
    return 2;
  }
  std::string const repo_root = argv[1];
  std::string const bin_path  = argv[2];

  std::vector<std::string> leaves;
  if (!load_leaves(bin_path, leaves)) {
    std::println(stderr, "error: failed to load schema from {}", bin_path);
    return 2;
  }

  auto const    doc_path = fs::path{repo_root} / "docs" / "cli-reference.md";
  std::ifstream doc_in(doc_path, std::ios::binary);
  if (!doc_in) {
    std::println(stderr, "error: cannot read {}", doc_path.string());
    return 2;
  }
  std::string const doc_text((std::istreambuf_iterator<char>(doc_in)), std::istreambuf_iterator<char>());

  std::vector<std::string> undocumented;
  for (auto const& leaf : leaves) {
    if (doc_text.find(leaf) == std::string::npos)
      undocumented.push_back(leaf);
  }
  std::ranges::sort(undocumented);

  auto const baseline = load_baseline(fs::path{repo_root} / "scripts" / "cli-docs-baseline.txt");

  std::vector<std::string> new_gaps;
  for (auto const& leaf : undocumented) {
    if (!baseline.contains(leaf))
      new_gaps.push_back(leaf);
  }

  if (new_gaps.empty()) {
    std::println("cli-docs-coverage: clean ({} leaves, {} known-undocumented on baseline, 0 new)", leaves.size(),
                 undocumented.size());
    return 0;
  }

  for (auto const& leaf : new_gaps) {
    std::println("`{}` is not mentioned anywhere in docs/cli-reference.md, and is not on "
                 "scripts/cli-docs-baseline.txt",
                 leaf);
  }
  std::println("cli-docs-coverage: {} new undocumented leaf(-ves) not on the baseline "
               "(add docs, or if this is a deliberate known gap, add it to scripts/cli-docs-baseline.txt)",
               new_gaps.size());
  return 1;
}
