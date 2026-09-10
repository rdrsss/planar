// @file workflow.t.cpp
// @brief End-to-end behaviour of `run_workflow`: the run loop, the two
// marshalling directions, and the confinement each host group enforces
// (plan 996, task 6042).
//
// Every expected string here was produced by RUNNING
// zig/zig-out/bin/planar-execute over the same workflow and copying its
// bytes. The differential half — the same workflows run through BOTH
// binaries and diffed — lives in src/cmd/planar-execute/parity.t.cpp, which
// is where an oracle binary is available; this file pins the same contract
// where it can be checked without one.

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>

import std;
import planar.engine_execute;

namespace {

namespace ex = planar::engine::execute;

/// @brief What one `run_workflow` call produced.
struct outcome {
  ex::run_status status;
  std::string    out;
  std::string    err;
};

/// @brief Run a workflow with an otherwise-empty configuration.
/// @param source The Lua source.
/// @param phase The phase to call.
/// @param tweak Applied to the config before running, for the cases that
/// need a worktree, a sandbox root or `--args`.
/// @return The outcome.
template <typename Tweak> auto run(std::string_view source, std::string_view phase, Tweak tweak) -> outcome {
  ex::run_config config{.source = std::string{source}, .phase = std::string{phase}};
  tweak(config);
  std::ostringstream out;
  std::ostringstream err;
  auto const         status = ex::run_workflow(config, out, err);
  return outcome{.status = status, .out = out.str(), .err = err.str()};
}

/// @brief Run a workflow with a bare configuration.
/// @param source The Lua source.
/// @param phase The phase to call.
/// @return The outcome.
auto run(std::string_view source, std::string_view phase = "p") -> outcome {
  return run(source, phase, [](ex::run_config&) {});
}

/// @brief A scratch directory that removes itself.
class scratch {
public:
  scratch()
      : _path(std::filesystem::temp_directory_path() /
              std::format("planar_execute_wf_{}", std::chrono::steady_clock::now().time_since_epoch().count())) {
    std::error_code ec;
    std::filesystem::create_directories(_path, ec);
  }
  scratch(scratch const&)                    = delete;
  auto operator=(scratch const&) -> scratch& = delete;
  ~scratch() {
    std::error_code ec;
    std::filesystem::remove_all(_path, ec);
  }
  /// @brief The directory.
  /// @return Its path.
  [[nodiscard]] auto path() const -> std::filesystem::path const& {
    return _path;
  }

private:
  std::filesystem::path _path;
};

} // namespace

TEST_CASE("a phase that declares a result marshals it as JSON on stdout", "[engine][execute][workflow]") {
  auto const got = run("function p() flow.result({b = 2, a = 1}) end");
  CHECK(got.status == ex::run_status::ok);
  // Keys SORTED, and the trailing newline is part of the payload. The sort
  // is a determinism contract, not tidiness: Lua's traversal order over a
  // string-keyed table depends on the per-state hash seed, which
  // luaL_newstate derives from the clock and an ASLR address, so raw order
  // is not reproducible even for the same binary on the same input.
  CHECK(got.out == "{\"a\":1,\"b\":2}\n");
  CHECK(got.err.empty());
}

TEST_CASE("a phase that declares no result still emits JSON", "[engine][execute][workflow]") {
  auto const got = run("function p() end");
  CHECK(got.status == ex::run_status::ok);
  // `{}` rather than nothing, so a caller never has to special-case empty
  // output. This is also why the unported-engine path in the previous cycle
  // could NOT print `{}` and exit 0 — it is a real success shape.
  CHECK(got.out == "{}\n");
}

TEST_CASE("integers survive the round trip through the host", "[engine][execute][workflow]") {
  // The reason the JSON reader is hand-written instead of going through a
  // generic value type that stores every number as a double. 9007199254740993
  // is not representable as a double; a double intermediate returns
  // ...992 — a silently wrong id.
  auto const got =
      run("function p() flow.result({ n = ctx.args.n, t = math.type(ctx.args.n), big = ctx.args.big, f = ctx.args.f, "
          "tf = math.type(ctx.args.f) }) end",
          "p", [](ex::run_config& config) { config.args_json = R"({"n":5,"big":9007199254740993,"f":2.5})"; });
  CHECK(got.status == ex::run_status::ok);
  CHECK(got.out == "{\"big\":9007199254740993,\"f\":2.5,\"n\":5,\"t\":\"integer\",\"tf\":\"float\"}\n");
}

TEST_CASE("nested tables, arrays and strings marshal both ways", "[engine][execute][workflow]") {
  auto const got = run(R"(function p()
    flow.result({ list = {1, 2, 3}, nested = { inner = { deep = true } }, empty = {}, text = "a\"b\nc", echo = ctx.args.list })
  end)",
                       "p", [](ex::run_config& config) { config.args_json = R"({"list":["x","y"]})"; });
  CHECK(got.status == ex::run_status::ok);
  CHECK(got.out == "{\"echo\":[\"x\",\"y\"],\"empty\":{},\"list\":[1,2,3],\"nested\":{\"inner\":{\"deep\":true}},"
                   "\"text\":\"a\\\"b\\nc\"}\n");
}

TEST_CASE("a table the JSON model cannot express is refused rather than truncated", "[engine][execute][workflow]") {
  // A sparse array silently becoming a short array would be a wrong answer a
  // caller could not detect.
  auto const sparse = run("function p() local t = {} t[1] = 'a' t[3] = 'c' flow.result({v = t}) end");
  CHECK(sparse.status == ex::run_status::phase_failed);
  CHECK(sparse.err.contains("sparse Lua arrays are not supported"));

  auto const mixed = run("function p() local t = {'a'} t.key = 'b' flow.result({v = t}) end");
  CHECK(mixed.status == ex::run_status::phase_failed);
  CHECK(mixed.err.contains("mixed Lua tables cannot be serialized as JSON arrays"));

  auto const fn = run("function p() flow.result({v = print}) end");
  CHECK(fn.status == ex::run_status::phase_failed);
  CHECK(fn.err.contains("cannot serialize Lua function to JSON"));

  auto const not_a_table = run("function p() flow.result('nope') end");
  CHECK(not_a_table.status == ex::run_status::phase_failed);
  CHECK(not_a_table.err.contains("flow.result expects a table"));
}

TEST_CASE("flow.log writes to stderr so stdout stays a clean JSON channel", "[engine][execute][workflow]") {
  auto const got = run("function p() flow.log('hello') flow.result({}) end");
  CHECK(got.status == ex::run_status::ok);
  CHECK(got.out == "{}\n");
  CHECK(got.err == "[planar-execute] hello\n");
}

TEST_CASE("flow.fail reports its own message rather than the Lua error text", "[engine][execute][workflow]") {
  auto const got = run("function p() flow.fail('nope') end");
  CHECK(got.status == ex::run_status::phase_failed);
  // Oracle bytes. flow.fail raises as well as records, so the naive report
  // would be the Lua error — which carries a `workflow:1:` prefix and a
  // `flow.fail: ` marker the operator did not ask for.
  CHECK(got.err == "planar-execute: phase failed: nope\n");
  CHECK(got.out.empty());
}

TEST_CASE("a missing phase is distinguished from a failing one", "[engine][execute][workflow]") {
  auto const absent = run("function p() end", "zzz");
  CHECK(absent.status == ex::run_status::phase_missing);
  CHECK(absent.err == "planar-execute: phase function not found: zzz\n");

  // A global of that name that is NOT a function reports the same thing —
  // the oracle does not distinguish, and a workflow that assigned `p = 5` has
  // the same problem either way.
  auto const not_a_function = run("p = 5");
  CHECK(not_a_function.status == ex::run_status::phase_missing);
  CHECK(not_a_function.err == "planar-execute: phase function not found: p\n");
}

TEST_CASE("load and init failures are reported at their own stage", "[engine][execute][workflow]") {
  auto const syntax = run("this is not lua");
  CHECK(syntax.status == ex::run_status::load_failed);
  // `workflow:1:` — the chunk name is "@workflow", which is what puts that
  // prefix on every Lua diagnostic this binary emits.
  CHECK(syntax.err == "planar-execute: load error: workflow:1: syntax error near 'is'\n");

  auto const top_level = run("error('boom')");
  CHECK(top_level.status == ex::run_status::load_failed);
  CHECK(top_level.err == "planar-execute: init error: workflow:1: boom\n");

  auto const in_phase = run("function p() error('kaboom') end");
  CHECK(in_phase.status == ex::run_status::phase_failed);
  CHECK(in_phase.err == "planar-execute: phase error: workflow:1: kaboom\n");
}

TEST_CASE("ctx.now and ctx.seed default to zero", "[engine][execute][workflow]") {
  // With `os` absent these two are the ONLY clock and seed a workflow can
  // reach, so zero-by-default is what makes a run reproducible.
  auto const got = run("function p() flow.result({ now = ctx.now, seed = ctx.seed, args = ctx.args }) end");
  CHECK(got.out == "{\"args\":{},\"now\":0,\"seed\":0}\n");

  auto const injected = run("function p() flow.result({ now = ctx.now, seed = ctx.seed }) end", "p", [](ex::run_config& config) {
    config.now  = 1700000000;
    config.seed = 42;
  });
  CHECK(injected.out == "{\"now\":1700000000,\"seed\":42}\n");
}

TEST_CASE("cli and git refuse to run when their configuration is absent", "[engine][execute][workflow]") {
  // Not a fallback to PATH and not a fallback to cwd — both would silently
  // reach something the caller did not name.
  auto const no_bin_dir = run("function p() cli.planar({'schema'}) end");
  CHECK(no_bin_dir.status == ex::run_status::phase_failed);
  CHECK(no_bin_dir.err.contains("trusted binary directory is unavailable"));

  auto const no_worktree = run("function p() git.head_sha() end");
  CHECK(no_worktree.status == ex::run_status::phase_failed);
  CHECK(no_worktree.err.contains("git.* requires a configured worktree"));

  auto const no_sandbox = run("function p() fs.read('a.txt') end");
  CHECK(no_sandbox.status == ex::run_status::phase_failed);
  CHECK(no_sandbox.err.contains("fs.read rejected or failed: a.txt"));
}

TEST_CASE("the command allowlist is enforced from inside a workflow", "[engine][execute][workflow]") {
  // The pure predicate is covered in manifest.t.cpp; this checks it is
  // actually consulted on the live path, and BEFORE the binary directory is
  // resolved — a workflow gets "outside the capability set" rather than a
  // spawn error, even with no bin_dir configured.
  auto const denied = run("function p() cli.planar({'task', 'done', '1'}) end");
  CHECK(denied.status == ex::run_status::phase_failed);
  CHECK(denied.err.contains("command is outside the deterministic workflow capability set"));

  auto const bad_argv = run("function p() cli.planar('schema') end");
  CHECK(bad_argv.status == ex::run_status::phase_failed);
  CHECK(bad_argv.err.contains("expected an argv table (array of strings)"));

  auto const bad_element = run("function p() cli.planar({1}) end");
  CHECK(bad_element.status == ex::run_status::phase_failed);
  CHECK(bad_element.err.contains("argv element 1 is not a string"));
}

TEST_CASE("fs confinement holds against text escapes and symlinks alike", "[engine][execute][workflow]") {
  // The sandbox root is a SUBDIRECTORY of the scratch area, so `outside.txt`
  // beside it is a real file that `../outside.txt` would reach if the `..`
  // component were ever accepted. Aiming that case at a path that does not
  // exist either way would let it pass for the wrong reason — and a
  // break-probe that removed the `..` check confirmed the difference.
  scratch const   scratch_area;
  auto const      box = scratch_area.path() / "box";
  std::error_code ec;
  std::filesystem::create_directories(box / "sub", ec);
  {
    std::ofstream file(scratch_area.path() / "outside.txt", std::ios::binary);
    file << "forbidden";
  }
  {
    std::ofstream file(box / "inside.txt", std::ios::binary);
    file << "visible";
  }
  {
    std::ofstream file(box / "sub" / "deep.txt", std::ios::binary);
    file << "deep";
  }
  // A symlink pointing OUT of the sandbox, and a symlinked DIRECTORY. The
  // second is the one a text-only check misses: `dirlink/deep.txt` contains
  // no `..` and is not absolute.
  std::filesystem::create_symlink(scratch_area.path() / "outside.txt", box / "escape.txt", ec);
  std::filesystem::create_directory_symlink("sub", box / "dirlink", ec);

  auto const with_root = [&](ex::run_config& config) { config.sandbox_root = box.string(); };

  auto const reads = run(R"(local function try(f, ...)
      local ok, v = pcall(f, ...)
      if not ok then return "ERR" end
      return v
    end
    function p()
      flow.result({
        inside = try(fs.read, "inside.txt"),
        deep = try(fs.read, "sub/deep.txt"),
        parent = try(fs.read, "../outside.txt"),
        absolute = try(fs.read, "/etc/hosts"),
        dot = try(fs.read, "./inside.txt"),
        symlink = try(fs.read, "escape.txt"),
        through_dirlink = try(fs.read, "dirlink/deep.txt"),
      })
    end)",
                         "p", with_root);
  CHECK(reads.status == ex::run_status::ok);
  CHECK(reads.out == "{\"absolute\":\"ERR\",\"deep\":\"deep\",\"dot\":\"ERR\",\"inside\":\"visible\",\"parent\":\"ERR\","
                     "\"symlink\":\"ERR\",\"through_dirlink\":\"ERR\"}\n");

  // The symlink was not followed, and it still points where it did.
  CHECK(std::filesystem::is_symlink(box / "escape.txt"));

  auto const writes = run(R"(function p()
      fs.mkdir("made/deeper")
      fs.write("made/deeper/out.txt", "written")
      fs.write("inside.txt", "hi")
      flow.result({ back = fs.read("made/deeper/out.txt"), truncated = fs.read("inside.txt"),
                    yes = fs.exists("inside.txt"), no = fs.exists("absent.txt"), link = fs.exists("escape.txt") })
    end)",
                          "p", with_root);
  CHECK(writes.status == ex::run_status::ok);
  // `truncated` is "hi", not "hivisible": a shorter write over a longer file
  // replaces it rather than overwriting its prefix.
  CHECK(writes.out == "{\"back\":\"written\",\"link\":false,\"no\":false,\"truncated\":\"hi\",\"yes\":true}\n");
  CHECK(std::filesystem::exists(box / "made" / "deeper" / "out.txt"));

  // fs.write must not follow the symlink either. The target is a file this
  // process CAN write, so a passing check here really does prove the write
  // was refused rather than merely blocked by permissions.
  auto const through_link = run("function p() fs.write('escape.txt', 'nope') end", "p", with_root);
  CHECK(through_link.status == ex::run_status::phase_failed);
  CHECK(through_link.err.contains("fs.write rejected or failed: escape.txt"));
  std::ifstream      target(scratch_area.path() / "outside.txt", std::ios::binary);
  std::ostringstream target_bytes;
  target_bytes << target.rdbuf();
  CHECK(target_bytes.str() == "forbidden");

  // And fs.mkdir does not accept a pre-existing symlink as "already there".
  auto const over_link = run("function p() fs.mkdir('dirlink') end", "p", with_root);
  CHECK(over_link.status == ex::run_status::phase_failed);
  CHECK(over_link.err.contains("fs.mkdir rejected symlink: dirlink"));
}

TEST_CASE("ctx.brief maps a missing plan/task to 'not found', matching the oracle's StateError catch",
          "[engine][execute][workflow]") {
  // task 6125 landed the body: this used to be the one declared divergence
  // ("the brief compiler is not ported yet"). With no bin_dir configured,
  // `run_allowlisted`'s own "trusted binary directory is unavailable" is
  // exactly the shell failure host.zig's `state.planShow(...) catch
  // raiseError(L, "ctx.brief: plan {d} not found", ...)` collapses into
  // "not found" too — so this pins the SAME oracle-shaped message the real
  // shell-and-parse failure would produce, without needing a live `planar`.
  auto const got = run("function p() ctx.brief({plan_id = 1, task_id = 1, claim_token = 'x', problem_statement = 'y'}) end");
  CHECK(got.status == ex::run_status::phase_failed);
  CHECK(got.err.contains("ctx.brief: plan 1 not found"));

  // And the field really is a function, so the message above came from
  // calling it rather than from `ctx` having no such key.
  auto const shape = run("function p() flow.result({t = type(ctx.brief)}) end");
  CHECK(shape.out == "{\"t\":\"function\"}\n");
}

namespace {

/// @brief Write an executable POSIX shell script at `path`.
/// @param path The destination.
/// @param body The script body (a `#!/bin/sh` line is prepended).
auto write_script(std::filesystem::path const& path, std::string_view body) -> void {
  {
    std::ofstream file(path, std::ios::binary);
    file << "#!/bin/sh\n" << body;
  }
  std::filesystem::permissions(path, std::filesystem::perms::owner_all | std::filesystem::perms::group_read |
                                         std::filesystem::perms::group_exec | std::filesystem::perms::others_read |
                                         std::filesystem::perms::others_exec);
}

} // namespace

TEST_CASE("ctx.brief compiles a real brief from a fake planar/planar-agent bin_dir", "[engine][execute][workflow]") {
  // Fakes the two binaries `run_allowlisted` shells (`planar` for
  // plan/task/packet reads, `planar-agent` for the schema catalog) with
  // fixed-argv shell scripts, so the whole `state`/`schema`/`brief` pipeline
  // runs deterministically without a live database.
  scratch const bin_dir;

  write_script(bin_dir.path() / "planar", R"(case "$1 $2" in
  "plan show")
    echo '{"id":7,"title":"Fake Plan","status":"active","slug":"fake-plan","parent_plan_id":null}'
    ;;
  "task show")
    echo '{"id":9,"slug":"fake-task","status":"todo"}'
    ;;
  "task packet")
    echo '{"input":{"task_id":9,"status":"todo","title":"Authoritative title","body":"body text","next_action":"do the thing","acceptance_criteria":"it works","owning_plans":[{"kind":"plan","id":7,"locator":"plan:7","text":"Fake Plan","source_digest":"d","current_digest":"d","required":true,"covered":true,"status":"current","provenance":"db"}],"anchor_plans":[],"citations":[{"kind":"spec","id":1,"locator":"tech-spec.md #Brief","text":"cite me","source_digest":"d","current_digest":"d","required":true,"covered":true,"status":"current","provenance":"workbench"}],"decisions":[],"questions":[],"scenarios":[],"dependencies":[],"touches":[],"claims":[{"kind":"claim","id":3,"locator":"claim:3","text":"tok-123","source_digest":"d","current_digest":"d","required":true,"covered":true,"status":"active","provenance":"db"}],"validation_gates":[{"kind":"gate","id":1,"locator":"gate:1","text":"make test","source_digest":"d","current_digest":"d","required":true,"covered":true,"status":"current","provenance":"methodology"}],"facts":[]},"digest":"packet-digest-abc","reasons":[]}'
    ;;
esac
)");

  write_script(bin_dir.path() / "planar-agent", R"(if [ "$1" = "schema" ]; then
  echo '{"schemaVersion":1,"layout":"flat","root":"planar-agent","commands":[{"name":"planar-agent","command":"planar-agent","subcommands":["complete"],"flags":[],"hidden":false},{"name":"complete","command":"planar-agent complete","subcommands":[],"flags":[{"long":"--claim","required":true,"description":"Claim token"}],"hidden":false},{"name":"internal","command":"planar-agent internal","subcommands":[],"flags":[],"hidden":true}]}'
fi
)");

  auto const got = run("function p() flow.result({b = ctx.brief({plan_id = 7, task_id = 9, claim_token = 'tok-123', "
                       "problem_statement = 'unused because the packet is authoritative'})}) end",
                       "p", [&](ex::run_config& config) { config.bin_dir = bin_dir.path().string(); });
  CHECK(got.status == ex::run_status::ok);
  CHECK(got.out.contains("# Coder Brief"));
  CHECK(got.out.contains("**Plan:** Fake Plan (id 7)"));
  CHECK(got.out.contains("task:9 — Authoritative title"));
  CHECK(got.out.contains("**Claim token:** `tok-123`"));
  CHECK(got.out.contains("**Authoritative packet digest:** `packet-digest-abc`"));
  CHECK(got.out.contains("tech-spec.md #Brief"));
  CHECK(got.out.contains("planar-agent complete"));
  // The hidden command must not appear.
  CHECK(!got.out.contains("planar-agent internal"));
  CHECK(got.out.contains("make test"));
  CHECK(got.out.contains("## Terminal verb"));

  // A claim token that does not match the packet's active claim is the
  // AuthoritativeIdentityMismatch path.
  auto const mismatched =
      run("function p() ctx.brief({plan_id = 7, task_id = 9, claim_token = 'wrong-token', problem_statement = 'x'}) end", "p",
          [&](ex::run_config& config) { config.bin_dir = bin_dir.path().string(); });
  CHECK(mismatched.status == ex::run_status::phase_failed);
  CHECK(mismatched.err.contains("ctx.brief: caller plan/task/claim does not match authoritative packet"));
}

TEST_CASE("ctx.context returns an empty table, matching the oracle", "[engine][execute][workflow]") {
  // Not a stub: the engine has no run binding (one clean process per phase,
  // no scheduler), so there is no run id to read context for. The oracle
  // returns `{}` here too.
  auto const got = run("function p() flow.result({c = ctx.context()}) end");
  CHECK(got.out == "{\"c\":{}}\n");
}

namespace {

/// @brief Write an executable `/bin/sh` script.
/// @param at Where to write it.
/// @param body The script body, following the shebang line.
void write_executable(std::filesystem::path const& at, std::string_view body) {
  {
    std::ofstream script(at, std::ios::binary);
    script << "#!/bin/sh\n" << body;
  }
  std::filesystem::permissions(at, std::filesystem::perms::owner_all, std::filesystem::perm_options::add);
}

} // namespace

TEST_CASE("git.* resolves through PATH while cli.* stays beside the binary", "[engine][execute][workflow]") {
  // The oracle spawns the BARE NAME `git` and lets Zig's std.process.Child
  // search PATH (planar-execute/host.zig runGit, argv[0] = "git"). This port
  // probed /usr/bin/git, then /usr/local/bin/git, then fell back to a
  // bare-name execv -- which does NOT search PATH. On this machine the first
  // probe hit, so every differential case agreed; a machine carrying git only
  // in e.g. /opt/homebrew/bin failed outright (task 6126).
  //
  // The two halves below are deliberately one test. The fix has to move git
  // onto a PATH search WITHOUT moving cli.* there: cli.* resolving beside
  // this executable is what stops an unrelated `planar` earlier on PATH from
  // answering for the one that shipped with this planar-execute, and that
  // sibling resolution is part of planar-execute's contract.
  //
  // Mutating PATH is safe because catch_discover_tests runs each TEST_CASE as
  // its own process.
  scratch fake_bin;

  // A `git` reachable ONLY through PATH -- it is at neither absolute path the
  // old code probed. It answers `rev-parse HEAD` with a sentinel no real
  // repository produces, so a real git answering instead is visible.
  write_executable(fake_bin.path() / "git", "echo 0000000000000000000000000000000000000000\n");

  // A `planar` on that same directory. If cli.* ever starts searching PATH,
  // this is what it finds.
  write_executable(fake_bin.path() / "planar", "echo PATH_PLANAR_MUST_NOT_ANSWER\n");

  char const* const original = std::getenv("PATH");
  std::string const saved    = original == nullptr ? std::string{} : std::string{original};
  REQUIRE(::setenv("PATH", fake_bin.path().string().c_str(), 1) == 0);

  // git.* finds the PATH-only git.
  auto const via_path = run("function p() flow.result({sha = git.head_sha()}) end", "p",
                            [&](ex::run_config& config) { config.worktree = fake_bin.path().string(); });

  // cli.* does not. Pointed at a binary directory holding no `planar`, it
  // fails to spawn rather than reaching the `planar` sitting on PATH.
  scratch    empty_bin_dir;
  auto const via_sibling = run("function p() flow.result({o = cli.planar({'schema'})}) end", "p",
                               [&](ex::run_config& config) { config.bin_dir = empty_bin_dir.path().string(); });

  // Restore before asserting, so a failing CHECK does not leave Catch2's own
  // teardown running under a doctored PATH.
  if (saved.empty()) {
    ::unsetenv("PATH");
  } else {
    ::setenv("PATH", saved.c_str(), 1);
  }

  CHECK(via_path.status == ex::run_status::ok);
  CHECK(via_path.out == "{\"sha\":\"0000000000000000000000000000000000000000\"}\n");

  CHECK(via_sibling.status == ex::run_status::phase_failed);
  CHECK_FALSE(via_sibling.out.contains("PATH_PLANAR_MUST_NOT_ANSWER"));
  CHECK_FALSE(via_sibling.err.contains("PATH_PLANAR_MUST_NOT_ANSWER"));
}
